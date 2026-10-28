#include "esp_openclaw_devices.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_timer.h"

#define DEVICES_TIMEOUT_US (15 * 1000 * 1000)

typedef struct {
    esp_openclaw_node_handle_t client;
    uint64_t incarnation;
    uint32_t connection;
    unsigned revision;
    int64_t deadline;
    esp_openclaw_devices_state_t state;
    char *json;
} device_request_t;

static struct {
    SemaphoreHandle_t lock;
    void (*wake)(void);
    esp_timer_handle_t timer;
    esp_openclaw_node_handle_t client;
    uint64_t incarnation;
    uint32_t connection;
    unsigned revision;
    int64_t deadline;
    bool ready, interested, pending;
    esp_openclaw_devices_state_t state;
    esp_openclaw_devices_snapshot_t *page;
    device_request_t *inflight;
} directory;
static _Atomic(device_request_t *) completed;

static void deadline_fired(void *arg)
{
    (void)arg;
    directory.wake();
}

esp_err_t esp_openclaw_devices_init(SemaphoreHandle_t lock, void (*wake)(void))
{
    directory.lock = lock;
    directory.wake = wake;
    const esp_timer_create_args_t args = {.callback = deadline_fired, .name = "device_directory"};
    return esp_timer_create(&args, &directory.timer);
}

static void invalidate(void)
{
    ++directory.revision;
    free(directory.page);
    directory.page = NULL;
    directory.pending = directory.interested;
    directory.state = !directory.interested ? ESP_OPENCLAW_DEVICES_IDLE :
        directory.ready ? ESP_OPENCLAW_DEVICES_LOADING : ESP_OPENCLAW_DEVICES_OFFLINE;
    esp_timer_stop(directory.timer);
    if (directory.state == ESP_OPENCLAW_DEVICES_LOADING) {
        /* The view owns its deadline even while an invalidated callback still
         * occupies the single transport slot. */
        directory.deadline = esp_timer_get_time() + DEVICES_TIMEOUT_US;
        if (esp_timer_start_once(directory.timer, DEVICES_TIMEOUT_US) != ESP_OK) {
            directory.state = ESP_OPENCLAW_DEVICES_ERROR;
            directory.pending = false;
        }
    }
}

void esp_openclaw_devices_set_owner_locked(esp_openclaw_node_handle_t client,
    uint64_t incarnation, uint32_t connection, bool ready)
{
    ready = ready && client != NULL && connection != 0;
    if (directory.client == client && directory.incarnation == incarnation &&
        directory.connection == connection && directory.ready == ready) return;
    directory.client = client;
    directory.incarnation = incarnation;
    directory.connection = connection;
    directory.ready = ready;
    invalidate();
}

esp_err_t esp_openclaw_devices_refresh(void)
{
    if (directory.lock == NULL || directory.timer == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(directory.lock, portMAX_DELAY);
    directory.interested = true;
    invalidate();
    xSemaphoreGive(directory.lock);
    directory.wake();
    return ESP_OK;
}

void esp_openclaw_devices_cancel(void)
{
    if (directory.lock == NULL) return;
    xSemaphoreTake(directory.lock, portMAX_DELAY);
    directory.interested = false;
    invalidate();
    xSemaphoreGive(directory.lock);
    directory.wake();
}

void esp_openclaw_devices_copy(esp_openclaw_devices_snapshot_t *snapshot)
{
    *snapshot = (esp_openclaw_devices_snapshot_t){0};
    if (directory.lock == NULL) return;
    xSemaphoreTake(directory.lock, portMAX_DELAY);
    if (directory.page != NULL) *snapshot = *directory.page;
    snapshot->state = directory.state;
    snapshot->revision = directory.revision;
    xSemaphoreGive(directory.lock);
}

static bool current(const device_request_t *request)
{
    return directory.ready && directory.interested && request->revision == directory.revision &&
        request->client == directory.client && request->incarnation == directory.incarnation &&
        request->connection == directory.connection;
}

static void copy_label(char *out, size_t capacity, const char *value)
{
    size_t length = strlen(value);
    if (length >= capacity) {
        length = capacity - 1;
        while (length != 0 && ((unsigned char)value[length] & 0xc0) == 0x80) --length;
    }
    memcpy(out, value, length);
    out[length] = '\0';
}

static esp_openclaw_devices_state_t parse(const char *json, esp_openclaw_devices_snapshot_t *page)
{
    cJSON *root = cJSON_ParseWithLengthOpts(json, strlen(json) + 1, NULL, true);
    cJSON *nodes = cJSON_GetObjectItemCaseSensitive(root, "nodes");
    esp_openclaw_devices_state_t state = ESP_OPENCLAW_DEVICES_ERROR;
    if (!cJSON_IsObject(root) || !cJSON_IsArray(nodes)) goto done;
    if (cJSON_GetArraySize(nodes) > ESP_OPENCLAW_DEVICES_MAX) { state = ESP_OPENCLAW_DEVICES_TOO_LARGE; goto done; }
    const cJSON *node;
    cJSON_ArrayForEach(node, nodes) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(node, "nodeId");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(node, "displayName");
        const cJSON *model = cJSON_GetObjectItemCaseSensitive(node, "modelIdentifier");
        const cJSON *connected = cJSON_GetObjectItemCaseSensitive(node, "connected");
        const cJSON *paired = cJSON_GetObjectItemCaseSensitive(node, "paired");
        if (!cJSON_IsObject(node) || !cJSON_IsString(id) || id->valuestring[0] == '\0' ||
            strlen(id->valuestring) >= sizeof(page->devices[0].id) ||
            (name != NULL && !cJSON_IsString(name)) || (model != NULL && !cJSON_IsString(model)) ||
            (connected != NULL && !cJSON_IsBool(connected)) || (paired != NULL && !cJSON_IsBool(paired))) goto done;
        for (size_t i = 0; i < page->count; ++i)
            if (strcmp(page->devices[i].id, id->valuestring) == 0) goto done;
        esp_openclaw_device_t *device = &page->devices[page->count++];
        strcpy(device->id, id->valuestring);
        copy_label(device->name, sizeof(device->name), name != NULL && name->valuestring[0] != '\0'
            ? name->valuestring : id->valuestring);
        if (model != NULL) copy_label(device->model, sizeof(device->model), model->valuestring);
        device->connected_known = connected != NULL;
        device->connected = cJSON_IsTrue(connected);
        device->paired_known = paired != NULL;
        device->paired = cJSON_IsTrue(paired);
    }
    state = page->count == 0 ? ESP_OPENCLAW_DEVICES_EMPTY : ESP_OPENCLAW_DEVICES_READY;
done:
    cJSON_Delete(root);
    return state;
}

static void response(esp_openclaw_node_handle_t client,
    const esp_openclaw_node_gateway_result_t *result, void *context)
{
    (void)client;
    device_request_t *request = context;
    if (result->ok && result->payload_json != NULL) {
        size_t length = strnlen(result->payload_json, ESP_OPENCLAW_DEVICES_MAX_JSON_BYTES + 1);
        if (length > ESP_OPENCLAW_DEVICES_MAX_JSON_BYTES) request->state = ESP_OPENCLAW_DEVICES_TOO_LARGE;
        else {
            request->json = malloc(length + 1);
            if (request->json != NULL) memcpy(request->json, result->payload_json, length + 1);
        }
    }
    atomic_store(&completed, request);
    directory.wake();
}

void esp_openclaw_devices_poll(bool defer_request)
{
    if (directory.lock == NULL || directory.timer == NULL) return;
    device_request_t *done = atomic_exchange(&completed, NULL);
    if (done != NULL) {
        esp_openclaw_devices_snapshot_t *page = done->json != NULL ? calloc(1, sizeof(*page)) : NULL;
        if (page != NULL) done->state = parse(done->json, page);
        xSemaphoreTake(directory.lock, portMAX_DELAY);
        directory.inflight = NULL;
        if (current(done)) {
            esp_timer_stop(directory.timer);
            directory.state = esp_timer_get_time() >= done->deadline ? ESP_OPENCLAW_DEVICES_TIMEOUT : done->state;
            if (directory.state == ESP_OPENCLAW_DEVICES_READY || directory.state == ESP_OPENCLAW_DEVICES_EMPTY) {
                directory.page = page;
                page = NULL;
            }
        }
        xSemaphoreGive(directory.lock);
        free(page);
        free(done->json);
        free(done);
    }
    xSemaphoreTake(directory.lock, portMAX_DELAY);
    if (directory.state == ESP_OPENCLAW_DEVICES_LOADING && directory.ready && directory.interested &&
        esp_timer_get_time() >= directory.deadline) {
        directory.state = ESP_OPENCLAW_DEVICES_TIMEOUT;
        directory.pending = false;
        ++directory.revision;
        /* Keep the callback context until completion; the socket also owns Talk. */
    }
    if (defer_request || !directory.pending || !directory.ready || directory.inflight != NULL) {
        xSemaphoreGive(directory.lock);
        return;
    }
    directory.pending = false;
    device_request_t *request = calloc(1, sizeof(*request));
    if (request == NULL) {
        directory.state = ESP_OPENCLAW_DEVICES_ERROR;
        xSemaphoreGive(directory.lock);
        return;
    }
    *request = (device_request_t){.client = directory.client, .incarnation = directory.incarnation,
        .connection = directory.connection, .revision = directory.revision,
        .deadline = directory.deadline, .state = ESP_OPENCLAW_DEVICES_ERROR};
    directory.inflight = request;
    xSemaphoreGive(directory.lock);
    esp_err_t err = esp_openclaw_node_gateway_request_on_connection(request->client,
        request->connection, "node.list", "{}", response, request);
    if (err != ESP_OK) {
        atomic_store(&completed, request);
        directory.wake();
    }
}
