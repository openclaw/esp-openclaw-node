/* SPDX-License-Identifier: Apache-2.0 */
#include "esp_openclaw_chat.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define JSON_BYTES 16384U
#define RPC_TIMEOUT_US (30LL * 1000 * 1000)
#define RUN_IDLE_US (300LL * 1000 * 1000)

struct esp_openclaw_chat {
    SemaphoreHandle_t lock;
    atomic_uint refs;
    esp_openclaw_node_handle_t node;
    uint32_t connection;
    esp_openclaw_chat_snapshot_t snapshot;
    char leaf[ESP_OPENCLAW_CHAT_ID_BYTES];
    bool closed, pending, submitted, have_seq;
    uint32_t seq;
    int64_t deadline;
};

static void release(esp_openclaw_chat_handle_t h)
{
    if (atomic_fetch_sub(&h->refs, 1) == 1) {
        vSemaphoreDelete(h->lock);
        free(h);
    }
}

static bool utf8(const char *text, size_t length)
{
    for (size_t i = 0; i < length;) {
        unsigned char first = (unsigned char)text[i++];
        if (first == 0) return false;
        if (first < 0x80) continue;
        size_t tail = first >= 0xc2 && first <= 0xdf ? 1 :
            first >= 0xe0 && first <= 0xef ? 2 : first >= 0xf0 && first <= 0xf4 ? 3 : 0;
        if (tail == 0 || tail > length - i) return false;
        unsigned char second = (unsigned char)text[i];
        if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0) ||
            (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90)) return false;
        for (size_t j = 0; j < tail; ++j) {
            if (((unsigned char)text[i++] & 0xc0) != 0x80) return false;
        }
    }
    return true;
}

static bool bounded_string(const char *s, size_t capacity)
{
    if (s == NULL) return false;
    size_t length = strnlen(s, capacity);
    return length > 0 && length < capacity && utf8(s, length);
}

static bool unique_fields(const cJSON *value)
{
    const cJSON *item;
    cJSON_ArrayForEach(item, value) {
        if (cJSON_IsObject(value)) {
            for (const cJSON *other = item->next; other != NULL; other = other->next) {
                if (strcmp(item->string, other->string) == 0) return false;
            }
        }
        if ((cJSON_IsObject(item) || cJSON_IsArray(item)) && !unique_fields(item)) return false;
    }
    return true;
}

/* Bound cJSON's recursive parse and reject escaped NUL before it can truncate
 * a routing identity. JSON syntax itself is still validated by cJSON. */
static cJSON *parse(const char *json)
{
    if (json == NULL) return NULL;
    size_t length = strnlen(json, JSON_BYTES + 1);
    if (length == 0 || length > JSON_BYTES || !utf8(json, length)) return NULL;
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < length; ++i) {
        char c = json[i];
        if (quoted) {
            if (escaped) {
                if (c == 'u' && length - i > 4 && memcmp(json + i + 1, "0000", 4) == 0)
                    return NULL;
                escaped = false;
            } else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 16) return NULL; }
        else if (c == '}' || c == ']') { if (depth == 0) return NULL; --depth; }
    }
    if (depth != 0 || quoted) return NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length + 1, NULL, true);
    if (!cJSON_IsObject(root) || !unique_fields(root)) { cJSON_Delete(root); return NULL; }
    return root;
}

static const cJSON *field(const cJSON *object, const char *name)
{
    if (!cJSON_IsObject(object)) return NULL;
    const cJSON *found = NULL, *item;
    cJSON_ArrayForEach(item, object) {
        if (item->string != NULL && strcmp(item->string, name) == 0) {
            if (found != NULL) return NULL;
            found = item;
        }
    }
    return found;
}

static const char *string(const cJSON *object, const char *name, size_t capacity)
{
    const cJSON *item = field(object, name);
    return cJSON_IsString(item) && bounded_string(item->valuestring, capacity) ? item->valuestring : NULL;
}

static bool is(const cJSON *object, const char *name, const char *value)
{
    const char *s = string(object, name, JSON_BYTES);
    return s != NULL && strcmp(s, value) == 0;
}

static bool terminal(esp_openclaw_chat_handle_t h)
{
    return h->snapshot.state >= ESP_OPENCLAW_CHAT_COMPLETE &&
        h->snapshot.state != ESP_OPENCLAW_CHAT_OUTCOME_UNKNOWN;
}

static void lost(esp_openclaw_chat_handle_t h, esp_openclaw_chat_error_t error)
{
    if (terminal(h)) return;
    h->snapshot.state = h->submitted ? ESP_OPENCLAW_CHAT_OUTCOME_UNKNOWN : ESP_OPENCLAW_CHAT_FAILED;
    h->snapshot.error = error;
    h->deadline = 0;
}

static void state(esp_openclaw_chat_handle_t h, esp_openclaw_chat_state_t value)
{
    h->snapshot.state = value;
    h->snapshot.error = ESP_OPENCLAW_CHAT_ERROR_NONE;
    h->deadline = value == ESP_OPENCLAW_CHAT_RUNNING ? esp_timer_get_time() + RUN_IDLE_US : 0;
}

static bool current(esp_openclaw_chat_handle_t h)
{
    return esp_openclaw_node_get_connection_id(h->node) == h->connection;
}

static void resolve(esp_openclaw_chat_handle_t h, const cJSON *root)
{
    const cJSON *info = field(root, "sessionInfo");
    const char *key = string(info, "key", sizeof(h->snapshot.session_key));
    const char *agent = string(info, "agentId", sizeof(h->snapshot.agent_id));
    const char *id = string(root, "sessionId", sizeof(h->snapshot.session_id));
    const cJSON *leaf = field(info, "activeLeafEntryId");
    const char *leaf_id = string(info, "activeLeafEntryId", sizeof(h->leaf));
    if (key == NULL || agent == NULL || id == NULL ||
        strcmp(agent, h->snapshot.agent_id) != 0 ||
        !is(info, "sessionId", id) ||
        (!cJSON_IsNull(leaf) && leaf_id == NULL)) {
        lost(h, ESP_OPENCLAW_CHAT_ERROR_FORMAT);
        return;
    }
    strcpy(h->snapshot.session_key, key);
    strcpy(h->snapshot.session_id, id);
    if (leaf_id != NULL) strcpy(h->leaf, leaf_id);
    state(h, ESP_OPENCLAW_CHAT_READY);
}

static void response(esp_openclaw_node_handle_t node,
    const esp_openclaw_node_gateway_result_t *result, void *ctx)
{
    esp_openclaw_chat_handle_t h = ctx;
    cJSON *root = parse(result->payload_json);
    xSemaphoreTake(h->lock, portMAX_DELAY);
    h->pending = false;
    if (!h->closed && !terminal(h)) {
        if (node != h->node || !current(h)) lost(h, ESP_OPENCLAW_CHAT_ERROR_CONNECTION);
        else if (!h->submitted) {
            if (!result->ok) lost(h, ESP_OPENCLAW_CHAT_ERROR_GATEWAY);
            else resolve(h, root);
        } else if (!is(root, "runId", h->snapshot.run_id)) {
            lost(h, result->ok ? ESP_OPENCLAW_CHAT_ERROR_FORMAT : ESP_OPENCLAW_CHAT_ERROR_GATEWAY);
        } else if (result->ok && is(root, "status", "ok")) state(h, ESP_OPENCLAW_CHAT_COMPLETE);
        else if (is(root, "status", "aborted")) state(h, ESP_OPENCLAW_CHAT_ABORTED);
        else if (is(root, "status", "error") || is(root, "status", "timeout")) {
            state(h, ESP_OPENCLAW_CHAT_FAILED);
            h->snapshot.error = ESP_OPENCLAW_CHAT_ERROR_GATEWAY;
        } else if (result->ok && (is(root, "status", "started") || is(root, "status", "in_flight"))) {
            state(h, ESP_OPENCLAW_CHAT_RUNNING);
        } else lost(h, ESP_OPENCLAW_CHAT_ERROR_FORMAT);
    }
    xSemaphoreGive(h->lock);
    cJSON_Delete(root);
    release(h);
}

/* Node copies params and never calls back inline. Admission under this lock
 * orders early events and destruction against the captured request identity. */
static esp_err_t request(esp_openclaw_chat_handle_t h, const char *method, cJSON *params)
{
    char *json = params != NULL ? cJSON_PrintUnformatted(params) : NULL;
    if (json == NULL) return ESP_ERR_NO_MEM;
    atomic_fetch_add(&h->refs, 1);
    esp_err_t err = esp_openclaw_node_gateway_request_on_connection(
        h->node, h->connection, method, json, response, h);
    free(json);
    if (err != ESP_OK) release(h);
    else {
        h->pending = true;
        h->deadline = esp_timer_get_time() + RPC_TIMEOUT_US;
    }
    return err;
}

esp_err_t esp_openclaw_chat_prepare(esp_openclaw_node_handle_t node,
    const char *session_key, const char *agent_id, esp_openclaw_chat_handle_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (node == NULL || !bounded_string(session_key, ESP_OPENCLAW_CHAT_KEY_BYTES) ||
        !bounded_string(agent_id, 65)) return ESP_ERR_INVALID_ARG;
    uint32_t connection = esp_openclaw_node_get_connection_id(node);
    if (connection == 0) return ESP_ERR_INVALID_STATE;
    esp_openclaw_chat_handle_t h = calloc(1, sizeof(*h));
    if (h == NULL) return ESP_ERR_NO_MEM;
    h->lock = xSemaphoreCreateMutex();
    if (h->lock == NULL) { free(h); return ESP_ERR_NO_MEM; }
    atomic_init(&h->refs, 1);
    h->node = node;
    h->connection = connection;
    strcpy(h->snapshot.agent_id, agent_id);
    strcpy(h->snapshot.session_key, session_key);
    cJSON *params = cJSON_CreateObject();
    esp_err_t err = ESP_ERR_NO_MEM;
    if (params != NULL && cJSON_AddStringToObject(params, "sessionKey", session_key) != NULL &&
        cJSON_AddStringToObject(params, "agentId", agent_id) != NULL &&
        cJSON_AddNumberToObject(params, "limit", 1) != NULL &&
        cJSON_AddNumberToObject(params, "maxBytes", 4096) != NULL &&
        cJSON_AddNumberToObject(params, "maxChars", 1) != NULL) {
        xSemaphoreTake(h->lock, portMAX_DELAY);
        err = request(h, "chat.history", params);
        xSemaphoreGive(h->lock);
    }
    cJSON_Delete(params);
    if (err != ESP_OK) { release(h); return err; }
    *out = h;
    return ESP_OK;
}

esp_err_t esp_openclaw_chat_send(esp_openclaw_chat_handle_t h,
    const char *text, const char *idempotency_key)
{
    if (h == NULL || !bounded_string(text, ESP_OPENCLAW_CHAT_TEXT_BYTES) ||
        !bounded_string(idempotency_key, ESP_OPENCLAW_CHAT_ID_BYTES)) return ESP_ERR_INVALID_ARG;
    bool nonspace = false;
    for (const char *p = text; *p != '\0'; ++p) {
        if (*p != ' ' && *p != '\n' && *p != '\t' && *p != '\r') nonspace = true;
    }
    if (!nonspace) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!h->closed && h->snapshot.state == ESP_OPENCLAW_CHAT_READY && !h->pending && current(h)) {
        cJSON *params = cJSON_CreateObject();
        err = ESP_ERR_NO_MEM;
        if (params != NULL &&
            cJSON_AddStringToObject(params, "sessionKey", h->snapshot.session_key) != NULL &&
            cJSON_AddStringToObject(params, "agentId", h->snapshot.agent_id) != NULL &&
            cJSON_AddStringToObject(params, "sessionId", h->snapshot.session_id) != NULL &&
            (h->leaf[0] != '\0' ? cJSON_AddStringToObject(params, "expectedLeafEntryId", h->leaf) :
                cJSON_AddNullToObject(params, "expectedLeafEntryId")) != NULL &&
            cJSON_AddStringToObject(params, "message", text) != NULL &&
            cJSON_AddStringToObject(params, "idempotencyKey", idempotency_key) != NULL &&
            cJSON_AddStringToObject(params, "queueMode", "followup") != NULL &&
            cJSON_AddBoolToObject(params, "deliver", false) != NULL) {
            err = request(h, "chat.send", params);
            if (err == ESP_OK) {
                strcpy(h->snapshot.run_id, idempotency_key);
                h->submitted = true;
                h->snapshot.state = ESP_OPENCLAW_CHAT_SENDING;
            }
        }
        cJSON_Delete(params);
    }
    xSemaphoreGive(h->lock);
    return err;
}

void esp_openclaw_chat_event(esp_openclaw_chat_handle_t h,
    esp_openclaw_node_handle_t node, const char *event, const char *payload_json)
{
    if (h == NULL || event == NULL || strcmp(event, "chat") != 0) return;
    cJSON *root = parse(payload_json);
    if (root == NULL) return;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    const cJSON *seq = field(root, "seq");
    bool sequence = cJSON_IsNumber(seq) && seq->valuedouble >= 0 && seq->valuedouble <= UINT32_MAX;
    uint32_t number = sequence ? (uint32_t)seq->valuedouble : 0;
    sequence = sequence && (double)number == seq->valuedouble;
    /* agentId is optional in the protocol; when present it must match. */
    const cJSON *agent = field(root, "agentId");
    bool agent_matches = agent == NULL || is(root, "agentId", h->snapshot.agent_id);
    if (!h->closed && h->submitted && !terminal(h) && node == h->node && current(h) &&
        sequence && (!h->have_seq || number > h->seq) && agent_matches &&
        is(root, "runId", h->snapshot.run_id) && is(root, "sessionKey", h->snapshot.session_key)) {
        if (is(root, "state", "status") || is(root, "state", "delta")) state(h, ESP_OPENCLAW_CHAT_RUNNING);
        else if (is(root, "state", "final")) {
            state(h, cJSON_IsTrue(field(root, "yielded")) ?
                ESP_OPENCLAW_CHAT_HANDED_OFF : ESP_OPENCLAW_CHAT_COMPLETE);
        } else if (is(root, "state", "aborted")) state(h, ESP_OPENCLAW_CHAT_ABORTED);
        else if (is(root, "state", "error")) {
            state(h, ESP_OPENCLAW_CHAT_FAILED);
            h->snapshot.error = ESP_OPENCLAW_CHAT_ERROR_GATEWAY;
        } else goto done;
        h->seq = number;
        h->have_seq = true;
    }
done:
    xSemaphoreGive(h->lock);
    cJSON_Delete(root);
}

void esp_openclaw_chat_poll(esp_openclaw_chat_handle_t h)
{
    if (h == NULL) return;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    if (!h->closed && !terminal(h)) {
        if (!current(h)) lost(h, ESP_OPENCLAW_CHAT_ERROR_CONNECTION);
        else if (h->deadline != 0 && esp_timer_get_time() >= h->deadline)
            lost(h, ESP_OPENCLAW_CHAT_ERROR_TIMEOUT);
    }
    xSemaphoreGive(h->lock);
}

void esp_openclaw_chat_copy(esp_openclaw_chat_handle_t h, esp_openclaw_chat_snapshot_t *out)
{
    if (h == NULL || out == NULL) return;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    *out = h->snapshot;
    xSemaphoreGive(h->lock);
}

void esp_openclaw_chat_destroy(esp_openclaw_chat_handle_t h)
{
    if (h == NULL) return;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    h->closed = true;
    xSemaphoreGive(h->lock);
    release(h);
}
