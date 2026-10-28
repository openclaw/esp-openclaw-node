/* SPDX-License-Identifier: Apache-2.0 */
#include "esp_openclaw_transcription.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define SESSION_BYTES 129U
#define JSON_BYTES 8192U
#define REQUEST_TIMEOUT_US (15LL * 1000 * 1000)
#define DRAIN_TIMEOUT_US (10LL * 1000 * 1000)

typedef enum { REQUEST_NONE, REQUEST_CREATE, REQUEST_AUDIO, REQUEST_CLOSE } request_t;
typedef struct {
    char id[SESSION_BYTES];
    bool ready;
    bool failed;
    uint32_t seq;
} early_event_t;

struct esp_openclaw_transcription {
    SemaphoreHandle_t lock;
    atomic_uint refs;
    esp_openclaw_node_handle_t node;
    uint32_t connection;
    esp_openclaw_transcription_snapshot_t snapshot;
    char id[SESSION_BYTES];
    /* A provider may become ready before the create RPC response. The Gateway
     * permits at most two transcription sessions on one connection. */
    early_event_t early[2];
    uint32_t seq;
    request_t pending;
    int64_t deadline;
    bool close_sent;
    bool closed;
};

static void release(esp_openclaw_transcription_handle_t h)
{
    if (atomic_fetch_sub(&h->refs, 1) == 1) {
        vSemaphoreDelete(h->lock);
        free(h);
    }
}

static bool terminal(esp_openclaw_transcription_handle_t h)
{
    return h->snapshot.state >= ESP_OPENCLAW_TRANSCRIPTION_COMPLETE;
}

static void fail(esp_openclaw_transcription_handle_t h, esp_openclaw_transcription_error_t error)
{
    if (terminal(h)) return;
    h->snapshot.state = ESP_OPENCLAW_TRANSCRIPTION_FAILED;
    h->snapshot.error = error;
    h->snapshot.text[0] = '\0';
}

static void set_ready(esp_openclaw_transcription_handle_t h)
{
    h->snapshot.state = ESP_OPENCLAW_TRANSCRIPTION_READY;
    h->deadline = esp_timer_get_time() + REQUEST_TIMEOUT_US;
}

static bool string_is(const cJSON *root, const char *key, const char *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) && strcmp(item->valuestring, value) == 0;
}

static cJSON *parse(const char *json)
{
    if (json == NULL || strnlen(json, JSON_BYTES + 1) > JSON_BYTES) return NULL;
    return cJSON_ParseWithLengthOpts(json, strlen(json) + 1, NULL, true);
}

static void response(esp_openclaw_node_handle_t node,
    const esp_openclaw_node_gateway_result_t *result, void *ctx);

/* Node submission copies params and never invokes a callback inline. Keeping
 * admission under this mutex orders cancellation against queue submission. */
static esp_err_t request(esp_openclaw_transcription_handle_t h, request_t kind, cJSON *params)
{
    char *json = params != NULL ? cJSON_PrintUnformatted(params) : NULL;
    if (json == NULL) return ESP_ERR_NO_MEM;
    const char *method = kind == REQUEST_CREATE ? "talk.session.create" :
        kind == REQUEST_AUDIO ? "talk.session.appendAudio" : "talk.session.close";
    h->pending = kind;
    atomic_fetch_add(&h->refs, 1);
    esp_err_t err = esp_openclaw_node_gateway_request_on_connection(
        h->node, h->connection, method, json, response, h);
    free(json);
    if (err != ESP_OK) {
        h->pending = REQUEST_NONE;
        release(h);
    } else if (kind != REQUEST_CLOSE || !terminal(h)) {
        h->deadline = esp_timer_get_time() +
            (kind == REQUEST_CLOSE ? DRAIN_TIMEOUT_US : REQUEST_TIMEOUT_US);
    }
    return err;
}

static esp_err_t close_session(esp_openclaw_transcription_handle_t h)
{
    if (h->close_sent || h->closed || h->pending != REQUEST_NONE || h->id[0] == '\0') return ESP_OK;
    cJSON *params = cJSON_CreateObject();
    if (params == NULL || cJSON_AddStringToObject(params, "sessionId", h->id) == NULL) {
        cJSON_Delete(params);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = request(h, REQUEST_CLOSE, params);
    /* A rejected queue submission sent nothing. Only an accepted close is
     * non-idempotent and must never be replayed. */
    if (err == ESP_OK) h->close_sent = true;
    cJSON_Delete(params);
    return err;
}

static bool connection_current(esp_openclaw_transcription_handle_t h)
{
    return esp_openclaw_node_get_connection_id(h->node) == h->connection;
}

static void response(esp_openclaw_node_handle_t node,
    const esp_openclaw_node_gateway_result_t *result, void *ctx)
{
    esp_openclaw_transcription_handle_t h = ctx;
    cJSON *root = result->ok ? parse(result->payload_json) : NULL;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    request_t kind = h->pending;
    h->pending = REQUEST_NONE;
    if (node != h->node || !connection_current(h)) {
        fail(h, ESP_OPENCLAW_TRANSCRIPTION_ERROR_CONNECTION);
    } else if (!result->ok) {
        fail(h, ESP_OPENCLAW_TRANSCRIPTION_ERROR_GATEWAY);
    } else if (kind == REQUEST_CREATE) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "sessionId");
        if (cJSON_IsString(id) && id->valuestring[0] != '\0' &&
            strlen(id->valuestring) < sizeof(h->id)) {
            strcpy(h->id, id->valuestring);
        }
        const cJSON *audio = cJSON_GetObjectItemCaseSensitive(root, "audio");
        const cJSON *rate = cJSON_GetObjectItemCaseSensitive(audio, "inputSampleRateHz");
        if (h->id[0] == '\0' || !string_is(root, "transcriptionSessionId", h->id) ||
            !string_is(root, "mode", "transcription") || !string_is(root, "transport", "gateway-relay") ||
            !string_is(root, "brain", "none") || !string_is(audio, "inputEncoding", "g711_ulaw") ||
            !cJSON_IsNumber(rate) || rate->valuedouble != 8000) {
            fail(h, ESP_OPENCLAW_TRANSCRIPTION_ERROR_FORMAT);
        }
        for (size_t i = 0; i < 2; ++i) {
            if (strcmp(h->early[i].id, h->id) != 0) continue;
            h->seq = h->early[i].seq;
            if (h->early[i].failed) fail(h, ESP_OPENCLAW_TRANSCRIPTION_ERROR_GATEWAY);
            if (h->early[i].ready && !terminal(h)) set_ready(h);
        }
    } else {
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok"))) {
            fail(h, ESP_OPENCLAW_TRANSCRIPTION_ERROR_FORMAT);
        } else if (kind == REQUEST_AUDIO && !terminal(h)) {
            set_ready(h);
        }
    }
    if (terminal(h) && h->snapshot.state != ESP_OPENCLAW_TRANSCRIPTION_COMPLETE) (void)close_session(h);
    xSemaphoreGive(h->lock);
    cJSON_Delete(root);
    release(h);
}

esp_err_t esp_openclaw_transcription_begin(esp_openclaw_node_handle_t node,
    esp_openclaw_transcription_handle_t *out)
{
    if (node == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    uint32_t connection = esp_openclaw_node_get_connection_id(node);
    if (connection == 0) return ESP_ERR_INVALID_STATE;
    esp_openclaw_transcription_handle_t h = calloc(1, sizeof(*h));
    if (h == NULL) return ESP_ERR_NO_MEM;
    h->lock = xSemaphoreCreateMutex();
    if (h->lock == NULL) { free(h); return ESP_ERR_NO_MEM; }
    atomic_init(&h->refs, 1);
    h->node = node;
    h->connection = connection;
    cJSON *params = cJSON_CreateObject();
    if (params == NULL || cJSON_AddStringToObject(params, "mode", "transcription") == NULL ||
        cJSON_AddStringToObject(params, "transport", "gateway-relay") == NULL ||
        cJSON_AddStringToObject(params, "brain", "none") == NULL) {
        cJSON_Delete(params);
        release(h);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(h->lock, portMAX_DELAY);
    esp_err_t err = request(h, REQUEST_CREATE, params);
    xSemaphoreGive(h->lock);
    cJSON_Delete(params);
    if (err != ESP_OK) { release(h); return err; }
    *out = h;
    return ESP_OK;
}

esp_err_t esp_openclaw_transcription_append(esp_openclaw_transcription_handle_t h,
    const uint8_t *audio, size_t length)
{
    if (h == NULL || audio == NULL || length == 0 || length > ESP_OPENCLAW_TRANSCRIPTION_CHUNK_BYTES)
        return ESP_ERR_INVALID_ARG;
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *encoded = malloc(4 * ((length + 2) / 3) + 1);
    if (encoded == NULL) return ESP_ERR_NO_MEM;
    size_t n = 0;
    for (size_t i = 0; i < length; i += 3) {
        uint32_t value = (uint32_t)audio[i] << 16;
        if (i + 1 < length) value |= (uint32_t)audio[i + 1] << 8;
        if (i + 2 < length) value |= audio[i + 2];
        encoded[n++] = alphabet[value >> 18];
        encoded[n++] = alphabet[(value >> 12) & 63];
        encoded[n++] = i + 1 < length ? alphabet[(value >> 6) & 63] : '=';
        encoded[n++] = i + 2 < length ? alphabet[value & 63] : '=';
    }
    encoded[n] = '\0';
    cJSON *params = cJSON_CreateObject();
    esp_err_t err = ESP_ERR_NO_MEM;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    if (h->snapshot.state != ESP_OPENCLAW_TRANSCRIPTION_READY || h->pending != REQUEST_NONE) {
        err = ESP_ERR_INVALID_STATE;
    } else if (length > ESP_OPENCLAW_TRANSCRIPTION_AUDIO_BYTES - h->snapshot.audio_bytes) {
        err = ESP_ERR_INVALID_ARG;
    } else if (params != NULL && cJSON_AddStringToObject(params, "sessionId", h->id) != NULL &&
        cJSON_AddStringToObject(params, "audioBase64", encoded) != NULL) {
        err = request(h, REQUEST_AUDIO, params);
        if (err == ESP_OK) {
            h->snapshot.audio_bytes += length;
            h->snapshot.state = ESP_OPENCLAW_TRANSCRIPTION_SENDING;
        } else {
            fail(h, ESP_OPENCLAW_TRANSCRIPTION_ERROR_GATEWAY);
        }
    }
    xSemaphoreGive(h->lock);
    cJSON_Delete(params);
    free(encoded);
    return err;
}

esp_err_t esp_openclaw_transcription_finish(esp_openclaw_transcription_handle_t h)
{
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (h->snapshot.state == ESP_OPENCLAW_TRANSCRIPTION_READY && h->pending == REQUEST_NONE) {
        h->snapshot.state = ESP_OPENCLAW_TRANSCRIPTION_FINISHING;
        err = close_session(h);
        if (err != ESP_OK) fail(h, ESP_OPENCLAW_TRANSCRIPTION_ERROR_GATEWAY);
    }
    xSemaphoreGive(h->lock);
    return err;
}

void esp_openclaw_transcription_event(esp_openclaw_transcription_handle_t h,
    esp_openclaw_node_handle_t source, const char *event, const char *payload_json)
{
    if (h == NULL || source != h->node || event == NULL || strcmp(event, "talk.event") != 0) return;
    cJSON *root = parse(payload_json);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "transcriptionSessionId");
    const cJSON *inner = cJSON_GetObjectItemCaseSensitive(root, "talkEvent");
    const cJSON *seq = cJSON_GetObjectItemCaseSensitive(inner, "seq");
    if (!cJSON_IsString(id) || id->valuestring[0] == '\0' || strlen(id->valuestring) >= SESSION_BYTES ||
        !string_is(inner, "sessionId", id->valuestring) || !string_is(inner, "mode", "transcription") ||
        !string_is(inner, "transport", "gateway-relay") || !string_is(inner, "brain", "none") ||
        !cJSON_IsNumber(seq) || seq->valuedouble < 1 || seq->valuedouble > UINT32_MAX ||
        seq->valuedouble != (uint32_t)seq->valuedouble) { cJSON_Delete(root); return; }
    bool ready = string_is(inner, "type", "session.ready");
    bool error = string_is(inner, "type", "session.error");
    bool closed = string_is(inner, "type", "session.closed");
    xSemaphoreTake(h->lock, portMAX_DELAY);
    if (!connection_current(h)) fail(h, ESP_OPENCLAW_TRANSCRIPTION_ERROR_CONNECTION);
    if (!terminal(h) && h->id[0] == '\0' && (ready || error || closed)) {
        for (size_t i = 0; i < 2; ++i) {
            early_event_t *e = &h->early[i];
            if (e->id[0] != '\0' && strcmp(e->id, id->valuestring) != 0) continue;
            strcpy(e->id, id->valuestring);
            e->ready |= ready;
            e->failed |= error || closed;
            if (seq->valuedouble > e->seq) e->seq = (uint32_t)seq->valuedouble;
            break;
        }
    } else if (!terminal(h) && strcmp(h->id, id->valuestring) == 0 && seq->valuedouble > h->seq) {
        h->seq = (uint32_t)seq->valuedouble;
        if (error) fail(h, ESP_OPENCLAW_TRANSCRIPTION_ERROR_GATEWAY);
        else if (ready && h->snapshot.state == ESP_OPENCLAW_TRANSCRIPTION_STARTING)
            set_ready(h);
        else if (closed) {
            h->closed = true;
            const cJSON *payload = cJSON_GetObjectItemCaseSensitive(inner, "payload");
            if (h->snapshot.state == ESP_OPENCLAW_TRANSCRIPTION_FINISHING &&
                string_is(payload, "reason", "completed")) h->snapshot.state = ESP_OPENCLAW_TRANSCRIPTION_COMPLETE;
            else fail(h, ESP_OPENCLAW_TRANSCRIPTION_ERROR_GATEWAY);
        } else if (string_is(inner, "type", "transcript.done")) {
            const cJSON *payload = cJSON_GetObjectItemCaseSensitive(inner, "payload");
            const cJSON *text = cJSON_GetObjectItemCaseSensitive(payload, "text");
            if (!cJSON_IsString(text)) fail(h, ESP_OPENCLAW_TRANSCRIPTION_ERROR_FORMAT);
            else {
                size_t have = strlen(h->snapshot.text), length = strlen(text->valuestring);
                size_t separator = have != 0 && length != 0 ? 1 : 0;
                if (length + separator >= sizeof(h->snapshot.text) - have)
                    fail(h, ESP_OPENCLAW_TRANSCRIPTION_ERROR_TOO_LONG);
                else if (length != 0) {
                    if (separator) h->snapshot.text[have++] = ' ';
                    memcpy(h->snapshot.text + have, text->valuestring, length + 1);
                }
            }
        }
    }
    if (terminal(h) && h->snapshot.state != ESP_OPENCLAW_TRANSCRIPTION_COMPLETE) (void)close_session(h);
    xSemaphoreGive(h->lock);
    cJSON_Delete(root);
}

void esp_openclaw_transcription_poll(esp_openclaw_transcription_handle_t h)
{
    if (h == NULL) return;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    if (!connection_current(h)) fail(h, ESP_OPENCLAW_TRANSCRIPTION_ERROR_CONNECTION);
    else if (!terminal(h) && esp_timer_get_time() >= h->deadline)
        fail(h, ESP_OPENCLAW_TRANSCRIPTION_ERROR_TIMEOUT);
    if (terminal(h) && h->snapshot.state != ESP_OPENCLAW_TRANSCRIPTION_COMPLETE) (void)close_session(h);
    xSemaphoreGive(h->lock);
}

void esp_openclaw_transcription_copy(esp_openclaw_transcription_handle_t h,
    esp_openclaw_transcription_snapshot_t *out)
{
    if (h == NULL || out == NULL) return;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    *out = h->snapshot;
    xSemaphoreGive(h->lock);
}

void esp_openclaw_transcription_cancel(esp_openclaw_transcription_handle_t h)
{
    if (h == NULL) return;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    h->snapshot.state = ESP_OPENCLAW_TRANSCRIPTION_CANCELED;
    h->snapshot.text[0] = '\0';
    (void)close_session(h);
    xSemaphoreGive(h->lock);
}

esp_err_t esp_openclaw_transcription_destroy(esp_openclaw_transcription_handle_t h)
{
    if (h == NULL) return ESP_OK;
    esp_openclaw_transcription_cancel(h);
    xSemaphoreTake(h->lock, portMAX_DELAY);
    bool needs_cleanup = connection_current(h) &&
        (h->pending == REQUEST_CREATE || h->pending == REQUEST_AUDIO ||
         (h->id[0] != '\0' && !h->closed && !h->close_sent));
    xSemaphoreGive(h->lock);
    if (needs_cleanup) return ESP_ERR_INVALID_STATE;
    release(h);
    return ESP_OK;
}
