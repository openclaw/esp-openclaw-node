#include "room_canvas.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_jpeg_dec.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "src/draw/lv_image_decoder_private.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "mbedtls/base64.h"
#include "room_ui.h"

#define TAG "room_canvas"
#define ROOM_CANVAS_WIDTH 410
#define ROOM_CANVAS_HEIGHT 502
#define ROOM_CANVAS_BRIGHTNESS 80
#define ROOM_CANVAS_MAX_COMPONENTS 96
#define ROOM_CANVAS_MAX_JSONL_BYTES (256 * 1024)
#define ROOM_CANVAS_MAX_STORE_BYTES (256 * 1024)
#define ROOM_CANVAS_MAX_DEPTH 8
#define ROOM_CANVAS_MAX_RENDER_NODES 256
#define ROOM_CANVAS_MAX_IMAGES 6
#define ROOM_CANVAS_MAX_IMAGE_BYTES (2 * 1024 * 1024)
#define ROOM_CANVAS_MAX_IMAGE_SIDE_PX 2048
#define ROOM_CANVAS_MAX_IMAGE_PIXELS (1024 * 1024)
#define ROOM_CANVAS_HTTP_TIMEOUT_MS 10000
#define ROOM_CANVAS_DISPLAY_LOCK_MS 1000

static const char *HTML_GUIDANCE =
    "this canvas renders images and A2UI only; render HTML to a PNG/JPEG (e.g. browser screenshot) and present that URL, or use canvas.a2ui.pushJSONL";
static const char *SVG_GUIDANCE =
    "SVG is not supported on this canvas; rasterize to PNG/JPEG first, or use canvas.a2ui.pushJSONL";

typedef enum {
    ROOM_CANVAS_IMAGE_NONE = 0,
    ROOM_CANVAS_IMAGE_JPEG,
    ROOM_CANVAS_IMAGE_PNG,
} room_canvas_image_kind_t;

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

typedef struct {
    uint8_t *data;
    size_t data_len;
    room_canvas_image_kind_t kind;
    lv_image_dsc_t descriptor;
    lv_draw_buf_t *decoded;
    uint32_t width;
    uint32_t height;
    char *component_id;
} room_canvas_image_t;

typedef struct {
    char *id;
    cJSON *component;
    double weight;
} room_canvas_component_t;

typedef struct {
    room_canvas_component_t components[ROOM_CANVAS_MAX_COMPONENTS];
    size_t component_count;
    cJSON *data_model;
    char *surface_id;
    char *root_component_id;
} room_canvas_store_snapshot_t;

static lv_obj_t *status_screen;
static lv_obj_t *canvas_screen;
static volatile bool canvas_active;
static esp_openclaw_node_handle_t canvas_node;
static char *gateway_http_base;
static room_canvas_component_t components[ROOM_CANVAS_MAX_COMPONENTS];
static size_t component_count;
static cJSON *data_model;
static char *surface_id;
static char *root_component_id;
static room_canvas_image_t images[ROOM_CANVAS_MAX_IMAGES];
static size_t image_count;

static void release_image(room_canvas_image_t *image);

static esp_err_t set_error(
    esp_openclaw_node_error_t *out_error,
    const char *code,
    const char *message,
    esp_err_t err)
{
    out_error->code = code;
    out_error->message = message;
    return err;
}

static void *large_alloc(size_t size)
{
    void *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return buffer;
}

static void *large_aligned_alloc(size_t alignment, size_t size)
{
    void *buffer = heap_caps_aligned_alloc(
        alignment,
        size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = heap_caps_aligned_alloc(alignment, size, MALLOC_CAP_8BIT);
    }
    return buffer;
}

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

    size_t capacity = response->capacity > 0 ? response->capacity : 16384;
    while (capacity < required) {
        size_t next = capacity * 2;
        capacity = next > ROOM_CANVAS_MAX_IMAGE_BYTES
            ? ROOM_CANVAS_MAX_IMAGE_BYTES
            : next;
        if (capacity < required && capacity == ROOM_CANVAS_MAX_IMAGE_BYTES) {
            response->too_large = true;
            return false;
        }
    }

    uint8_t *grown = large_alloc(capacity);
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
        return ESP_ERR_TIMEOUT;
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
            return ESP_ERR_NO_MEM;
        }
        free(response->content_type);
        response->content_type = copy;
    } else if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        size_t required = response->data_len + (size_t)event->data_len;
        if (!response_reserve(response, required)) {
            return response->too_large ? ESP_ERR_INVALID_SIZE : ESP_ERR_NO_MEM;
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
            set_error(out_error, "INTERNAL", "not enough memory to copy the canvas URL", ESP_ERR_NO_MEM);
        }
        return copy;
    }
    if (url[0] != '/') {
        set_error(
            out_error,
            "FETCH_FAILED",
            "pass an absolute HTTP(S) URL reachable from the device",
            ESP_FAIL);
        return NULL;
    }
    if (canvas_node == NULL || gateway_http_base == NULL) {
        set_error(
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
    char *surface_url = esp_openclaw_node_dup_plugin_surface_url(canvas_node, "canvas");
    if (surface_url == NULL) {
        set_error(
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
        set_error(
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
    size_t required = strlen(gateway_http_base) + surface_path_len + strlen(url) + 1;
    char *resolved = malloc(required);
    if (resolved == NULL) {
        free(surface_url);
        set_error(out_error, "INTERNAL", "not enough memory to resolve the canvas URL", ESP_ERR_NO_MEM);
        return NULL;
    }
    snprintf(
        resolved,
        required,
        "%s%.*s%s",
        gateway_http_base,
        (int)surface_path_len,
        surface_path,
        url);

    free(surface_url);
    return resolved;
}

static esp_err_t fetch_image(
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

    room_canvas_http_response_t response = {
        .deadline_us = esp_timer_get_time() + (int64_t)ROOM_CANVAS_HTTP_TIMEOUT_MS * 1000,
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
        return set_error(
            out_error,
            "FETCH_FAILED",
            "could not start the HTTP request; verify the URL is reachable from the device",
            ESP_FAIL);
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (response.too_large) {
        heap_caps_free(response.data);
        free(response.content_type);
        return set_error(
            out_error,
            "INVALID_PARAMS",
            "canvas image exceeds the 2097152-byte fetch bound",
            ESP_ERR_INVALID_SIZE);
    }
    if (response.timed_out) {
        heap_caps_free(response.data);
        free(response.content_type);
        return set_error(
            out_error,
            "FETCH_FAILED",
            "image download exceeded the 10-second fetch deadline; serve a smaller image or a faster host",
            ESP_ERR_TIMEOUT);
    }
    if (response.no_memory) {
        heap_caps_free(response.data);
        free(response.content_type);
        return set_error(
            out_error,
            "INTERNAL",
            "not enough memory to receive the canvas image",
            ESP_ERR_NO_MEM);
    }
    if (err != ESP_OK || status < 200 || status >= 300) {
        heap_caps_free(response.data);
        free(response.content_type);
        return set_error(
            out_error,
            "FETCH_FAILED",
            "image download failed; verify the URL is reachable from the device and returns HTTP 2xx",
            err != ESP_OK ? err : ESP_FAIL);
    }
    if (content_type_is(response.content_type, "text/html")) {
        heap_caps_free(response.data);
        free(response.content_type);
        return set_error(out_error, "DECODE_FAILED", HTML_GUIDANCE, ESP_ERR_NOT_SUPPORTED);
    }
    if (content_type_is(response.content_type, "image/svg+xml")) {
        heap_caps_free(response.data);
        free(response.content_type);
        return set_error(out_error, "DECODE_FAILED", SVG_GUIDANCE, ESP_ERR_NOT_SUPPORTED);
    }
    free(response.content_type);

    room_canvas_image_kind_t kind = ROOM_CANVAS_IMAGE_NONE;
    if (response.data_len >= 2 && response.data[0] == 0xff && response.data[1] == 0xd8) {
        kind = ROOM_CANVAS_IMAGE_JPEG;
    } else if (response.data_len >= 4 && response.data[0] == 0x89 &&
               response.data[1] == 0x50 && response.data[2] == 0x4e &&
               response.data[3] == 0x47) {
        kind = ROOM_CANVAS_IMAGE_PNG;
    }
    if (kind == ROOM_CANVAS_IMAGE_NONE) {
        heap_caps_free(response.data);
        return set_error(
            out_error,
            "DECODE_FAILED",
            "the URL did not return PNG or JPEG bytes; present a raster image or use canvas.a2ui.pushJSONL",
            ESP_ERR_NOT_SUPPORTED);
    }

    image->data = response.data;
    image->data_len = response.data_len;
    image->kind = kind;
    image->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    image->descriptor.header.cf = LV_COLOR_FORMAT_RAW;
    image->descriptor.data_size = (uint32_t)response.data_len;
    image->descriptor.data = response.data;
    return ESP_OK;
}

static void release_image(room_canvas_image_t *image)
{
    if (image->decoded != NULL) {
        lv_draw_buf_destroy(image->decoded);
    }
    heap_caps_free(image->data);
    free(image->component_id);
    memset(image, 0, sizeof(*image));
}

static void clear_images_locked(void)
{
    for (size_t i = 0; i < image_count; ++i) {
        release_image(&images[i]);
    }
    image_count = 0;
}

static bool materialize_jpeg_locked(room_canvas_image_t *image)
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

    uint8_t *pixels = valid ? large_aligned_alloc(16, (size_t)output_len) : NULL;
    if (pixels == NULL) {
        valid = false;
    }
    if (valid) {
        io.outbuf = pixels;
        valid = jpeg_dec_process(decoder, &io) == JPEG_ERR_OK && io.out_size == output_len;
    }
    jpeg_dec_close(decoder);

    lv_draw_buf_t *decoded = valid
        ? lv_draw_buf_create(header.width, header.height, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO)
        : NULL;
    if (decoded == NULL) {
        valid = false;
    }
    if (valid) {
        size_t row_size = (size_t)header.width * 2;
        for (uint32_t y = 0; y < header.height; ++y) {
            memcpy(
                decoded->data + ((size_t)y * decoded->header.stride),
                pixels + ((size_t)y * row_size),
                row_size);
        }
        heap_caps_free(image->data);
        image->data = NULL;
        image->data_len = 0;
        image->decoded = decoded;
        image->width = header.width;
        image->height = header.height;
    } else if (decoded != NULL) {
        lv_draw_buf_destroy(decoded);
    }
    heap_caps_free(pixels);
    return valid;
}

static bool validate_image_locked(room_canvas_image_t *image)
{
    if (image->kind == ROOM_CANVAS_IMAGE_JPEG) {
        return materialize_jpeg_locked(image);
    }
    if (image->kind != ROOM_CANVAS_IMAGE_PNG) {
        return false;
    }

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
 * The physical glass has large rounded corners, so laid-out content needs a
 * safe-area inset (Apple's "safe area") to stay visible. Full-bleed image
 * presentation intentionally keeps zero padding, like wallpaper.
 */
#define ROOM_CANVAS_SAFE_PAD 32

static void style_canvas_screen(int32_t pad)
{
    lv_obj_set_style_bg_color(canvas_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(canvas_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(canvas_screen, lv_color_white(), 0);
    lv_obj_set_style_pad_all(canvas_screen, pad, 0);
    lv_obj_remove_flag(canvas_screen, LV_OBJ_FLAG_SCROLLABLE);
}

static const lv_font_t *font_for_usage(const char *usage);

/*
 * Diagnostic placeholder: a full-bleed field with a border drawn exactly on the
 * safe-area inset and dots at each safe-area corner. Solid coverage makes panel
 * flush artifacts obvious, and the markers show at a glance how much of the
 * rectangle the rounded glass actually clips.
 */
static void show_test_card_locked(void)
{
    lv_obj_clean(canvas_screen);
    clear_images_locked();
    style_canvas_screen(0);

    lv_obj_t *field = lv_obj_create(canvas_screen);
    lv_obj_remove_style_all(field);
    lv_obj_set_size(field, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(field, lv_color_hex(0x202830), 0);
    lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
    lv_obj_remove_flag(field, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(field, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *safe = lv_obj_create(field);
    lv_obj_remove_style_all(safe);
    lv_obj_set_pos(safe, ROOM_CANVAS_SAFE_PAD, ROOM_CANVAS_SAFE_PAD);
    lv_obj_set_size(
        safe,
        ROOM_CANVAS_WIDTH - 2 * ROOM_CANVAS_SAFE_PAD,
        ROOM_CANVAS_HEIGHT - 2 * ROOM_CANVAS_SAFE_PAD);
    lv_obj_set_style_border_width(safe, 2, 0);
    lv_obj_set_style_border_color(safe, lv_color_hex(0x36d399), 0);
    lv_obj_set_style_radius(safe, 8, 0);
    lv_obj_set_style_bg_opa(safe, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(safe, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(safe, LV_OBJ_FLAG_CLICKABLE);

    static const lv_align_t corners[] = {
        LV_ALIGN_TOP_LEFT,
        LV_ALIGN_TOP_RIGHT,
        LV_ALIGN_BOTTOM_LEFT,
        LV_ALIGN_BOTTOM_RIGHT,
    };
    for (size_t i = 0; i < sizeof(corners) / sizeof(corners[0]); ++i) {
        lv_obj_t *dot = lv_obj_create(safe);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 14, 14);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(0xf87272), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_align(dot, corners[i], 0, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *caption = lv_label_create(safe);
    lv_obj_set_style_text_color(caption, lv_color_white(), 0);
    lv_obj_set_style_text_font(caption, font_for_usage("body"), 0);
    lv_label_set_text(caption, "safe area\ntap to exit");
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(caption);
}

/*
 * Uniform fill with zero content variation. If the panel still shows streaks
 * here, the artifact is in the transfer path; if this is clean while the test
 * card streaks, it is in rendering.
 */
static void show_solid_fill_locked(void)
{
    lv_obj_clean(canvas_screen);
    clear_images_locked();
    style_canvas_screen(0);
    lv_obj_t *field = lv_obj_create(canvas_screen);
    lv_obj_remove_style_all(field);
    lv_obj_set_size(field, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(field, lv_color_hex(0x1040c0), 0);
    lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
    lv_obj_remove_flag(field, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(field, LV_OBJ_FLAG_CLICKABLE);
}

static void show_placeholder_locked(void)
{
    lv_obj_clean(canvas_screen);
    clear_images_locked();
    style_canvas_screen(ROOM_CANVAS_SAFE_PAD);
    lv_obj_t *label = lv_label_create(canvas_screen);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
#if LV_FONT_MONTSERRAT_20
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
#elif LV_FONT_MONTSERRAT_16
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
#else
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
#endif
    lv_label_set_text(label, "Canvas ready");
    lv_obj_center(label);
}

static void activate_canvas_locked(void)
{
    lv_screen_load(canvas_screen);
    canvas_active = true;
}

static void configure_image_object(
    lv_obj_t *object,
    lv_obj_t *parent,
    const room_canvas_image_t *image,
    bool fullscreen)
{
    lv_image_set_inner_align(object, LV_IMAGE_ALIGN_CONTAIN);
    if (fullscreen) {
        lv_obj_set_size(object, ROOM_CANVAS_WIDTH, ROOM_CANVAS_HEIGHT);
    } else {
        lv_obj_update_layout(parent);
        int width = lv_obj_get_content_width(parent);
        if (width <= 0 || width > ROOM_CANVAS_WIDTH - 24) {
            width = ROOM_CANVAS_WIDTH - 24;
        }
        int height = image->width > 0
            ? (int)(((uint64_t)width * image->height) / image->width)
            : ROOM_CANVAS_HEIGHT / 3;
        if (height <= 0 || height > ROOM_CANVAS_HEIGHT / 3) {
            height = ROOM_CANVAS_HEIGHT / 3;
        }
        lv_obj_set_size(object, LV_PCT(100), height);
    }
    lv_obj_set_align(object, LV_ALIGN_CENTER);
}

static esp_err_t show_present_image(
    room_canvas_image_t *image,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    if (!bsp_display_lock(ROOM_CANVAS_DISPLAY_LOCK_MS)) {
        release_image(image);
        return set_error(
            out_error,
            "UNAVAILABLE",
            "the display is busy; retry canvas.present",
            ESP_ERR_TIMEOUT);
    }

    if (!validate_image_locked(image)) {
        release_image(image);
        bsp_display_unlock();
        return set_error(
            out_error,
            "DECODE_FAILED",
            "LVGL could not decode the PNG or JPEG; images are bounded to 2048 px per side and 1 megapixel",
            ESP_ERR_INVALID_RESPONSE);
    }

    lv_obj_clean(canvas_screen);
    clear_images_locked();
    style_canvas_screen(0);
    images[0] = *image;
    memset(image, 0, sizeof(*image));
    image_count = 1;
    lv_obj_t *object = lv_image_create(canvas_screen);
    lv_image_set_src(object, images[0].decoded);
    configure_image_object(object, canvas_screen, &images[0], true);
    activate_canvas_locked();
    bsp_display_unlock();
    bsp_display_brightness_set(ROOM_CANVAS_BRIGHTNESS);
    room_ui_refresh();

    *out_payload_json = strdup(
        "{\"shown\":true,\"kind\":\"image\",\"width\":410,\"height\":502}");
    if (*out_payload_json == NULL) {
        return set_error(out_error, "INTERNAL", "not enough memory for the command result", ESP_ERR_NO_MEM);
    }
    return ESP_OK;
}

static void clear_store(void)
{
    for (size_t i = 0; i < component_count; ++i) {
        free(components[i].id);
        cJSON_Delete(components[i].component);
        memset(&components[i], 0, sizeof(components[i]));
    }
    component_count = 0;
    cJSON_Delete(data_model);
    data_model = NULL;
    free(surface_id);
    surface_id = NULL;
    free(root_component_id);
    root_component_id = NULL;
}

static void free_store_snapshot(room_canvas_store_snapshot_t *snapshot)
{
    for (size_t i = 0; i < snapshot->component_count; ++i) {
        free(snapshot->components[i].id);
        cJSON_Delete(snapshot->components[i].component);
    }
    cJSON_Delete(snapshot->data_model);
    free(snapshot->surface_id);
    free(snapshot->root_component_id);
    memset(snapshot, 0, sizeof(*snapshot));
}

static esp_err_t copy_store_snapshot(room_canvas_store_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->component_count = component_count;
    for (size_t i = 0; i < component_count; ++i) {
        snapshot->components[i].id = strdup(components[i].id);
        snapshot->components[i].component = cJSON_Duplicate(components[i].component, true);
        snapshot->components[i].weight = components[i].weight;
        if (snapshot->components[i].id == NULL ||
            snapshot->components[i].component == NULL) {
            free_store_snapshot(snapshot);
            return ESP_ERR_NO_MEM;
        }
    }
    snapshot->data_model = data_model != NULL ? cJSON_Duplicate(data_model, true) : NULL;
    snapshot->surface_id = surface_id != NULL ? strdup(surface_id) : NULL;
    snapshot->root_component_id = root_component_id != NULL
        ? strdup(root_component_id)
        : NULL;
    if ((data_model != NULL && snapshot->data_model == NULL) ||
        (surface_id != NULL && snapshot->surface_id == NULL) ||
        (root_component_id != NULL && snapshot->root_component_id == NULL)) {
        free_store_snapshot(snapshot);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void restore_store_snapshot(room_canvas_store_snapshot_t *snapshot)
{
    clear_store();
    component_count = snapshot->component_count;
    for (size_t i = 0; i < component_count; ++i) {
        components[i] = snapshot->components[i];
        memset(&snapshot->components[i], 0, sizeof(snapshot->components[i]));
    }
    data_model = snapshot->data_model;
    surface_id = snapshot->surface_id;
    root_component_id = snapshot->root_component_id;
    memset(snapshot, 0, sizeof(*snapshot));
}

static room_canvas_component_t *find_component(const char *id)
{
    for (size_t i = 0; i < component_count; ++i) {
        if (strcmp(components[i].id, id) == 0) {
            return &components[i];
        }
    }
    return NULL;
}

static cJSON *component_type_item(const room_canvas_component_t *entry)
{
    return cJSON_IsObject(entry->component) ? entry->component->child : NULL;
}

static bool component_is_image(const room_canvas_component_t *entry)
{
    cJSON *type = component_type_item(entry);
    return type != NULL && type->string != NULL && strcmp(type->string, "Image") == 0;
}

static esp_err_t validate_component_depth(
    const char *id,
    size_t depth,
    size_t *node_budget,
    esp_openclaw_node_error_t *out_error)
{
    if (*node_budget == 0) {
        return set_error(
            out_error,
            "INVALID_PARAMS",
            "A2UI graph expands past the 256-node bound",
            ESP_ERR_INVALID_SIZE);
    }
    --*node_budget;
    if (depth > ROOM_CANVAS_MAX_DEPTH) {
        return set_error(
            out_error,
            "INVALID_PARAMS",
            "A2UI component nesting exceeds the 8-level bound",
            ESP_ERR_INVALID_SIZE);
    }
    room_canvas_component_t *entry = find_component(id);
    cJSON *type = entry != NULL ? component_type_item(entry) : NULL;
    if (type == NULL || type->string == NULL || !cJSON_IsObject(type)) {
        return ESP_OK;
    }
    if (strcmp(type->string, "Card") == 0) {
        cJSON *child = cJSON_GetObjectItemCaseSensitive(type, "child");
        return cJSON_IsString(child) && child->valuestring != NULL
            ? validate_component_depth(
                child->valuestring,
                depth + 1,
                node_budget,
                out_error)
            : ESP_OK;
    }
    if (strcmp(type->string, "Column") != 0 &&
        strcmp(type->string, "Row") != 0 &&
        strcmp(type->string, "List") != 0) {
        return ESP_OK;
    }
    cJSON *children = cJSON_GetObjectItemCaseSensitive(type, "children");
    cJSON *list = cJSON_IsObject(children)
        ? cJSON_GetObjectItemCaseSensitive(children, "explicitList")
        : NULL;
    cJSON *child = NULL;
    cJSON_ArrayForEach(child, list) {
        if (cJSON_IsString(child) && child->valuestring != NULL) {
            esp_err_t err = validate_component_depth(
                child->valuestring,
                depth + 1,
                node_budget,
                out_error);
            if (err != ESP_OK) {
                return err;
            }
        }
    }
    return ESP_OK;
}

static size_t count_images_in_store(void)
{
    size_t count = 0;
    for (size_t i = 0; i < component_count; ++i) {
        count += component_is_image(&components[i]) ? 1 : 0;
    }
    return count;
}

static char *decode_json_pointer_token(const char *start, size_t len)
{
    char *token = malloc(len + 1);
    if (token == NULL) {
        return NULL;
    }
    size_t written = 0;
    for (size_t i = 0; i < len; ++i) {
        if (start[i] == '~' && i + 1 < len) {
            if (start[i + 1] == '0') {
                token[written++] = '~';
                ++i;
                continue;
            }
            if (start[i + 1] == '1') {
                token[written++] = '/';
                ++i;
                continue;
            }
        }
        token[written++] = start[i];
    }
    token[written] = '\0';
    return token;
}

static cJSON *resolve_data_path(const char *path)
{
    if (data_model == NULL || path == NULL) {
        return NULL;
    }
    if (path[0] == '\0' || strcmp(path, "/") == 0) {
        return data_model;
    }
    if (path[0] != '/') {
        return NULL;
    }

    cJSON *current = data_model;
    const char *cursor = path + 1;
    while (*cursor != '\0') {
        const char *slash = strchr(cursor, '/');
        size_t len = slash != NULL ? (size_t)(slash - cursor) : strlen(cursor);
        char *token = decode_json_pointer_token(cursor, len);
        if (token == NULL) {
            return NULL;
        }
        if (cJSON_IsObject(current)) {
            current = cJSON_GetObjectItemCaseSensitive(current, token);
        } else if (cJSON_IsArray(current)) {
            char *end = NULL;
            long index = strtol(token, &end, 10);
            current = end != token && *end == '\0' && index >= 0
                ? cJSON_GetArrayItem(current, (int)index)
                : NULL;
        } else {
            current = NULL;
        }
        free(token);
        if (current == NULL || slash == NULL) {
            return current;
        }
        cursor = slash + 1;
    }
    return current;
}

static cJSON *resolve_value(const cJSON *value)
{
    if (!cJSON_IsObject(value)) {
        return NULL;
    }
    cJSON *path = cJSON_GetObjectItemCaseSensitive(value, "path");
    if (cJSON_IsString(path) && path->valuestring != NULL) {
        return resolve_data_path(path->valuestring);
    }
    cJSON *literal = cJSON_GetObjectItemCaseSensitive(value, "literalString");
    if (literal != NULL) {
        return literal;
    }
    literal = cJSON_GetObjectItemCaseSensitive(value, "literalNumber");
    if (literal != NULL) {
        return literal;
    }
    return cJSON_GetObjectItemCaseSensitive(value, "literalBoolean");
}

static const char *resolve_string(const cJSON *value)
{
    cJSON *resolved = resolve_value(value);
    return cJSON_IsString(resolved) && resolved->valuestring != NULL
        ? resolved->valuestring
        : "";
}

static const char *resolve_text(const cJSON *value, char *buffer, size_t buffer_size)
{
    cJSON *resolved = resolve_value(value);
    if (cJSON_IsString(resolved) && resolved->valuestring != NULL) {
        return resolved->valuestring;
    }
    if (cJSON_IsNumber(resolved)) {
        snprintf(buffer, buffer_size, "%.15g", resolved->valuedouble);
        return buffer;
    }
    if (cJSON_IsBool(resolved)) {
        return cJSON_IsTrue(resolved) ? "true" : "false";
    }
    return "";
}

static bool resolve_boolean(const cJSON *value)
{
    cJSON *resolved = resolve_value(value);
    return cJSON_IsTrue(resolved) ||
           (cJSON_IsNumber(resolved) && resolved->valuedouble != 0);
}

static const lv_font_t *font_for_usage(const char *usage)
{
    if (usage != NULL && strcmp(usage, "h1") == 0) {
#if LV_FONT_MONTSERRAT_28
        return &lv_font_montserrat_28;
#elif LV_FONT_MONTSERRAT_24
        return &lv_font_montserrat_24;
#elif LV_FONT_MONTSERRAT_20
        return &lv_font_montserrat_20;
#elif LV_FONT_MONTSERRAT_16
        return &lv_font_montserrat_16;
#else
        return &lv_font_montserrat_14;
#endif
    }
    if (usage != NULL && strcmp(usage, "h2") == 0) {
#if LV_FONT_MONTSERRAT_24
        return &lv_font_montserrat_24;
#elif LV_FONT_MONTSERRAT_20
        return &lv_font_montserrat_20;
#elif LV_FONT_MONTSERRAT_16
        return &lv_font_montserrat_16;
#else
        return &lv_font_montserrat_14;
#endif
    }
    if (usage != NULL && strcmp(usage, "h3") == 0) {
#if LV_FONT_MONTSERRAT_20
        return &lv_font_montserrat_20;
#elif LV_FONT_MONTSERRAT_16
        return &lv_font_montserrat_16;
#else
        return &lv_font_montserrat_14;
#endif
    }
    if (usage != NULL && strcmp(usage, "caption") == 0) {
        return &lv_font_montserrat_14;
    }
#if LV_FONT_MONTSERRAT_16
    return &lv_font_montserrat_16;
#else
    return &lv_font_montserrat_14;
#endif
}

static lv_flex_align_t cross_alignment(const char *alignment)
{
    if (alignment != NULL && strcmp(alignment, "center") == 0) {
        return LV_FLEX_ALIGN_CENTER;
    }
    if (alignment != NULL && strcmp(alignment, "end") == 0) {
        return LV_FLEX_ALIGN_END;
    }
    return LV_FLEX_ALIGN_START;
}

static room_canvas_image_t *find_image(
    room_canvas_image_t *retained_images,
    size_t retained_image_count,
    const char *component_id)
{
    for (size_t i = 0; i < retained_image_count; ++i) {
        if (retained_images[i].component_id != NULL &&
            strcmp(retained_images[i].component_id, component_id) == 0) {
            return &retained_images[i];
        }
    }
    return NULL;
}

static void button_event(lv_event_t *event)
{
    char *action_name = lv_event_get_user_data(event);
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "A2UI button action: %s", action_name != NULL ? action_name : "");
    } else if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        free(action_name);
    }
}

static lv_obj_t *render_component(
    const char *id,
    lv_obj_t *parent,
    room_canvas_image_t *render_images,
    size_t render_image_count,
    size_t depth,
    bool root,
    bool parent_row,
    size_t *node_budget,
    esp_openclaw_node_error_t *out_error);

static void apply_weight(lv_obj_t *object, const room_canvas_component_t *entry)
{
    if (entry->weight > 0) {
        uint32_t weight = entry->weight >= UINT32_MAX
            ? UINT32_MAX
            : (uint32_t)entry->weight;
        lv_obj_set_flex_grow(object, weight > 0 ? weight : 1);
    }
}

static void render_children(
    cJSON *props,
    lv_obj_t *parent,
    room_canvas_image_t *render_images,
    size_t render_image_count,
    size_t depth,
    bool parent_row,
    size_t *node_budget,
    esp_openclaw_node_error_t *out_error)
{
    cJSON *children = cJSON_GetObjectItemCaseSensitive(props, "children");
    cJSON *list = cJSON_IsObject(children)
        ? cJSON_GetObjectItemCaseSensitive(children, "explicitList")
        : NULL;
    cJSON *child = NULL;
    cJSON_ArrayForEach(child, list) {
        if (cJSON_IsString(child) && child->valuestring != NULL) {
            render_component(
                child->valuestring,
                parent,
                render_images,
                render_image_count,
                depth + 1,
                false,
                parent_row,
                node_budget,
                out_error);
            if (out_error->code != NULL) {
                return;
            }
        }
    }
}

static lv_obj_t *render_container(
    const room_canvas_component_t *entry,
    cJSON *props,
    lv_obj_t *parent,
    room_canvas_image_t *render_images,
    size_t render_image_count,
    size_t depth,
    bool root,
    bool row,
    size_t *node_budget,
    esp_openclaw_node_error_t *out_error)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(object, 4, 0);
    lv_obj_set_style_pad_gap(object, 8, 0);
    lv_obj_set_layout(object, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(object, row ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    cJSON *alignment = cJSON_GetObjectItemCaseSensitive(props, "alignment");
    const char *alignment_text = cJSON_IsString(alignment) ? alignment->valuestring : NULL;
    lv_obj_set_flex_align(
        object,
        LV_FLEX_ALIGN_START,
        cross_alignment(alignment_text),
        LV_FLEX_ALIGN_START);
    lv_obj_set_size(
        object,
        LV_PCT(100),
        root ? LV_PCT(100) : LV_SIZE_CONTENT);
    apply_weight(object, entry);
    render_children(
        props,
        object,
        render_images,
        render_image_count,
        depth,
        row,
        node_budget,
        out_error);
    if (alignment_text != NULL && strcmp(alignment_text, "stretch") == 0) {
        uint32_t child_count = lv_obj_get_child_count(object);
        for (uint32_t i = 0; i < child_count; ++i) {
            lv_obj_t *child = lv_obj_get_child(object, (int32_t)i);
            if (row) {
                lv_obj_set_height(child, LV_PCT(100));
            } else {
                lv_obj_set_width(child, LV_PCT(100));
            }
        }
    }
    return object;
}

static lv_obj_t *render_component(
    const char *id,
    lv_obj_t *parent,
    room_canvas_image_t *render_images,
    size_t render_image_count,
    size_t depth,
    bool root,
    bool parent_row,
    size_t *node_budget,
    esp_openclaw_node_error_t *out_error)
{
    if (*node_budget == 0) {
        set_error(
            out_error,
            "INVALID_PARAMS",
            "A2UI graph expands past the 256-node bound",
            ESP_ERR_INVALID_SIZE);
        return NULL;
    }
    --*node_budget;
    if (depth > ROOM_CANVAS_MAX_DEPTH) {
        set_error(
            out_error,
            "INVALID_PARAMS",
            "A2UI component nesting exceeds the 8-level bound",
            ESP_ERR_INVALID_SIZE);
        return NULL;
    }
    room_canvas_component_t *entry = find_component(id);
    if (entry == NULL) {
        lv_obj_t *missing = lv_label_create(parent);
        lv_label_set_text_fmt(missing, "[missing:%s]", id);
        return missing;
    }
    cJSON *type = component_type_item(entry);
    if (type == NULL || type->string == NULL || !cJSON_IsObject(type)) {
        lv_obj_t *invalid = lv_label_create(parent);
        lv_label_set_text(invalid, "[Invalid]");
        return invalid;
    }
    const char *name = type->string;
    cJSON *props = type;

    if (strcmp(name, "Column") == 0 || strcmp(name, "List") == 0) {
        return render_container(
            entry,
            props,
            parent,
            render_images,
            render_image_count,
            depth,
            root,
            false,
            node_budget,
            out_error);
    }
    if (strcmp(name, "Row") == 0) {
        return render_container(
            entry,
            props,
            parent,
            render_images,
            render_image_count,
            depth,
            root,
            true,
            node_budget,
            out_error);
    }
    if (strcmp(name, "Text") == 0) {
        lv_obj_t *label = lv_label_create(parent);
        cJSON *text = cJSON_GetObjectItemCaseSensitive(props, "text");
        cJSON *usage = cJSON_GetObjectItemCaseSensitive(props, "usageHint");
        char text_buffer[32] = {0};
        lv_label_set_text(label, resolve_text(text, text_buffer, sizeof(text_buffer)));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(label, parent_row ? LV_SIZE_CONTENT : LV_PCT(100));
        lv_obj_set_style_text_font(
            label,
            font_for_usage(cJSON_IsString(usage) ? usage->valuestring : "body"),
            0);
        apply_weight(label, entry);
        return label;
    }
    if (strcmp(name, "Image") == 0) {
        room_canvas_image_t *image = find_image(
            render_images,
            render_image_count,
            id);
        if (image == NULL) {
            lv_obj_t *missing = lv_label_create(parent);
            lv_label_set_text(missing, "[Image]");
            return missing;
        }
        lv_obj_t *object = lv_image_create(parent);
        lv_image_set_src(object, image->decoded);
        configure_image_object(object, parent, image, root);
        apply_weight(object, entry);
        return object;
    }
    if (strcmp(name, "Button") == 0) {
        lv_obj_t *button = lv_button_create(parent);
        lv_obj_t *label = lv_label_create(button);
        cJSON *label_value = cJSON_GetObjectItemCaseSensitive(props, "label");
        char label_buffer[32] = {0};
        lv_label_set_text(
            label,
            resolve_text(label_value, label_buffer, sizeof(label_buffer)));
        lv_obj_center(label);
        cJSON *action = cJSON_GetObjectItemCaseSensitive(props, "action");
        cJSON *action_name = cJSON_IsObject(action)
            ? cJSON_GetObjectItemCaseSensitive(action, "name")
            : NULL;
        char *action_copy = cJSON_IsString(action_name) && action_name->valuestring != NULL
            ? strdup(action_name->valuestring)
            : strdup("");
        lv_obj_add_event_cb(button, button_event, LV_EVENT_ALL, action_copy);
        apply_weight(button, entry);
        return button;
    }
    if (strcmp(name, "Card") == 0) {
        lv_obj_t *card = lv_obj_create(parent);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x151515), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(card, lv_color_white(), 0);
        lv_obj_set_style_pad_all(card, 12, 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x3a3a3a), 0);
        cJSON *child = cJSON_GetObjectItemCaseSensitive(props, "child");
        if (cJSON_IsString(child) && child->valuestring != NULL) {
            render_component(
                child->valuestring,
                card,
                render_images,
                render_image_count,
                depth + 1,
                false,
                false,
                node_budget,
                out_error);
        }
        apply_weight(card, entry);
        return card;
    }
    if (strcmp(name, "Divider") == 0) {
        lv_obj_t *divider = lv_obj_create(parent);
        lv_obj_remove_style_all(divider);
        lv_obj_set_size(divider, LV_PCT(100), 1);
        lv_obj_set_style_bg_color(divider, lv_color_hex(0x666666), 0);
        lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
        return divider;
    }
    if (strcmp(name, "CheckBox") == 0) {
        lv_obj_t *checkbox = lv_checkbox_create(parent);
        cJSON *label = cJSON_GetObjectItemCaseSensitive(props, "label");
        cJSON *value = cJSON_GetObjectItemCaseSensitive(props, "value");
        char label_buffer[32] = {0};
        lv_checkbox_set_text(
            checkbox,
            resolve_text(label, label_buffer, sizeof(label_buffer)));
        if (resolve_boolean(value)) {
            lv_obj_add_state(checkbox, LV_STATE_CHECKED);
        }
        lv_obj_remove_flag(checkbox, LV_OBJ_FLAG_CLICKABLE);
        apply_weight(checkbox, entry);
        return checkbox;
    }

    lv_obj_t *unknown = lv_label_create(parent);
    lv_label_set_text_fmt(unknown, "[%s]", name);
    lv_obj_set_style_text_font(unknown, font_for_usage("caption"), 0);
    return unknown;
}

static esp_err_t prefetch_a2ui_images(esp_openclaw_node_error_t *out_error)
{
    if (root_component_id == NULL) {
        if (!bsp_display_lock(ROOM_CANVAS_DISPLAY_LOCK_MS)) {
            return set_error(
                out_error,
                "UNAVAILABLE",
                "the display is busy; retry the A2UI command",
                ESP_ERR_TIMEOUT);
        }
        show_placeholder_locked();
        activate_canvas_locked();
        bsp_display_unlock();
        bsp_display_brightness_set(ROOM_CANVAS_BRIGHTNESS);
        room_ui_refresh();
        return ESP_OK;
    }

    room_canvas_image_t fetched[ROOM_CANVAS_MAX_IMAGES] = {0};
    size_t fetched_count = 0;
    for (size_t i = 0; i < component_count; ++i) {
        if (!component_is_image(&components[i])) {
            continue;
        }
        cJSON *type = component_type_item(&components[i]);
        cJSON *url = cJSON_GetObjectItemCaseSensitive(type, "url");
        const char *url_text = resolve_string(url);
        if (url_text[0] == '\0') {
            for (size_t j = 0; j < fetched_count; ++j) {
                release_image(&fetched[j]);
            }
            return set_error(
                out_error,
                "INVALID_PARAMS",
                "A2UI Image url must resolve to a non-empty string",
                ESP_ERR_INVALID_ARG);
        }
        esp_err_t err = fetch_image(url_text, &fetched[fetched_count], out_error);
        if (err != ESP_OK) {
            for (size_t j = 0; j <= fetched_count; ++j) {
                release_image(&fetched[j]);
            }
            return err;
        }
        fetched[fetched_count].component_id = strdup(components[i].id);
        if (fetched[fetched_count].component_id == NULL) {
            for (size_t j = 0; j <= fetched_count; ++j) {
                release_image(&fetched[j]);
            }
            return set_error(
                out_error,
                "INTERNAL",
                "not enough memory for the A2UI image store",
                ESP_ERR_NO_MEM);
        }
        ++fetched_count;
    }

    if (!bsp_display_lock(ROOM_CANVAS_DISPLAY_LOCK_MS)) {
        for (size_t i = 0; i < fetched_count; ++i) {
            release_image(&fetched[i]);
        }
        return set_error(
            out_error,
            "UNAVAILABLE",
            "the display is busy; retry the A2UI command",
            ESP_ERR_TIMEOUT);
    }
    for (size_t i = 0; i < fetched_count; ++i) {
        if (!validate_image_locked(&fetched[i])) {
            for (size_t j = 0; j < fetched_count; ++j) {
                release_image(&fetched[j]);
            }
            bsp_display_unlock();
            return set_error(
                out_error,
                "DECODE_FAILED",
                "LVGL could not decode an A2UI Image; images are bounded to 2048 px per side and 1 megapixel",
                ESP_ERR_INVALID_RESPONSE);
        }
    }

    lv_obj_t *container = lv_obj_create(canvas_screen);
    if (container == NULL) {
        for (size_t i = 0; i < fetched_count; ++i) {
            release_image(&fetched[i]);
        }
        bsp_display_unlock();
        return set_error(
            out_error,
            "INTERNAL",
            "not enough memory to stage the A2UI surface",
            ESP_ERR_NO_MEM);
    }
    /* This hidden transaction root has exactly the canvas content geometry,
     * so an LV_PCT(100) root renders as it did directly on canvas_screen. */
    lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_style_all(container);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_layout(container, LV_LAYOUT_NONE);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_size(container, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(container, LV_SCROLLBAR_MODE_OFF);
    /* Background taps must reach the screen's tap-to-hide handler; A2UI
     * buttons stay individually clickable. */
    lv_obj_remove_flag(container, LV_OBJ_FLAG_CLICKABLE);

    esp_openclaw_node_error_t render_error = {0};
    size_t node_budget = ROOM_CANVAS_MAX_RENDER_NODES;
    lv_obj_t *root = render_component(
        root_component_id,
        container,
        fetched,
        fetched_count,
        1,
        true,
        false,
        &node_budget,
        &render_error);
    if (root == NULL || render_error.code != NULL) {
        lv_obj_delete(container);
        for (size_t i = 0; i < fetched_count; ++i) {
            release_image(&fetched[i]);
        }
        if (render_error.code != NULL) {
            *out_error = render_error;
        } else {
            set_error(
                out_error,
                "INTERNAL",
                "not enough memory to render the A2UI surface",
                ESP_ERR_NO_MEM);
        }
        bsp_display_unlock();
        return render_error.code != NULL ? ESP_ERR_INVALID_SIZE : ESP_ERR_NO_MEM;
    }

    uint32_t child_count = lv_obj_get_child_count(canvas_screen);
    for (uint32_t i = child_count; i > 0; --i) {
        lv_obj_t *child = lv_obj_get_child(canvas_screen, (int32_t)i - 1);
        if (child != container) {
            lv_obj_delete(child);
        }
    }
    clear_images_locked();
    for (size_t i = 0; i < fetched_count; ++i) {
        /* LVGL retains the heap draw-buffer pointer, so moving its owning
         * record to stable storage does not invalidate the staged widget. */
        images[i] = fetched[i];
        memset(&fetched[i], 0, sizeof(fetched[i]));
    }
    image_count = fetched_count;
    /* The image path runs full-bleed (pad 0); restore the safe-area inset for
     * laid-out content. Only on success, so a failed render leaves the screen
     * exactly as it was. */
    style_canvas_screen(ROOM_CANVAS_SAFE_PAD);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_HIDDEN);
    activate_canvas_locked();
    bsp_display_unlock();
    bsp_display_brightness_set(ROOM_CANVAS_BRIGHTNESS);
    room_ui_refresh();
    return ESP_OK;
}

static esp_err_t render_a2ui_result(
    bool force_active,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    if (!force_active && !canvas_active) {
        int required = snprintf(
            NULL,
            0,
            "{\"shown\":false,\"kind\":\"a2ui\",\"components\":%u}",
            (unsigned)component_count);
        *out_payload_json = malloc((size_t)required + 1);
        if (*out_payload_json == NULL) {
            return set_error(out_error, "INTERNAL", "not enough memory for the command result", ESP_ERR_NO_MEM);
        }
        snprintf(
            *out_payload_json,
            (size_t)required + 1,
            "{\"shown\":false,\"kind\":\"a2ui\",\"components\":%u}",
            (unsigned)component_count);
        return ESP_OK;
    }

    esp_err_t err = prefetch_a2ui_images(out_error);
    if (err != ESP_OK) {
        return err;
    }
    int required = snprintf(
        NULL,
        0,
        "{\"shown\":true,\"kind\":\"a2ui\",\"components\":%u}",
        (unsigned)component_count);
    *out_payload_json = malloc((size_t)required + 1);
    if (*out_payload_json == NULL) {
        return set_error(out_error, "INTERNAL", "not enough memory for the command result", ESP_ERR_NO_MEM);
    }
    snprintf(
        *out_payload_json,
        (size_t)required + 1,
        "{\"shown\":true,\"kind\":\"a2ui\",\"components\":%u}",
        (unsigned)component_count);
    return ESP_OK;
}

static esp_err_t remember_surface_id(cJSON *action)
{
    cJSON *id = cJSON_GetObjectItemCaseSensitive(action, "surfaceId");
    if (!cJSON_IsString(id) || id->valuestring == NULL || id->valuestring[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    char *copy = strdup(id->valuestring);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    free(surface_id);
    surface_id = copy;
    return ESP_OK;
}

static esp_err_t apply_surface_update(
    cJSON *action,
    esp_openclaw_node_error_t *out_error)
{
    esp_err_t err = remember_surface_id(action);
    cJSON *updates = cJSON_GetObjectItemCaseSensitive(action, "components");
    if (err == ESP_ERR_INVALID_ARG || !cJSON_IsArray(updates)) {
        return set_error(
            out_error,
            "INVALID_PARAMS",
            "surfaceUpdate requires string surfaceId and a components array",
            ESP_ERR_INVALID_ARG);
    }
    if (err != ESP_OK) {
        return set_error(out_error, "INTERNAL", "not enough memory for the A2UI surface id", err);
    }

    cJSON *update = NULL;
    cJSON_ArrayForEach(update, updates) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(update, "id");
        cJSON *component = cJSON_GetObjectItemCaseSensitive(update, "component");
        cJSON *weight = cJSON_GetObjectItemCaseSensitive(update, "weight");
        if (!cJSON_IsObject(update) || !cJSON_IsString(id) ||
            id->valuestring == NULL || id->valuestring[0] == '\0' ||
            !cJSON_IsObject(component) || cJSON_GetArraySize(component) != 1 ||
            (weight != NULL && !cJSON_IsNumber(weight))) {
            return set_error(
                out_error,
                "INVALID_PARAMS",
                "each surfaceUpdate component requires string id and one component type",
                ESP_ERR_INVALID_ARG);
        }
        room_canvas_component_t *entry = find_component(id->valuestring);
        if (entry == NULL) {
            if (component_count >= ROOM_CANVAS_MAX_COMPONENTS) {
                return set_error(
                    out_error,
                    "INVALID_PARAMS",
                    "A2UI surface exceeds the 96-component bound",
                    ESP_ERR_INVALID_SIZE);
            }
            entry = &components[component_count];
            entry->id = strdup(id->valuestring);
            if (entry->id == NULL) {
                return set_error(out_error, "INTERNAL", "not enough memory for an A2UI component id", ESP_ERR_NO_MEM);
            }
            ++component_count;
        }
        cJSON *copy = cJSON_Duplicate(component, true);
        if (copy == NULL) {
            return set_error(out_error, "INTERNAL", "not enough memory for an A2UI component", ESP_ERR_NO_MEM);
        }
        cJSON_Delete(entry->component);
        entry->component = copy;
        entry->weight = cJSON_IsNumber(weight) ? weight->valuedouble : 0;
    }

    if (count_images_in_store() > ROOM_CANVAS_MAX_IMAGES) {
        return set_error(
            out_error,
            "INVALID_PARAMS",
            "A2UI surface exceeds the 6-image bound",
            ESP_ERR_INVALID_SIZE);
    }
    return ESP_OK;
}

static esp_err_t apply_begin_rendering(
    cJSON *action,
    esp_openclaw_node_error_t *out_error)
{
    esp_err_t err = remember_surface_id(action);
    cJSON *root = cJSON_GetObjectItemCaseSensitive(action, "root");
    if (err == ESP_ERR_INVALID_ARG || !cJSON_IsString(root) ||
        root->valuestring == NULL || root->valuestring[0] == '\0') {
        return set_error(
            out_error,
            "INVALID_PARAMS",
            "beginRendering requires string surfaceId and root",
            ESP_ERR_INVALID_ARG);
    }
    if (err != ESP_OK) {
        return set_error(out_error, "INTERNAL", "not enough memory for the A2UI surface id", err);
    }
    char *copy = strdup(root->valuestring);
    if (copy == NULL) {
        return set_error(out_error, "INTERNAL", "not enough memory for the A2UI root id", ESP_ERR_NO_MEM);
    }
    free(root_component_id);
    root_component_id = copy;
    return ESP_OK;
}

static esp_err_t set_data_model_path(
    const char *path,
    cJSON *contents,
    esp_openclaw_node_error_t *out_error)
{
    cJSON *copy = cJSON_Duplicate(contents, true);
    if (copy == NULL) {
        return set_error(out_error, "INTERNAL", "not enough memory for the A2UI data model", ESP_ERR_NO_MEM);
    }
    if (path == NULL || path[0] == '\0' || strcmp(path, "/") == 0) {
        cJSON_Delete(data_model);
        data_model = copy;
        return ESP_OK;
    }
    if (path[0] != '/') {
        cJSON_Delete(copy);
        return set_error(out_error, "INVALID_PARAMS", "dataModelUpdate path must be a JSON pointer", ESP_ERR_INVALID_ARG);
    }
    if (!cJSON_IsObject(data_model)) {
        cJSON_Delete(data_model);
        data_model = cJSON_CreateObject();
        if (data_model == NULL) {
            cJSON_Delete(copy);
            return set_error(out_error, "INTERNAL", "not enough memory for the A2UI data model", ESP_ERR_NO_MEM);
        }
    }

    cJSON *current = data_model;
    const char *cursor = path + 1;
    for (;;) {
        const char *slash = strchr(cursor, '/');
        size_t len = slash != NULL ? (size_t)(slash - cursor) : strlen(cursor);
        char *token = decode_json_pointer_token(cursor, len);
        if (token == NULL) {
            cJSON_Delete(copy);
            return set_error(out_error, "INTERNAL", "not enough memory for the A2UI data path", ESP_ERR_NO_MEM);
        }
        if (slash == NULL) {
            cJSON_DeleteItemFromObjectCaseSensitive(current, token);
            cJSON_AddItemToObject(current, token, copy);
            free(token);
            return ESP_OK;
        }
        cJSON *next = cJSON_GetObjectItemCaseSensitive(current, token);
        if (!cJSON_IsObject(next)) {
            cJSON_DeleteItemFromObjectCaseSensitive(current, token);
            next = cJSON_CreateObject();
            if (next == NULL) {
                free(token);
                cJSON_Delete(copy);
                return set_error(out_error, "INTERNAL", "not enough memory for the A2UI data path", ESP_ERR_NO_MEM);
            }
            cJSON_AddItemToObject(current, token, next);
        }
        free(token);
        current = next;
        cursor = slash + 1;
    }
}

static esp_err_t apply_data_model_update(
    cJSON *action,
    esp_openclaw_node_error_t *out_error)
{
    cJSON *surface = cJSON_GetObjectItemCaseSensitive(action, "surfaceId");
    cJSON *path = cJSON_GetObjectItemCaseSensitive(action, "path");
    cJSON *contents = cJSON_GetObjectItemCaseSensitive(action, "contents");
    if ((surface != NULL && (!cJSON_IsString(surface) || surface->valuestring == NULL)) ||
        (path != NULL && (!cJSON_IsString(path) || path->valuestring == NULL)) ||
        contents == NULL) {
        return set_error(
            out_error,
            "INVALID_PARAMS",
            "dataModelUpdate requires contents and optional string surfaceId and path",
            ESP_ERR_INVALID_ARG);
    }
    if (surface != NULL) {
        esp_err_t err = remember_surface_id(action);
        if (err != ESP_OK) {
            return set_error(
                out_error,
                err == ESP_ERR_NO_MEM ? "INTERNAL" : "INVALID_PARAMS",
                "dataModelUpdate surfaceId must be a non-empty string",
                err);
        }
    }
    return set_data_model_path(
        path != NULL ? path->valuestring : NULL,
        contents,
        out_error);
}

static esp_err_t apply_a2ui_message(
    cJSON *message,
    bool *out_begin_rendering,
    esp_openclaw_node_error_t *out_error)
{
    if (!cJSON_IsObject(message)) {
        return set_error(out_error, "INVALID_PARAMS", "each A2UI message must be an object", ESP_ERR_INVALID_ARG);
    }
    if (cJSON_GetObjectItemCaseSensitive(message, "version") != NULL ||
        cJSON_GetObjectItemCaseSensitive(message, "createSurface") != NULL) {
        return set_error(
            out_error,
            "INVALID_PARAMS",
            "A2UI v0.8 only; version and createSurface messages are not supported",
            ESP_ERR_INVALID_ARG);
    }
    if (cJSON_GetArraySize(message) != 1) {
        return set_error(
            out_error,
            "INVALID_PARAMS",
            "each A2UI v0.8 message must contain exactly one action key",
            ESP_ERR_INVALID_ARG);
    }

    cJSON *action = message->child;
    if (action == NULL || action->string == NULL || !cJSON_IsObject(action)) {
        return set_error(out_error, "INVALID_PARAMS", "A2UI action values must be objects", ESP_ERR_INVALID_ARG);
    }
    if (strcmp(action->string, "surfaceUpdate") == 0) {
        return apply_surface_update(action, out_error);
    }
    if (strcmp(action->string, "beginRendering") == 0) {
        esp_err_t err = apply_begin_rendering(action, out_error);
        if (err == ESP_OK) {
            *out_begin_rendering = true;
        }
        return err;
    }
    if (strcmp(action->string, "dataModelUpdate") == 0) {
        return apply_data_model_update(action, out_error);
    }
    if (strcmp(action->string, "deleteSurface") == 0) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(action, "surfaceId");
        if (!cJSON_IsString(id) || id->valuestring == NULL || id->valuestring[0] == '\0') {
            return set_error(out_error, "INVALID_PARAMS", "deleteSurface requires string surfaceId", ESP_ERR_INVALID_ARG);
        }
        clear_store();
        return ESP_OK;
    }
    return set_error(
        out_error,
        "INVALID_PARAMS",
        "A2UI v0.8 only; expected beginRendering, surfaceUpdate, dataModelUpdate, or deleteSurface",
        ESP_ERR_INVALID_ARG);
}

static esp_err_t validate_store_size(esp_openclaw_node_error_t *out_error)
{
    char *scratch = large_alloc(ROOM_CANVAS_MAX_STORE_BYTES + 6);
    if (scratch == NULL) {
        return set_error(
            out_error,
            "INTERNAL",
            "not enough memory to validate the A2UI store",
            ESP_ERR_NO_MEM);
    }

    size_t total = 0;
    for (size_t i = 0; i < component_count; ++i) {
        if (!cJSON_PrintPreallocated(
                components[i].component,
                scratch,
                ROOM_CANVAS_MAX_STORE_BYTES + 6,
                false)) {
            heap_caps_free(scratch);
            return set_error(
                out_error,
                "INVALID_PARAMS",
                "A2UI store exceeds the 262144-byte bound",
                ESP_ERR_INVALID_SIZE);
        }
        size_t item_size = strlen(components[i].id) + strlen(scratch) + 8;
        if (item_size > ROOM_CANVAS_MAX_STORE_BYTES - total) {
            heap_caps_free(scratch);
            return set_error(
                out_error,
                "INVALID_PARAMS",
                "A2UI store exceeds the 262144-byte bound",
                ESP_ERR_INVALID_SIZE);
        }
        total += item_size;
    }
    if (data_model != NULL) {
        if (!cJSON_PrintPreallocated(
                data_model,
                scratch,
                ROOM_CANVAS_MAX_STORE_BYTES + 6,
                false) ||
            strlen(scratch) > ROOM_CANVAS_MAX_STORE_BYTES - total) {
            heap_caps_free(scratch);
            return set_error(
                out_error,
                "INVALID_PARAMS",
                "A2UI store exceeds the 262144-byte bound",
                ESP_ERR_INVALID_SIZE);
        }
        total += strlen(scratch);
    }
    size_t metadata_size =
        (surface_id != NULL ? strlen(surface_id) : 0) +
        (root_component_id != NULL ? strlen(root_component_id) : 0);
    heap_caps_free(scratch);
    if (metadata_size > ROOM_CANVAS_MAX_STORE_BYTES - total) {
        return set_error(
            out_error,
            "INVALID_PARAMS",
            "A2UI store exceeds the 262144-byte bound",
            ESP_ERR_INVALID_SIZE);
    }
    return ESP_OK;
}

static esp_err_t apply_a2ui_array(
    const cJSON *messages,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    room_canvas_store_snapshot_t snapshot = {0};
    esp_err_t snapshot_err = copy_store_snapshot(&snapshot);
    if (snapshot_err != ESP_OK) {
        return set_error(
            out_error,
            "INTERNAL",
            "not enough memory to update the A2UI store safely",
            snapshot_err);
    }
    bool begin_rendering = false;
    cJSON *message = NULL;
    cJSON_ArrayForEach(message, messages) {
        esp_err_t err = apply_a2ui_message(message, &begin_rendering, out_error);
        if (err != ESP_OK) {
            restore_store_snapshot(&snapshot);
            return err;
        }
    }
    esp_err_t store_err = validate_store_size(out_error);
    if (store_err != ESP_OK) {
        restore_store_snapshot(&snapshot);
        return store_err;
    }
    if (root_component_id != NULL) {
        size_t node_budget = ROOM_CANVAS_MAX_RENDER_NODES;
        esp_err_t err = validate_component_depth(
            root_component_id,
            1,
            &node_budget,
            out_error);
        if (err != ESP_OK) {
            restore_store_snapshot(&snapshot);
            return err;
        }
    }
    esp_err_t err = render_a2ui_result(
        begin_rendering || canvas_active,
        out_payload_json,
        out_error);
    if (err != ESP_OK) {
        restore_store_snapshot(&snapshot);
        return err;
    }
    free_store_snapshot(&snapshot);
    return ESP_OK;
}

/* Tap on empty canvas background returns to the status screen. */
static void canvas_screen_clicked(lv_event_t *event)
{
    (void)event;
    char *payload = NULL;
    esp_openclaw_node_error_t error = {0};
    if (room_canvas_hide(&payload, &error) == ESP_OK) {
        free(payload);
    }
}

/*
 * Debug view cycling for the on-device buttons/touch: status -> canvas
 * (existing A2UI content or the placeholder) -> status. Safe from LVGL event
 * callbacks too: the display lock is a recursive mutex and neither branch
 * deletes the object dispatching the event.
 */
void room_canvas_debug_toggle(void)
{
    char *payload = NULL;
    esp_openclaw_node_error_t error = {0};
    if (canvas_active) {
        if (room_canvas_hide(&payload, &error) == ESP_OK) {
            free(payload);
        }
        room_ui_show_awake_hint();
        return;
    }
    if (root_component_id != NULL) {
        if (room_canvas_present(NULL, &payload, &error) == ESP_OK) {
            free(payload);
        }
        return;
    }
    /* Nothing pushed yet: cycle the two diagnostic views so the panel itself
     * can be judged (solid fill isolates the transfer path from rendering). */
    if (!bsp_display_lock(ROOM_CANVAS_DISPLAY_LOCK_MS)) {
        return;
    }
    static bool show_solid;
    if (show_solid) {
        show_solid_fill_locked();
    } else {
        show_test_card_locked();
    }
    show_solid = !show_solid;
    activate_canvas_locked();
    bsp_display_unlock();
    bsp_display_brightness_set(ROOM_CANVAS_BRIGHTNESS);
    room_ui_refresh();
}

esp_err_t room_canvas_init(void)
{
    if (!bsp_display_lock(ROOM_CANVAS_DISPLAY_LOCK_MS)) {
        ESP_LOGE(TAG, "failed to lock display for canvas initialization");
        return ESP_ERR_TIMEOUT;
    }
    status_screen = lv_screen_active();
    canvas_screen = lv_obj_create(NULL);
    if (canvas_screen == NULL) {
        bsp_display_unlock();
        ESP_LOGE(TAG, "failed to create canvas screen");
        return ESP_ERR_NO_MEM;
    }
    style_canvas_screen(ROOM_CANVAS_SAFE_PAD);
    lv_obj_add_event_cb(canvas_screen, canvas_screen_clicked, LV_EVENT_CLICKED, NULL);
    bsp_display_unlock();
    return ESP_OK;
}

void room_canvas_set_node(esp_openclaw_node_handle_t node)
{
    canvas_node = node;
}

void room_canvas_set_gateway_http_base(const char *base_url)
{
    char *copy = base_url != NULL && base_url[0] != '\0' ? strdup(base_url) : NULL;
    if (copy != NULL) {
        size_t len = strlen(copy);
        while (len > 0 && copy[len - 1] == '/') {
            copy[--len] = '\0';
        }
    }
    free(gateway_http_base);
    gateway_http_base = copy;
}

bool room_canvas_is_active(void)
{
    return canvas_active;
}

esp_err_t room_canvas_present(
    const char *url,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    if (canvas_screen == NULL) {
        return set_error(out_error, "UNAVAILABLE", "the canvas display is not initialized", ESP_ERR_INVALID_STATE);
    }
    if (url != NULL) {
        room_canvas_image_t image = {0};
        esp_err_t err = fetch_image(url, &image, out_error);
        if (err != ESP_OK) {
            return err;
        }
        return show_present_image(&image, out_payload_json, out_error);
    }
    return render_a2ui_result(true, out_payload_json, out_error);
}

esp_err_t room_canvas_hide(
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    if (canvas_screen == NULL || status_screen == NULL) {
        return set_error(out_error, "UNAVAILABLE", "the canvas display is not initialized", ESP_ERR_INVALID_STATE);
    }
    char *payload = strdup("{\"hidden\":true}");
    if (payload == NULL) {
        return set_error(out_error, "INTERNAL", "not enough memory for the command result", ESP_ERR_NO_MEM);
    }
    if (!bsp_display_lock(ROOM_CANVAS_DISPLAY_LOCK_MS)) {
        free(payload);
        return set_error(out_error, "UNAVAILABLE", "the display is busy; retry canvas.hide", ESP_ERR_TIMEOUT);
    }
    canvas_active = false;
    lv_screen_load(status_screen);
    bsp_display_unlock();
    /* Restore the live Talk state; forcing idle would blank an active call. */
    room_ui_refresh();
    *out_payload_json = payload;
    return ESP_OK;
}

esp_err_t room_canvas_snapshot(
    int max_width,
    double quality,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    if (canvas_screen == NULL) {
        return set_error(out_error, "UNAVAILABLE", "the display is not initialized", ESP_ERR_INVALID_STATE);
    }
    if (!bsp_display_lock(ROOM_CANVAS_DISPLAY_LOCK_MS)) {
        return set_error(out_error, "UNAVAILABLE", "the display is busy; retry canvas.snapshot", ESP_ERR_TIMEOUT);
    }
    lv_draw_buf_t *snapshot = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
    if (snapshot == NULL) {
        bsp_display_unlock();
        return set_error(out_error, "INTERNAL", "LVGL could not capture the active screen", ESP_FAIL);
    }

    int source_width = (int)snapshot->header.w;
    int source_height = (int)snapshot->header.h;
    int width = max_width > 0 && max_width < source_width ? max_width : source_width;
    int height = width < source_width
        ? (source_height * width) / source_width
        : source_height;
    if (height < 1) {
        height = 1;
    }
    size_t rgb_size = (size_t)width * (size_t)height * 3;
    uint8_t *rgb = large_aligned_alloc(16, rgb_size);
    if (rgb == NULL) {
        lv_draw_buf_destroy(snapshot);
        bsp_display_unlock();
        return set_error(out_error, "INTERNAL", "not enough memory for the snapshot pixels", ESP_ERR_NO_MEM);
    }
    for (int y = 0; y < height; ++y) {
        int source_y = (y * source_height) / height;
        const uint8_t *source_row = snapshot->data +
            ((size_t)source_y * snapshot->header.stride);
        uint8_t *target_row = rgb + ((size_t)y * (size_t)width * 3);
        for (int x = 0; x < width; ++x) {
            int source_x = (x * source_width) / width;
            const uint8_t *source_pixel = source_row + ((size_t)source_x * 3);
            uint8_t *target_pixel = target_row + ((size_t)x * 3);
            target_pixel[0] = source_pixel[2];
            target_pixel[1] = source_pixel[1];
            target_pixel[2] = source_pixel[0];
        }
    }
    lv_draw_buf_destroy(snapshot);
    bsp_display_unlock();

    jpeg_enc_config_t config = DEFAULT_JPEG_ENC_CONFIG();
    config.width = width;
    config.height = height;
    config.src_type = JPEG_PIXEL_FORMAT_RGB888;
    config.subsampling = JPEG_SUBSAMPLE_420;
    config.quality = (uint8_t)(1 + (int)(quality * 99.0));
    config.rotate = JPEG_ROTATE_0D;
    config.task_enable = false;
    jpeg_enc_handle_t encoder = NULL;
    if (jpeg_enc_open(&config, &encoder) != JPEG_ERR_OK || encoder == NULL) {
        heap_caps_free(rgb);
        return set_error(out_error, "INTERNAL", "could not initialize the JPEG encoder", ESP_FAIL);
    }

    size_t jpeg_capacity = rgb_size + 1024;
    uint8_t *jpeg = large_alloc(jpeg_capacity);
    int jpeg_size = 0;
    jpeg_error_t jpeg_err = jpeg != NULL
        ? jpeg_enc_process(
            encoder,
            rgb,
            (int)rgb_size,
            jpeg,
            (int)jpeg_capacity,
            &jpeg_size)
        : JPEG_ERR_NO_MEM;
    jpeg_enc_close(encoder);
    heap_caps_free(rgb);
    if (jpeg_err != JPEG_ERR_OK || jpeg_size <= 0) {
        heap_caps_free(jpeg);
        return set_error(
            out_error,
            jpeg_err == JPEG_ERR_NO_MEM ? "INTERNAL" : "INTERNAL",
            jpeg_err == JPEG_ERR_NO_MEM
                ? "not enough memory for the JPEG snapshot"
                : "the JPEG encoder could not encode the snapshot",
            jpeg_err == JPEG_ERR_NO_MEM ? ESP_ERR_NO_MEM : ESP_FAIL);
    }

    size_t base64_len = 4 * (((size_t)jpeg_size + 2) / 3);
    char prefix[64] = {0};
    int prefix_len = snprintf(prefix, sizeof(prefix), "{\"format\":\"jpeg\",\"base64\":\"");
    char suffix[80] = {0};
    int suffix_len = snprintf(
        suffix,
        sizeof(suffix),
        "\",\"width\":%d,\"height\":%d}",
        width,
        height);
    size_t payload_size = (size_t)prefix_len + base64_len + (size_t)suffix_len + 1;
    char *payload = large_alloc(payload_size);
    if (payload == NULL) {
        heap_caps_free(jpeg);
        return set_error(out_error, "INTERNAL", "not enough memory for the base64 snapshot", ESP_ERR_NO_MEM);
    }
    memcpy(payload, prefix, (size_t)prefix_len);
    size_t written = 0;
    int base64_err = mbedtls_base64_encode(
        (unsigned char *)payload + prefix_len,
        base64_len + 1,
        &written,
        jpeg,
        (size_t)jpeg_size);
    heap_caps_free(jpeg);
    if (base64_err != 0 || written != base64_len) {
        heap_caps_free(payload);
        return set_error(out_error, "INTERNAL", "could not base64-encode the snapshot", ESP_FAIL);
    }
    memcpy(payload + prefix_len + written, suffix, (size_t)suffix_len);
    payload[prefix_len + written + suffix_len] = '\0';
    *out_payload_json = payload;
    return ESP_OK;
}

esp_err_t room_canvas_a2ui_push_jsonl(
    const char *jsonl,
    size_t jsonl_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    if (jsonl_len > ROOM_CANVAS_MAX_JSONL_BYTES) {
        return set_error(
            out_error,
            "INVALID_PARAMS",
            "A2UI input exceeds the 262144-byte bound",
            ESP_ERR_INVALID_SIZE);
    }
    cJSON *messages = cJSON_CreateArray();
    if (messages == NULL) {
        return set_error(out_error, "INTERNAL", "not enough memory for A2UI JSONL", ESP_ERR_NO_MEM);
    }

    const char *cursor = jsonl;
    const char *end = jsonl + jsonl_len;
    while (cursor < end) {
        const char *line_end = memchr(cursor, '\n', (size_t)(end - cursor));
        if (line_end == NULL) {
            line_end = end;
        }
        const char *start = cursor;
        while (start < line_end && isspace((unsigned char)*start)) {
            ++start;
        }
        const char *trimmed_end = line_end;
        while (trimmed_end > start && isspace((unsigned char)trimmed_end[-1])) {
            --trimmed_end;
        }
        if (trimmed_end > start) {
            const char *parse_end = NULL;
            cJSON *message = cJSON_ParseWithLengthOpts(
                start,
                (size_t)(trimmed_end - start),
                &parse_end,
                false);
            if (message == NULL || parse_end != trimmed_end) {
                cJSON_Delete(message);
                cJSON_Delete(messages);
                return set_error(
                    out_error,
                    "INVALID_PARAMS",
                    "each non-empty A2UI JSONL line must contain one complete JSON object",
                    ESP_ERR_INVALID_ARG);
            }
            cJSON_AddItemToArray(messages, message);
        }
        cursor = line_end < end ? line_end + 1 : end;
    }
    esp_err_t err = apply_a2ui_array(messages, out_payload_json, out_error);
    cJSON_Delete(messages);
    return err;
}

esp_err_t room_canvas_a2ui_push_messages(
    const cJSON *messages,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    char *serialized = cJSON_PrintUnformatted(messages);
    if (serialized == NULL) {
        return set_error(out_error, "INTERNAL", "not enough memory to validate A2UI messages", ESP_ERR_NO_MEM);
    }
    size_t serialized_len = strlen(serialized);
    free(serialized);
    if (serialized_len > ROOM_CANVAS_MAX_JSONL_BYTES) {
        return set_error(
            out_error,
            "INVALID_PARAMS",
            "A2UI input exceeds the 262144-byte bound",
            ESP_ERR_INVALID_SIZE);
    }
    return apply_a2ui_array(messages, out_payload_json, out_error);
}

esp_err_t room_canvas_a2ui_reset(
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    char *payload = strdup("{\"reset\":true}");
    if (payload == NULL) {
        return set_error(out_error, "INTERNAL", "not enough memory for the command result", ESP_ERR_NO_MEM);
    }
    if (!bsp_display_lock(ROOM_CANVAS_DISPLAY_LOCK_MS)) {
        free(payload);
        return set_error(out_error, "UNAVAILABLE", "the display is busy; retry canvas.a2ui.reset", ESP_ERR_TIMEOUT);
    }
    lv_obj_clean(canvas_screen);
    clear_images_locked();
    clear_store();
    if (canvas_active) {
        show_placeholder_locked();
    }
    bsp_display_unlock();
    if (canvas_active) {
        bsp_display_brightness_set(ROOM_CANVAS_BRIGHTNESS);
    }
    *out_payload_json = payload;
    return ESP_OK;
}
