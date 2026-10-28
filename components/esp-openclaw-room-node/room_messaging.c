#include "room_messaging.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_timer.h"
#include "sdkconfig.h"

typedef struct {
    unsigned intent;
    esp_openclaw_node_handle_t client;
    uint64_t incarnation;
    uint32_t connection;
    bool waiting;
    char *json;
} approval_t;

static struct {
    SemaphoreHandle_t lock;
    void (*wake)(void);
    esp_openclaw_node_handle_t client;
    uint64_t incarnation;
    uint32_t connection;
    bool connected, write_registered, write_approved, reset_write, disconnect_queued, connect_queued;
    room_messaging_snapshot_t view;
    approval_t *inflight;
    char *uri, *token;
    int64_t deadline;
    unsigned intent;
} messaging;
static _Atomic(approval_t *) completed;

static void state(room_messaging_state_t value)
{
    messaging.view.state = value;
    ++messaging.view.revision;
}

static void clear_token(void)
{
    if (messaging.token != NULL) memset(messaging.token, 0, strlen(messaging.token));
    free(messaging.token);
    messaging.token = NULL;
}

void room_messaging_init(SemaphoreHandle_t lock, void (*wake)(void))
{
    messaging.lock = lock;
    messaging.wake = wake;
#if CONFIG_OPENCLAW_ROOM_VOICE_ACTIONS
    messaging.write_registered = messaging.write_approved = true;
#endif
}

bool room_messaging_ready_locked(void)
{
    return messaging.connected && messaging.write_registered && messaging.write_approved &&
        messaging.view.state == ROOM_MESSAGING_READY;
}

bool room_messaging_busy_locked(void)
{
    return messaging.inflight != NULL || messaging.token != NULL || messaging.reset_write ||
        messaging.view.state == ROOM_MESSAGING_REQUESTING ||
        messaging.view.state == ROOM_MESSAGING_WAITING ||
        messaging.view.state == ROOM_MESSAGING_CONNECTING;
}

void room_messaging_invalidate_locked(void)
{
    ++messaging.intent;
    state(ROOM_MESSAGING_REQUIRED);
    messaging.view.request_id[0] = '\0';
    messaging.disconnect_queued = messaging.connect_queued = false;
#if !CONFIG_OPENCLAW_ROOM_VOICE_ACTIONS
    messaging.write_approved = false;
    messaging.reset_write = messaging.write_registered;
#endif
    clear_token();
    free(messaging.uri);
    messaging.uri = NULL;
}

void room_messaging_set_operator_locked(esp_openclaw_node_handle_t client,
    uint64_t incarnation, uint32_t connection, bool ready)
{
    messaging.client = client;
    messaging.incarnation = incarnation;
    messaging.connection = connection;
    messaging.connected = ready;
    if (ready && messaging.connect_queued) {
        messaging.connect_queued = false;
        messaging.write_approved = true;
        clear_token();
        free(messaging.uri);
        messaging.uri = NULL;
        state(ROOM_MESSAGING_READY);
    } else if (ready && !room_messaging_busy_locked()) {
        state(messaging.write_approved ? ROOM_MESSAGING_READY : ROOM_MESSAGING_REQUIRED);
    } else if (!ready && messaging.connect_queued) {
        messaging.connect_queued = false;
        state(ROOM_MESSAGING_ERROR);
    }
}

void room_messaging_copy_locked(room_messaging_snapshot_t *snapshot, bool idle)
{
    *snapshot = messaging.view;
    snapshot->connected = messaging.connected;
    snapshot->busy = room_messaging_busy_locked();
    snapshot->can_request = idle && !messaging.inflight &&
        ((!room_messaging_busy_locked() && messaging.connected && !messaging.write_approved) ||
         (messaging.token != NULL && messaging.view.state == ROOM_MESSAGING_ERROR));
}

esp_err_t room_messaging_request_locked(unsigned revision, bool idle)
{
    room_messaging_snapshot_t view;
    room_messaging_copy_locked(&view, idle);
    if (!view.can_request || revision != view.revision) return ESP_ERR_INVALID_STATE;
    if (messaging.token != NULL) {
        state(ROOM_MESSAGING_CONNECTING);
    } else {
        free(messaging.uri);
        messaging.uri = esp_openclaw_node_dup_gateway_uri(messaging.client);
        if (messaging.uri == NULL) return ESP_ERR_NO_MEM;
        ++messaging.intent;
        messaging.disconnect_queued = false;
        messaging.view.request_id[0] = '\0';
        state(ROOM_MESSAGING_REQUESTING);
        messaging.deadline = esp_timer_get_time() + 15000000;
    }
    messaging.wake();
    return ESP_OK;
}

static void response(esp_openclaw_node_handle_t client,
    const esp_openclaw_node_gateway_result_t *result, void *context)
{
    (void)client;
    approval_t *request = context;
    if (result != NULL && result->ok && result->payload_json != NULL) {
        size_t length = strnlen(result->payload_json, 4097);
        if (length <= 4096) request->json = strdup(result->payload_json);
    }
    atomic_store(&completed, request);
    messaging.wake();
}

static cJSON *parse(const char *json)
{
    if (json == NULL) return NULL;
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; json[i] != '\0'; ++i) {
        char c = json[i];
        if (quoted) {
            if (escaped) {
                if (c == 'u' && strncmp(json + i + 1, "0000", 4) == 0) return NULL;
                escaped = false;
            } else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 4) return NULL; }
        else if (c == '}' || c == ']') { if (depth == 0) return NULL; --depth; }
    }
    if (depth != 0 || quoted) return NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, strlen(json) + 1, NULL, true);
    if (cJSON_IsObject(root)) return root;
    cJSON_Delete(root);
    return NULL;
}

static bool has_scopes(const cJSON *root)
{
    const cJSON *scopes = cJSON_GetObjectItemCaseSensitive(root, "scopes"), *scope;
    if (!cJSON_IsArray(scopes)) return false;
    bool read = false, talk = false, write = false;
    cJSON_ArrayForEach(scope, scopes) {
        if (!cJSON_IsString(scope)) return false;
        read |= strcmp(scope->valuestring, "operator.read") == 0;
        talk |= strcmp(scope->valuestring, "operator.talk") == 0;
        write |= strcmp(scope->valuestring, "operator.write") == 0;
    }
    return read && talk && write;
}

static void consume(void)
{
    approval_t *request = atomic_exchange(&completed, NULL);
    if (request == NULL) return;
    cJSON *root = parse(request->json);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "requestId");
    xSemaphoreTake(messaging.lock, portMAX_DELAY);
    messaging.inflight = NULL;
    /* DISCONNECTED may follow a valid approval before the supervisor runs.
     * The captured incarnation still owns that receipt; a new CONNECTED or
     * explicit re-pair revokes it. */
    bool current = request->intent == messaging.intent && request->client == messaging.client &&
        request->incarnation == messaging.incarnation && request->connection != 0;
    if (current) {
        bool valid_id = cJSON_IsString(id) && id->valuestring[0] != '\0' &&
            strlen(id->valuestring) < sizeof(messaging.view.request_id);
        if (!request->waiting) {
            if (valid_id && messaging.connected && request->connection == messaging.connection) {
                strcpy(messaging.view.request_id, id->valuestring);
                state(ROOM_MESSAGING_WAITING);
                messaging.deadline = esp_timer_get_time() + 315000000;
            } else if (messaging.view.state != ROOM_MESSAGING_EXPIRED) state(ROOM_MESSAGING_ERROR);
        } else {
            const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
            const cJSON *token = cJSON_GetObjectItemCaseSensitive(root, "deviceToken");
            bool matches = valid_id && strcmp(id->valuestring, messaging.view.request_id) == 0 &&
                cJSON_IsString(status);
            if (matches && strcmp(status->valuestring, "rejected") == 0) state(ROOM_MESSAGING_REJECTED);
            else if (matches && strcmp(status->valuestring, "expired") == 0) state(ROOM_MESSAGING_EXPIRED);
            else if (matches && strcmp(status->valuestring, "approved") == 0 &&
                cJSON_IsString(token) && token->valuestring[0] != '\0' &&
                strlen(token->valuestring) <= 2048 && has_scopes(root) &&
                (messaging.token = strdup(token->valuestring)) != NULL) {
                state(ROOM_MESSAGING_CONNECTING);
                messaging.disconnect_queued = messaging.connect_queued = false;
            } else if (messaging.view.state != ROOM_MESSAGING_EXPIRED) state(ROOM_MESSAGING_ERROR);
        }
    } else if (messaging.view.state == ROOM_MESSAGING_REQUESTING ||
               messaging.view.state == ROOM_MESSAGING_WAITING) state(ROOM_MESSAGING_ERROR);
    xSemaphoreGive(messaging.lock);
    cJSON_Delete(root);
    if (request->json != NULL) memset(request->json, 0, strlen(request->json));
    free(request->json);
    free(request);
}

void room_messaging_poll(void)
{
    if (messaging.lock == NULL) return;
    consume();
    xSemaphoreTake(messaging.lock, portMAX_DELAY);
    if (messaging.reset_write && !messaging.connected && messaging.inflight == NULL &&
        esp_openclaw_node_unregister_scope(messaging.client, "operator.write") == ESP_OK) {
        messaging.write_registered = messaging.reset_write = false;
    }
    room_messaging_state_t phase = messaging.view.state;
    if ((phase == ROOM_MESSAGING_REQUESTING || phase == ROOM_MESSAGING_WAITING) &&
        esp_timer_get_time() >= messaging.deadline) {
        /* Keep the in-flight owner: a late rotated credential is still useful.
         * A timeout never resubmits approval or frees callback storage. */
        state(phase == ROOM_MESSAGING_WAITING ? ROOM_MESSAGING_EXPIRED : ROOM_MESSAGING_ERROR);
        phase = messaging.view.state;
    }
    if (messaging.inflight != NULL && messaging.connected && !messaging.disconnect_queued &&
        (phase == ROOM_MESSAGING_EXPIRED || phase == ROOM_MESSAGING_ERROR)) {
        /* The SDK has no per-RPC cancellation. Closing this operator flushes
         * the bounded waiter; the independent node remains connected. */
        if (esp_openclaw_node_request_disconnect(messaging.client) == ESP_OK)
            messaging.disconnect_queued = true;
    }
    if (messaging.inflight == NULL && messaging.connected &&
        (phase == ROOM_MESSAGING_REQUESTING || phase == ROOM_MESSAGING_WAITING)) {
        approval_t *request = calloc(1, sizeof(*request));
        if (request == NULL) state(ROOM_MESSAGING_ERROR);
        else {
            request->intent = messaging.intent;
            request->client = messaging.client;
            request->incarnation = messaging.incarnation;
            request->connection = messaging.connection;
            request->waiting = phase == ROOM_MESSAGING_WAITING;
            cJSON *params = cJSON_CreateObject();
            bool valid;
            if (request->waiting) valid = cJSON_AddStringToObject(params, "requestId", messaging.view.request_id) != NULL;
            else {
                const char *scopes[] = {"operator.read", "operator.talk", "operator.write"};
                cJSON *array = cJSON_CreateStringArray(scopes, 3);
                valid = array != NULL && cJSON_AddItemToObject(params, "scopes", array);
                if (!valid) cJSON_Delete(array);
            }
            char *json = valid ? cJSON_PrintUnformatted(params) : NULL;
            cJSON_Delete(params);
            messaging.inflight = request;
            esp_err_t err = json == NULL ? ESP_ERR_NO_MEM : esp_openclaw_node_gateway_request_on_connection(
                messaging.client, request->connection, request->waiting ?
                "device.scopes.waitUpgrade" : "device.scopes.requestUpgrade", json, response, request);
            free(json);
            if (err != ESP_OK) { atomic_store(&completed, request); messaging.wake(); }
        }
    } else if (phase == ROOM_MESSAGING_CONNECTING && !messaging.connect_queued) {
        if (messaging.connected) {
            if (!messaging.disconnect_queued) {
                esp_err_t err = esp_openclaw_node_request_disconnect(messaging.client);
                if (err == ESP_OK) messaging.disconnect_queued = true;
                else state(ROOM_MESSAGING_ERROR);
            }
        } else {
            esp_err_t err = ESP_OK;
            if (!messaging.write_registered) {
                err = esp_openclaw_node_register_scope(messaging.client, "operator.write");
                if (err == ESP_OK) messaging.write_registered = true;
            }
            if (err == ESP_OK) {
                const esp_openclaw_node_connect_request_t connect = {
                    .source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_DEVICE_TOKEN,
                    .gateway_uri = messaging.uri, .value = messaging.token,
                };
                err = esp_openclaw_node_request_connect(messaging.client, &connect);
            }
            if (err == ESP_OK) messaging.connect_queued = true;
            else state(ROOM_MESSAGING_ERROR);
        }
    } else if (!messaging.connected && messaging.inflight == NULL &&
        (phase == ROOM_MESSAGING_REQUESTING || phase == ROOM_MESSAGING_WAITING)) {
        state(ROOM_MESSAGING_ERROR);
    }
    xSemaphoreGive(messaging.lock);
}
