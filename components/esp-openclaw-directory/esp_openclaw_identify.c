#include "esp_openclaw_identify.h"
#include "esp_openclaw_device_control.h"
#include "esp_openclaw_devices_internal.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_random.h"
#include "esp_timer.h"

#define IDENTIFY_TIMEOUT_US 15000000
#define IDENTIFY_RECEIPT_BYTES 4096

typedef struct {
    esp_openclaw_node_handle_t client;
    uint64_t incarnation;
    uint32_t connection;
    unsigned revision;
    bool ok;
    char *json;
} identify_request_t;

static struct {
    SemaphoreHandle_t lock;
    void (*wake)(void);
    bool (*current)(bool write);
    esp_timer_handle_t timer;
    esp_openclaw_node_handle_t client;
    uint64_t incarnation;
    uint32_t connection;
    bool ready, writable, pending;
    unsigned inventory_revision;
    int64_t deadline;
    char key[42];
    esp_openclaw_identify_snapshot_t snapshot;
    identify_request_t *inflight;
} identify;
static _Atomic(identify_request_t *) completed;

static void deadline_fired(void *arg)
{
    (void)arg;
    identify.wake();
}

esp_err_t esp_openclaw_identify_init(SemaphoreHandle_t lock, void (*wake)(void),
    bool (*current)(bool write))
{
    if (lock == NULL || wake == NULL || current == NULL || identify.lock != NULL) return ESP_ERR_INVALID_STATE;
    identify.lock = lock;
    identify.wake = wake;
    identify.current = current;
    const esp_timer_create_args_t args = {.callback = deadline_fired, .name = "identify"};
    return esp_timer_create(&args, &identify.timer);
}

static void state(esp_openclaw_identify_state_t next)
{
    identify.snapshot.state = next;
    identify.snapshot.can_send = false;
    ++identify.snapshot.revision;
}

static bool target_current(void)
{
    esp_openclaw_device_t target;
    return identify.ready && identify.writable && identify.current(true) &&
        esp_openclaw_devices_identify_target_locked(identify.snapshot.target.id,
            identify.inventory_revision, &target) &&
        strcmp(target.model, identify.snapshot.target.model) == 0;
}

void esp_openclaw_identify_set_owner_locked(esp_openclaw_node_handle_t client,
    uint64_t incarnation, uint32_t connection, bool ready, bool writable)
{
    ready = ready && client != NULL && connection != 0;
    writable = ready && writable;
    if (identify.client == client && identify.incarnation == incarnation &&
        identify.connection == connection && identify.ready == ready && identify.writable == writable) return;
    identify.client = client;
    identify.incarnation = incarnation;
    identify.connection = connection;
    identify.ready = ready;
    identify.writable = writable;
    if (identify.snapshot.state == ESP_OPENCLAW_IDENTIFY_REVIEW ||
        identify.snapshot.state == ESP_OPENCLAW_IDENTIFY_SENDING) {
        state(identify.inflight != NULL ? ESP_OPENCLAW_IDENTIFY_UNKNOWN : ESP_OPENCLAW_IDENTIFY_FAILED);
        identify.pending = false;
        esp_timer_stop(identify.timer);
    }
}

esp_err_t esp_openclaw_identify_begin(const char *node_id, unsigned inventory_revision)
{
    if (identify.timer == NULL || node_id == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(identify.lock, portMAX_DELAY);
    esp_openclaw_device_t target;
    if (!identify.ready || !identify.current(false) || identify.inflight != NULL || identify.pending ||
        !esp_openclaw_devices_identify_target_locked(node_id, inventory_revision, &target)) {
        xSemaphoreGive(identify.lock);
        return ESP_ERR_INVALID_STATE;
    }
    identify.snapshot.target = target;
    identify.inventory_revision = inventory_revision;
    unsigned char random[16];
    esp_fill_random(random, sizeof(random));
    char nonce[33];
    for (size_t i = 0; i < sizeof(random); ++i) snprintf(nonce + i * 2, 3, "%02x", random[i]);
    snprintf(identify.key, sizeof(identify.key), "identify:%s", nonce);
    snprintf(identify.snapshot.text, sizeof(identify.snapshot.text), "Code: %.16s", nonce);
    state(ESP_OPENCLAW_IDENTIFY_REVIEW);
    identify.snapshot.can_send = identify.writable;
    xSemaphoreGive(identify.lock);
    identify.wake();
    return ESP_OK;
}

esp_err_t esp_openclaw_identify_send(unsigned revision)
{
    if (identify.timer == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(identify.lock, portMAX_DELAY);
    if (identify.snapshot.state != ESP_OPENCLAW_IDENTIFY_REVIEW || identify.snapshot.revision != revision ||
        identify.inflight != NULL || identify.pending || !target_current()) {
        xSemaphoreGive(identify.lock);
        return ESP_ERR_INVALID_STATE;
    }
    state(ESP_OPENCLAW_IDENTIFY_SENDING);
    identify.deadline = esp_timer_get_time() + IDENTIFY_TIMEOUT_US;
    identify.pending = esp_timer_start_once(identify.timer, IDENTIFY_TIMEOUT_US) == ESP_OK;
    if (!identify.pending) state(ESP_OPENCLAW_IDENTIFY_FAILED);
    bool pending = identify.pending;
    xSemaphoreGive(identify.lock);
    identify.wake();
    return pending ? ESP_OK : ESP_FAIL;
}

void esp_openclaw_identify_copy(esp_openclaw_identify_snapshot_t *snapshot)
{
    *snapshot = (esp_openclaw_identify_snapshot_t){0};
    if (identify.timer == NULL) return;
    xSemaphoreTake(identify.lock, portMAX_DELAY);
    *snapshot = identify.snapshot;
    snapshot->can_send = snapshot->state == ESP_OPENCLAW_IDENTIFY_REVIEW && target_current();
    xSemaphoreGive(identify.lock);
}

static void response(esp_openclaw_node_handle_t client,
    const esp_openclaw_node_gateway_result_t *result, void *context)
{
    (void)client;
    identify_request_t *request = context;
    request->ok = result->ok;
    if (result->ok && result->payload_json != NULL) {
        size_t length = strnlen(result->payload_json, IDENTIFY_RECEIPT_BYTES + 1);
        if (length <= IDENTIFY_RECEIPT_BYTES) {
            request->json = malloc(length + 1);
            if (request->json != NULL) memcpy(request->json, result->payload_json, length + 1);
        }
    }
    atomic_store(&completed, request);
    identify.wake();
}

static char *parameters(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return NULL;
    cJSON *params = cJSON_AddObjectToObject(root, "params");
    char *json = NULL;
    if (params != NULL && cJSON_AddStringToObject(root, "nodeId", identify.snapshot.target.id) &&
        cJSON_AddStringToObject(root, "command", "display.show") &&
        cJSON_AddNumberToObject(root, "timeoutMs", 10000) &&
        cJSON_AddStringToObject(root, "idempotencyKey", identify.key) &&
        cJSON_AddStringToObject(params, "heading", "Identify") &&
        cJSON_AddStringToObject(params, "text", identify.snapshot.text)) json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

void esp_openclaw_identify_poll(bool defer_request)
{
    if (identify.timer == NULL) return;
    identify_request_t *done = atomic_exchange(&completed, NULL);
    xSemaphoreTake(identify.lock, portMAX_DELAY);
    if (done != NULL) {
        identify.inflight = NULL;
        if (identify.snapshot.state == ESP_OPENCLAW_IDENTIFY_SENDING &&
            identify.snapshot.revision == done->revision && identify.client == done->client &&
            identify.incarnation == done->incarnation && identify.connection == done->connection &&
            identify.ready && identify.current(true) && esp_timer_get_time() < identify.deadline) {
            esp_openclaw_display_receipt_t receipt;
            bool valid = done->ok && done->json != NULL && esp_openclaw_display_receipt_parse(done->json,
                strlen(done->json), identify.snapshot.target.id, "display.show", "Identify", identify.snapshot.text, &receipt);
            state(!valid ? ESP_OPENCLAW_IDENTIFY_UNKNOWN : receipt.rendered ?
                ESP_OPENCLAW_IDENTIFY_RENDERED : ESP_OPENCLAW_IDENTIFY_ACCEPTED);
            esp_timer_stop(identify.timer);
        } else if (identify.snapshot.state == ESP_OPENCLAW_IDENTIFY_SENDING &&
                   identify.snapshot.revision == done->revision) {
            /* Completion still proves submission even if its authority or
             * deadline expired before this worker consumed the callback. */
            state(ESP_OPENCLAW_IDENTIFY_UNKNOWN);
            esp_timer_stop(identify.timer);
        }
        free(done->json);
        free(done);
    }
    if (identify.snapshot.state == ESP_OPENCLAW_IDENTIFY_SENDING &&
        (esp_timer_get_time() >= identify.deadline || !target_current())) {
        state(identify.inflight != NULL ? ESP_OPENCLAW_IDENTIFY_UNKNOWN : ESP_OPENCLAW_IDENTIFY_FAILED);
        identify.pending = false;
        esp_timer_stop(identify.timer);
    }
    if (defer_request || !identify.pending || identify.inflight != NULL) {
        xSemaphoreGive(identify.lock);
        return;
    }
    identify.pending = false;
    char *json = parameters();
    identify_request_t *request = json != NULL ? calloc(1, sizeof(*request)) : NULL;
    if (request == NULL) {
        cJSON_free(json);
        state(ESP_OPENCLAW_IDENTIFY_FAILED);
        esp_timer_stop(identify.timer);
        xSemaphoreGive(identify.lock);
        return;
    }
    *request = (identify_request_t){.client = identify.client, .incarnation = identify.incarnation,
        .connection = identify.connection, .revision = identify.snapshot.revision};
    identify.inflight = request;
    xSemaphoreGive(identify.lock);
    esp_err_t error = esp_openclaw_node_gateway_request_on_connection(request->client,
        request->connection, "node.invoke", json, response, request);
    cJSON_free(json);
    if (error != ESP_OK) {
        /* A rejected local queue never submitted the command. */
        xSemaphoreTake(identify.lock, portMAX_DELAY);
        if (identify.snapshot.state == ESP_OPENCLAW_IDENTIFY_SENDING && identify.snapshot.revision == request->revision)
            state(ESP_OPENCLAW_IDENTIFY_FAILED);
        xSemaphoreGive(identify.lock);
        atomic_store(&completed, request);
        identify.wake();
    }
}
