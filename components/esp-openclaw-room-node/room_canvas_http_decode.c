#include "room_canvas_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "src/draw/lv_image_decoder_private.h"

#define TAG "room_canvas_http"
#define ROOM_CANVAS_MAX_IMAGE_BYTES (2 * 1024 * 1024)
#define ROOM_CANVAS_MAX_IMAGE_SIDE_PX 2048
#define ROOM_CANVAS_MAX_IMAGE_PIXELS (1024 * 1024)
#define ROOM_CANVAS_HTTP_TIMEOUT_MS 10000

static const char *HTML_GUIDANCE =
    "this canvas renders images and A2UI only; render HTML to a PNG/JPEG (e.g. browser screenshot) and present that URL, or use canvas.a2ui.pushJSONL";
static const char *SVG_GUIDANCE =
    "SVG is not supported on this canvas; rasterize to PNG/JPEG first, or use canvas.a2ui.pushJSONL";

typedef struct {
    uint8_t *data;
    size_t data_len;
    size_t capacity;
    char *content_type;
    bool too_large;
    bool no_memory;
    bool timed_out;
    int64_t deadline_us;
} room_canvas_http_response_t;

static bool content_type_is(const char *content_type, const char *expected)
{
    if (content_type == NULL) {
        return false;
    }
    size_t expected_len = strlen(expected);
    return strncasecmp(content_type, expected, expected_len) == 0 &&
           (content_type[expected_len] == '\0' ||
            content_type[expected_len] == ';' ||
            isspace((unsigned char)content_type[expected_len]));
}

static bool response_reserve(room_canvas_http_response_t *response, size_t required)
{
    if (required > ROOM_CANVAS_MAX_IMAGE_BYTES) {
        response->too_large = true;
        return false;
    }
    if (required <= response->capacity) {
        return true;
    }

    /* required <= the fetch bound here, so doubling clamped to the bound always
     * reaches it. */
    size_t capacity = response->capacity > 0 ? response->capacity : 16384;
    while (capacity < required) {
        capacity *= 2;
    }
    if (capacity > ROOM_CANVAS_MAX_IMAGE_BYTES) {
        capacity = ROOM_CANVAS_MAX_IMAGE_BYTES;
    }

    uint8_t *grown = room_canvas_large_alloc(capacity);
    if (grown == NULL) {
        response->no_memory = true;
        return false;
    }
    if (response->data_len > 0) {
        memcpy(grown, response->data, response->data_len);
    }
    heap_caps_free(response->data);
    response->data = grown;
    response->capacity = capacity;
    return true;
}

static void discard_response(room_canvas_http_response_t *response)
{
    heap_caps_free(response->data);
    free(response->content_type);
}

/*
 * esp_http_client discards event-handler return values (http_on_body ignores
 * the dispatch result), so refusing a download must close the transport;
 * perform() then errors out instead of pulling the rest of the body.
 */
static esp_err_t abort_http_fetch(esp_http_client_event_t *event, esp_err_t err)
{
    esp_http_client_close(event->client);
    return err;
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    room_canvas_http_response_t *response = event->user_data;
    if (response == NULL) {
        return ESP_FAIL;
    }
    /* timeout_ms bounds one socket operation; the wall-clock deadline also
     * bounds connect, header, and data progress across the whole request. */
    bool check_deadline = event->event_id == HTTP_EVENT_ON_CONNECTED ||
        event->event_id == HTTP_EVENT_ON_HEADER ||
        (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0);
    if (check_deadline && response->deadline_us != 0 &&
        esp_timer_get_time() > response->deadline_us) {
        response->timed_out = true;
        return abort_http_fetch(event, ESP_ERR_TIMEOUT);
    }
    if (event->event_id == HTTP_EVENT_REDIRECT) {
        response->data_len = 0;
        free(response->content_type);
        response->content_type = NULL;
    } else if (event->event_id == HTTP_EVENT_ON_HEADER &&
               event->header_key != NULL && event->header_value != NULL &&
               strcasecmp(event->header_key, "Content-Type") == 0) {
        char *copy = strdup(event->header_value);
        if (copy == NULL) {
            response->no_memory = true;
            return abort_http_fetch(event, ESP_ERR_NO_MEM);
        }
        free(response->content_type);
        response->content_type = copy;
    } else if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        if (response->capacity == 0) {
            /* Pre-size from Content-Length: skips the doubling-and-copy growth
             * and rejects an oversized download before pulling all its bytes.
             * Compare in 64-bit; size_t is 32-bit here. */
            int64_t content_length = esp_http_client_get_content_length(event->client);
            if (content_length > (int64_t)ROOM_CANVAS_MAX_IMAGE_BYTES) {
                response->too_large = true;
                return abort_http_fetch(event, ESP_ERR_INVALID_SIZE);
            }
            if (content_length > 0 &&
                !response_reserve(response, (size_t)content_length)) {
                return abort_http_fetch(
                    event,
                    response->too_large ? ESP_ERR_INVALID_SIZE : ESP_ERR_NO_MEM);
            }
        }
        size_t required = response->data_len + (size_t)event->data_len;
        if (!response_reserve(response, required)) {
            return abort_http_fetch(
                event,
                response->too_large ? ESP_ERR_INVALID_SIZE : ESP_ERR_NO_MEM);
        }
        memcpy(response->data + response->data_len, event->data, (size_t)event->data_len);
        response->data_len = required;
    }
    return ESP_OK;
}

static char *resolve_canvas_url(
    const char *url,
    esp_openclaw_node_error_t *out_error)
{
    /*
     * Absolute URLs are fetched as given, matching the canvas contract every
     * other node implements (Mac/iOS/Android/Linux load the presented URL in a
     * WebView). The caller is an authenticated gateway invoke, so this adds no
     * authority beyond what that caller already has; accepted deliberately
     * rather than making this node the one canvas that cannot show a URL.
     */
    if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) {
        char *copy = strdup(url);
        if (copy == NULL) {
            room_canvas_set_error(out_error, "INTERNAL", "not enough memory to copy the canvas URL", ESP_ERR_NO_MEM);
        }
        return copy;
    }
    if (url[0] != '/') {
        room_canvas_set_error(
            out_error,
            "FETCH_FAILED",
            "pass an absolute HTTP(S) URL reachable from the device",
            ESP_FAIL);
        return NULL;
    }
    if (room_canvas_node == NULL || room_canvas_gateway_http_base == NULL) {
        room_canvas_set_error(
            out_error,
            "FETCH_FAILED",
            "no canvas surface URL is available; pass an absolute URL reachable from the device",
            ESP_FAIL);
        return NULL;
    }

    /*
     * Deliberate origin swap: keep the surface URL's path and query (the
     * capability token rides in the path) but fetch through the node's own
     * gateway endpoint. Loopback-bound or tunneled gateways advertise origins
     * this device cannot reach; the transport host is the proven-reachable one.
     */
    char *surface_url = esp_openclaw_node_dup_plugin_surface_url(room_canvas_node, "canvas");
    if (surface_url == NULL) {
        room_canvas_set_error(
            out_error,
            "FETCH_FAILED",
            "no canvas surface URL is available; pass an absolute URL reachable from the device",
            ESP_FAIL);
        return NULL;
    }
    const char *scheme = strstr(surface_url, "://");
    const char *surface_path = scheme != NULL ? strchr(scheme + 3, '/') : NULL;
    if (surface_path == NULL) {
        free(surface_url);
        room_canvas_set_error(
            out_error,
            "FETCH_FAILED",
            "the canvas surface URL is invalid; pass an absolute URL reachable from the device",
            ESP_FAIL);
        return NULL;
    }

    /* The gateway mints capability-scoped surface URLs with the token in the
     * PATH, so only the path prefix carries authentication; any query/fragment
     * on it belongs to the surface, not to this request. */
    const char *surface_path_end = strpbrk(surface_path, "?#");
    size_t surface_path_len = surface_path_end != NULL
        ? (size_t)(surface_path_end - surface_path)
        : strlen(surface_path);
    size_t required = strlen(room_canvas_gateway_http_base) + surface_path_len + strlen(url) + 1;
    char *resolved = malloc(required);
    if (resolved == NULL) {
        free(surface_url);
        room_canvas_set_error(out_error, "INTERNAL", "not enough memory to resolve the canvas URL", ESP_ERR_NO_MEM);
        return NULL;
    }
    snprintf(
        resolved,
        required,
        "%s%.*s%s",
        room_canvas_gateway_http_base,
        (int)surface_path_len,
        surface_path,
        url);

    free(surface_url);
    return resolved;
}

esp_err_t room_canvas_fetch_image(
    const char *url,
    room_canvas_image_t *image,
    esp_openclaw_node_error_t *out_error)
{
    memset(image, 0, sizeof(*image));
    char *resolved = resolve_canvas_url(url, out_error);
    if (resolved == NULL) {
        return out_error->code != NULL && strcmp(out_error->code, "INTERNAL") == 0
            ? ESP_ERR_NO_MEM
            : ESP_FAIL;
    }

    int64_t fetch_start_us = esp_timer_get_time();
    room_canvas_http_response_t response = {
        .deadline_us = fetch_start_us + (int64_t)ROOM_CANVAS_HTTP_TIMEOUT_MS * 1000,
    };
    esp_http_client_config_t config = {
        .url = resolved,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event,
        .user_data = &response,
        .timeout_ms = ROOM_CANVAS_HTTP_TIMEOUT_MS,
        .disable_auto_redirect = false,
        .max_redirection_count = 5,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    free(resolved);
    if (client == NULL) {
        return room_canvas_set_error(
            out_error,
            "FETCH_FAILED",
            "could not start the HTTP request; verify the URL is reachable from the device",
            ESP_FAIL);
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (response.too_large) {
        discard_response(&response);
        return room_canvas_set_error(
            out_error,
            "INVALID_PARAMS",
            "canvas image exceeds the 2097152-byte fetch bound",
            ESP_ERR_INVALID_SIZE);
    }
    if (response.timed_out) {
        discard_response(&response);
        return room_canvas_set_error(
            out_error,
            "FETCH_FAILED",
            "image download exceeded the 10-second fetch deadline; serve a smaller image or a faster host",
            ESP_ERR_TIMEOUT);
    }
    if (response.no_memory) {
        discard_response(&response);
        return room_canvas_set_error(
            out_error,
            "INTERNAL",
            "not enough memory to receive the canvas image",
            ESP_ERR_NO_MEM);
    }
    if (err != ESP_OK || status < 200 || status >= 300) {
        discard_response(&response);
        return room_canvas_set_error(
            out_error,
            "FETCH_FAILED",
            "image download failed; verify the URL is reachable from the device and returns HTTP 2xx",
            err != ESP_OK ? err : ESP_FAIL);
    }
    if (content_type_is(response.content_type, "text/html")) {
        discard_response(&response);
        return room_canvas_set_error(out_error, "DECODE_FAILED", HTML_GUIDANCE, ESP_ERR_NOT_SUPPORTED);
    }
    if (content_type_is(response.content_type, "image/svg+xml")) {
        discard_response(&response);
        return room_canvas_set_error(out_error, "DECODE_FAILED", SVG_GUIDANCE, ESP_ERR_NOT_SUPPORTED);
    }

    room_canvas_image_kind_t kind = ROOM_CANVAS_IMAGE_NONE;
    if (response.data_len >= 2 && response.data[0] == 0xff && response.data[1] == 0xd8) {
        kind = ROOM_CANVAS_IMAGE_JPEG;
    } else if (response.data_len >= 4 && response.data[0] == 0x89 &&
               response.data[1] == 0x50 && response.data[2] == 0x4e &&
               response.data[3] == 0x47) {
        kind = ROOM_CANVAS_IMAGE_PNG;
    }
    if (kind == ROOM_CANVAS_IMAGE_NONE) {
        discard_response(&response);
        return room_canvas_set_error(
            out_error,
            "DECODE_FAILED",
            "the URL did not return PNG or JPEG bytes; present a raster image or use canvas.a2ui.pushJSONL",
            ESP_ERR_NOT_SUPPORTED);
    }

    free(response.content_type);
    ESP_LOGI(
        TAG,
        "fetched %u bytes in %d ms",
        (unsigned)response.data_len,
        (int)((esp_timer_get_time() - fetch_start_us) / 1000));
    image->data = response.data;
    image->data_len = response.data_len;
    image->kind = kind;
    image->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    image->descriptor.header.cf = LV_COLOR_FORMAT_RAW;
    image->descriptor.data_size = (uint32_t)response.data_len;
    image->descriptor.data = response.data;
    return ESP_OK;
}

void room_canvas_release_image(room_canvas_image_t *image)
{
    if (image->decoded != NULL) {
        lv_draw_buf_destroy(image->decoded);
    }
    heap_caps_free(image->data);
    free(image->component_id);
    memset(image, 0, sizeof(*image));
}

/* Releasing a zeroed entry is a no-op, so callers may always pass the full
 * array and never reason about how many slots were populated before a failure. */
void room_canvas_release_image_array(room_canvas_image_t *array, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        room_canvas_release_image(&array[i]);
    }
}


static bool materialize_jpeg(room_canvas_image_t *image)
{
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t decoder = NULL;
    if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK || decoder == NULL) {
        return false;
    }

    jpeg_dec_io_t io = {
        .inbuf = image->data,
        .inbuf_len = (int)image->data_len,
    };
    jpeg_dec_header_info_t header = {0};
    bool valid = jpeg_dec_parse_header(decoder, &io, &header) == JPEG_ERR_OK &&
        header.width > 0 && header.height > 0 &&
        header.width <= ROOM_CANVAS_MAX_IMAGE_SIDE_PX &&
        header.height <= ROOM_CANVAS_MAX_IMAGE_SIDE_PX &&
        (uint32_t)header.width * (uint32_t)header.height <= ROOM_CANVAS_MAX_IMAGE_PIXELS;
    int output_len = 0;
    if (valid) {
        valid = jpeg_dec_get_outbuf_len(decoder, &output_len) == JPEG_ERR_OK &&
            output_len == (int)((size_t)header.width * (size_t)header.height * 2);
    }

    /* Decode straight into the LVGL buffer: no temp frame, no full-frame copy,
     * half the peak PSRAM. Requires tightly packed rows
     * (LV_DRAW_BUF_STRIDE_ALIGN=1) and the 16-byte output alignment
     * esp_new_jpeg demands (LV_DRAW_BUF_ALIGN=16). Both are build invariants;
     * a violation fails loudly instead of falling back to a slow copy path. */
    lv_draw_buf_t *decoded = valid
        ? lv_draw_buf_create(header.width, header.height, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO)
        : NULL;
    if (decoded != NULL &&
        (decoded->header.stride != (uint32_t)header.width * 2 ||
         ((uintptr_t)decoded->data & 0xf) != 0)) {
        ESP_LOGE(
            TAG,
            "draw buffer not decode-compatible (stride=%u data=%p); check LV_DRAW_BUF_ALIGN=16 and LV_DRAW_BUF_STRIDE_ALIGN=1",
            (unsigned)decoded->header.stride,
            decoded->data);
        lv_draw_buf_destroy(decoded);
        decoded = NULL;
    }
    if (decoded == NULL) {
        valid = false;
    }
    if (valid) {
        io.outbuf = decoded->data;
        valid = jpeg_dec_process(decoder, &io) == JPEG_ERR_OK && io.out_size == output_len;
    }
    jpeg_dec_close(decoder);
    if (!valid) {
        if (decoded != NULL) {
            lv_draw_buf_destroy(decoded);
        }
        return false;
    }
    heap_caps_free(image->data);
    image->data = NULL;
    image->data_len = 0;
    image->decoded = decoded;
    image->width = header.width;
    image->height = header.height;
    return true;
}

static bool materialize_png(room_canvas_image_t *image)
{
    lv_image_header_t header = {0};
    if (lv_image_decoder_get_info(&image->descriptor, &header) != LV_RESULT_OK ||
        header.w == 0 || header.h == 0) {
        return false;
    }
    /* The fetch cap bounds compressed bytes only; a tiny file can declare a
     * huge decode. Reject before lv_image_decoder_open allocates it. */
    if (header.w > ROOM_CANVAS_MAX_IMAGE_SIDE_PX ||
        header.h > ROOM_CANVAS_MAX_IMAGE_SIDE_PX ||
        (uint32_t)header.w * (uint32_t)header.h > ROOM_CANVAS_MAX_IMAGE_PIXELS) {
        return false;
    }
    lv_image_decoder_dsc_t decoder = {0};
    lv_image_decoder_args_t args = {0};
    if (lv_image_decoder_open(&decoder, &image->descriptor, &args) != LV_RESULT_OK) {
        return false;
    }
    /* Materialize the decode now, on the command task: decoding at draw time
     * runs on the LVGL task and starves the task watchdog for large images. */
    lv_draw_buf_t *decoded_copy = decoder.decoded != NULL
        ? lv_draw_buf_dup(decoder.decoded)
        : NULL;
    lv_image_decoder_close(&decoder);
    if (decoded_copy == NULL) {
        return false;
    }
    heap_caps_free(image->data);
    image->data = NULL;
    image->data_len = 0;
    image->decoded = decoded_copy;
    image->width = header.w;
    image->height = header.h;
    return true;
}

/*
 * Runs on the command task WITHOUT the display lock: the LVGL image cache is
 * disabled (LV_CACHE_DEF_SIZE=0), the decoder list is init-time stable, and
 * lv_malloc is thread-safe clib malloc, so decoding touches no state the LVGL
 * task can see. Rendering and touch stay live through a long decode.
 */
bool room_canvas_validate_image(room_canvas_image_t *image)
{
    int64_t start_us = esp_timer_get_time();
    size_t compressed_len = image->data_len;
    bool jpeg = image->kind == ROOM_CANVAS_IMAGE_JPEG;
    bool valid = image->kind == ROOM_CANVAS_IMAGE_PNG
        ? materialize_png(image)
        : (jpeg && materialize_jpeg(image));
    if (valid) {
        ESP_LOGI(
            TAG,
            "decoded %s %ub -> %ux%u in %d ms",
            jpeg ? "jpeg" : "png",
            (unsigned)compressed_len,
            (unsigned)image->width,
            (unsigned)image->height,
            (int)((esp_timer_get_time() - start_us) / 1000));
    }
    return valid;
}

/*
 * The physical glass has large rounded corners, so laid-out content needs a
 * safe-area inset (Apple's "safe area") to stay visible. Full-bleed image
 * presentation intentionally keeps zero padding, like wallpaper.
 */
