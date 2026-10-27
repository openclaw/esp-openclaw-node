#include "dial_reader.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

typedef enum { IDLE, CONNECTING, CONNECTED, STOPPING } reader_phase_t;
typedef enum { OFFLINE, STARTING, READY, ACCESS_NEEDED, ERROR } reader_status_t;
typedef enum { NONE, UP, LOST, AUTH_REJECTED, FAILED, CLOSED } reader_event_t;

static atomic_uint node_generation;
static atomic_uint pending_event;

typedef struct {
    unsigned generation, revision;
    uint32_t connection;
    size_t offset;
    int64_t started;
    bool ok, too_large;
    char *json;
    esp_openclaw_agent_page_t page;
} agents_request_t;

static _Atomic(agents_request_t *) completed_request;

static struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    esp_openclaw_node_handle_t node, client;
    char *uri;
    uint32_t node_id, reader_id;
    unsigned generation, attempt;
    reader_phase_t phase;
    reader_status_t status;
    bool retry;
    bool directory_enabled, directory_pending, force_disconnect;
    size_t directory_offset;
    dial_agents_state_t directory_state;
    unsigned directory_revision;
    esp_openclaw_agent_page_t *directory_page;
    agents_request_t *inflight;
} reader;

static bool current_node(void)
{
    return reader.generation == atomic_load(&node_generation) &&
        reader.node_id != 0 && reader.uri != NULL &&
        reader.node_id == esp_openclaw_node_get_connection_id(reader.node);
}

static bool current_reader(void)
{
    return current_node() && reader.phase == CONNECTED && !reader.force_disconnect &&
        atomic_load(&pending_event) == NONE &&
        reader.status == READY && reader.attempt == reader.generation &&
        reader.reader_id != 0 && reader.reader_id == esp_openclaw_node_get_connection_id(reader.client);
}

static void invalidate_directory(void)
{
    ++reader.directory_revision;
    free(reader.directory_page);
    reader.directory_page = NULL;
    reader.directory_pending = reader.directory_enabled;
    if (reader.directory_enabled || reader.directory_state != DIAL_AGENTS_ERROR)
        reader.directory_state = reader.directory_enabled ? DIAL_AGENTS_LOADING : DIAL_AGENTS_IDLE;
}

static void reader_event(esp_openclaw_node_handle_t client, esp_openclaw_node_event_t event,
                         const void *data, void *context)
{
    (void)client;
    (void)context;
    reader_event_t next = FAILED;
    if (event == ESP_OPENCLAW_NODE_EVENT_CONNECTED) {
        next = UP;
    } else if (event == ESP_OPENCLAW_NODE_EVENT_CONNECT_FAILED) {
        const esp_openclaw_node_connect_failed_event_t *failure = data;
        if (failure != NULL) {
            if (failure->reason == ESP_OPENCLAW_NODE_CONNECT_FAILURE_AUTH_REJECTED) next = AUTH_REJECTED;
            else if (failure->reason == ESP_OPENCLAW_NODE_CONNECT_FAILURE_TRANSPORT_START_FAILED ||
                     failure->reason == ESP_OPENCLAW_NODE_CONNECT_FAILURE_CONNECTION_LOST) next = LOST;
        }
    } else {
        const esp_openclaw_node_disconnected_event_t *disconnected = data;
        next = disconnected != NULL && disconnected->reason == ESP_OPENCLAW_NODE_DISCONNECTED_REASON_CONNECTION_LOST ? LOST : CLOSED;
    }
    /* Only this worker starts/stops the client. If a connection closes before
     * the worker sees UP, the later terminal event supersedes it. Callbacks
     * never allocate, acquire another task's mutex, or wait on SDK state. */
    atomic_store(&pending_event, next);
    xTaskNotifyGive(reader.task);
}

static void refresh_node(void)
{
    unsigned generation = atomic_load(&node_generation);
    if (generation == reader.generation && (reader.node_id == 0 || reader.uri != NULL)) return;
    uint32_t id = esp_openclaw_node_get_connection_id(reader.node);
    char *uri = id != 0 ? esp_openclaw_node_dup_gateway_uri(reader.node) : NULL;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    free(reader.uri);
    reader.uri = uri;
    reader.node_id = id;
    if (reader.generation != generation) invalidate_directory();
    reader.generation = generation;
    reader.reader_id = 0;
    reader.retry = id != 0;
    reader.status = id == 0 ? OFFLINE : uri == NULL ? ERROR : STARTING;
    xSemaphoreGive(reader.lock);
}

static void consume_event(void)
{
    reader_event_t event = atomic_exchange(&pending_event, NONE);
    if (event == NONE) return;
    char *uri = event == UP ? esp_openclaw_node_dup_gateway_uri(reader.client) : NULL;
    uint32_t id = event == UP ? esp_openclaw_node_get_connection_id(reader.client) : 0;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    reader.reader_id = 0;
    if (event == UP) {
        reader.phase = CONNECTED;
        if (reader.attempt == reader.generation && current_node()) {
            /* A saved operator token can belong to the previous Gateway. A
             * failed URI allocation is retryable, not a credential failure. */
            bool same_gateway = uri != NULL && strcmp(uri, reader.uri) == 0;
            reader.reader_id = same_gateway ? id : 0;
            reader.status = uri == NULL || id == 0 ? ERROR : same_gateway ? READY : ACCESS_NEEDED;
            reader.retry = uri == NULL || id == 0;
        }
    } else {
        invalidate_directory();
        reader.force_disconnect = false;
        reader.phase = IDLE;
        if (reader.attempt != reader.generation) {
            reader.retry = current_node();
        } else if (event != CLOSED) {
            reader.retry = event == LOST;
            reader.status = event == LOST ? STARTING : event == AUTH_REJECTED ? ACCESS_NEEDED : ERROR;
        }
        if (!current_node()) reader.status = OFFLINE;
    }
    xSemaphoreGive(reader.lock);
    free(uri);
}

static void agents_response(esp_openclaw_node_handle_t client,
                            const esp_openclaw_node_gateway_result_t *result, void *context)
{
    (void)client;
    agents_request_t *request = context;
    request->ok = result->ok && result->payload_json != NULL;
    if (request->ok) {
        size_t length = strnlen(result->payload_json, ESP_OPENCLAW_DIRECTORY_MAX_JSON_BYTES + 1);
        request->too_large = length > ESP_OPENCLAW_DIRECTORY_MAX_JSON_BYTES;
        if (!request->too_large) {
            request->json = malloc(length + 1);
            if (request->json != NULL) memcpy(request->json, result->payload_json, length + 1);
        }
    }
    /* One outstanding request owns this mailbox. JSON is parsed on the worker,
     * after the SDK callback can release its own response allocation. */
    atomic_store(&completed_request, request);
    xTaskNotifyGive(reader.task);
}

static void consume_agents(void)
{
    agents_request_t *request = atomic_exchange(&completed_request, NULL);
    if (request == NULL) return;
    dial_agents_state_t state = request->too_large ? DIAL_AGENTS_TOO_LARGE : DIAL_AGENTS_ERROR;
    if (request->ok && request->json != NULL) {
        esp_openclaw_directory_result_t parsed = esp_openclaw_directory_parse(
            request->json, strlen(request->json), request->offset, &request->page);
        if (parsed == ESP_OPENCLAW_DIRECTORY_OK)
            state = request->page.count == 0 ? DIAL_AGENTS_EMPTY : DIAL_AGENTS_READY;
        else if (parsed == ESP_OPENCLAW_DIRECTORY_TOO_LARGE) state = DIAL_AGENTS_TOO_LARGE;
    }
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    reader.inflight = NULL;
    if (request->generation == reader.generation && request->revision == reader.directory_revision &&
        current_reader() && request->connection == reader.reader_id) {
        free(reader.directory_page);
        reader.directory_page = NULL;
        if (state == DIAL_AGENTS_READY || state == DIAL_AGENTS_EMPTY) {
            reader.directory_page = malloc(sizeof(*reader.directory_page));
            if (reader.directory_page != NULL) *reader.directory_page = request->page;
            else state = DIAL_AGENTS_ERROR;
        }
        reader.directory_state = state;
    }
    xSemaphoreGive(reader.lock);
    free(request->json);
    free(request);
}

static void request_agents(void)
{
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    if (!reader.directory_pending || reader.inflight != NULL || !current_reader()) {
        xSemaphoreGive(reader.lock);
        return;
    }
    agents_request_t *request = calloc(1, sizeof(*request));
    reader.directory_pending = false;
    if (request == NULL) {
        reader.directory_state = DIAL_AGENTS_ERROR;
        xSemaphoreGive(reader.lock);
        return;
    }
    request->generation = reader.generation;
    request->revision = reader.directory_revision;
    request->connection = reader.reader_id;
    request->offset = reader.directory_offset;
    request->started = esp_timer_get_time();
    reader.inflight = request;
    xSemaphoreGive(reader.lock);
    esp_err_t err = esp_openclaw_node_gateway_request_on_connection(
        reader.client, request->connection, "agents.list", "{}", agents_response, request);
    if (err != ESP_OK) {
        /* Immediate rejection owns no callback; use the same completion path. */
        atomic_store(&completed_request, request);
        xTaskNotifyGive(reader.task);
    }
}

static esp_err_t create_client(void)
{
    esp_openclaw_node_config_t config;
    esp_openclaw_node_config_init_default(&config);
    config.display_name = "OpenClaw M5Dial Reader";
    config.model_identifier = "m5stack-dial";
    config.role = "operator";
    config.event_cb = reader_event;
    esp_err_t err = esp_openclaw_node_create(&config, &reader.client);
    if (err == ESP_OK) {
        err = esp_openclaw_node_register_scope(reader.client, "operator.read");
        if (err != ESP_OK) {
            esp_openclaw_node_destroy(reader.client);
            reader.client = NULL;
        }
    }
    return err;
}

static void reader_task(void *context)
{
    (void)context;
    for (;;) {
        /* Also retry a rejected queue submission: it produces no SDK event.
         * This bounds reconnects without polling NVS or retrying auth failures. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        refresh_node();
        consume_event();
        consume_agents();
        xSemaphoreTake(reader.lock, portMAX_DELAY);
        if (reader.inflight != NULL && !reader.force_disconnect &&
            esp_timer_get_time() - reader.inflight->started >= 15000000) {
            /* SDK requests have no response deadline. Closing this read-only
             * transport completes its callbacks before another request starts. */
            ++reader.directory_revision;
            reader.directory_state = DIAL_AGENTS_ERROR;
            reader.directory_enabled = reader.directory_pending = false;
            reader.force_disconnect = reader.retry = true;
        }
        bool stale = reader.attempt != reader.generation || !current_node();
        bool stop = reader.phase != IDLE && reader.phase != STOPPING &&
            (stale || reader.force_disconnect || (reader.phase == CONNECTED && reader.status != READY));
        bool start = reader.phase == IDLE && reader.retry && current_node();
        unsigned attempt = reader.attempt;
        if (start) {
            reader.attempt = reader.generation;
            attempt = reader.attempt;
            reader.phase = CONNECTING;
            reader.status = STARTING;
            reader.retry = false;
        }
        xSemaphoreGive(reader.lock);

        if (stop) {
            esp_err_t err = esp_openclaw_node_request_disconnect(reader.client);
            xSemaphoreTake(reader.lock, portMAX_DELAY);
            if (err == ESP_OK && reader.attempt == attempt && reader.phase != IDLE) reader.phase = STOPPING;
            xSemaphoreGive(reader.lock);
        } else if (start) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            xSemaphoreTake(reader.lock, portMAX_DELAY);
            bool current = attempt == reader.generation && current_node();
            if (!current) {
                reader.phase = IDLE;
                reader.retry = current_node();
            }
            xSemaphoreGive(reader.lock);
            if (!current) continue;
            esp_err_t err = reader.client != NULL ? ESP_OK : create_client();
            if (err == ESP_OK) {
                const esp_openclaw_node_connect_request_t request = {
                    .source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_SAVED_SESSION,
                };
                err = esp_openclaw_node_request_connect(reader.client, &request);
            }
            if (err != ESP_OK) {
                xSemaphoreTake(reader.lock, portMAX_DELAY);
                reader.phase = IDLE;
                if (attempt == reader.generation) {
                    reader.status = err == ESP_ERR_NOT_FOUND ? ACCESS_NEEDED : ERROR;
                    reader.retry = err == ESP_ERR_NO_MEM || err == ESP_ERR_INVALID_STATE || err == ESP_FAIL;
                } else {
                    reader.retry = current_node();
                }
                xSemaphoreGive(reader.lock);
            }
        }
        request_agents();
    }
}

esp_err_t dial_reader_start(esp_openclaw_node_handle_t node)
{
    if (node == NULL) return ESP_ERR_INVALID_ARG;
    if (reader.lock != NULL) return ESP_ERR_INVALID_STATE;
    reader.lock = xSemaphoreCreateMutex();
    if (reader.lock == NULL) return ESP_ERR_NO_MEM;
    reader.node = node;
    if (xTaskCreate(reader_task, "dial_reader", 4096, NULL, 5, &reader.task) != pdPASS) {
        vSemaphoreDelete(reader.lock);
        reader.lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void dial_reader_node_event(esp_openclaw_node_event_t event)
{
    (void)event;
    if (reader.task == NULL) return;
    atomic_fetch_add(&node_generation, 1);
    xTaskNotifyGive(reader.task);
}

cJSON *dial_reader_status(void)
{
    const char *state = "unavailable";
    bool ready = false;
    if (reader.lock != NULL) {
        static const char *states[] = {"offline", "connecting", "ready", "access-needed", "error"};
        xSemaphoreTake(reader.lock, portMAX_DELAY);
        ready = current_reader();
        state = reader.generation != atomic_load(&node_generation) || reader.node_id == 0 ||
            reader.node_id != esp_openclaw_node_get_connection_id(reader.node) ? "offline" : reader.status == READY && !ready ? "connecting" : states[reader.status];
        xSemaphoreGive(reader.lock);
    }
    cJSON *status = cJSON_CreateObject();
    if (status == NULL) return NULL;
    if (cJSON_AddStringToObject(status, "state", state) == NULL ||
        cJSON_AddBoolToObject(status, "readConnected", ready) == NULL) {
        cJSON_Delete(status);
        return NULL;
    }
    return status;
}

esp_err_t dial_reader_agents_refresh(size_t offset)
{
    if (reader.lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    reader.directory_enabled = reader.directory_pending = true;
    reader.directory_offset = offset;
    ++reader.directory_revision;
    reader.directory_state = DIAL_AGENTS_LOADING;
    free(reader.directory_page);
    reader.directory_page = NULL;
    xSemaphoreGive(reader.lock);
    xTaskNotifyGive(reader.task);
    return ESP_OK;
}

void dial_reader_agents_copy(dial_agents_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = DIAL_AGENTS_ERROR;
    if (reader.lock == NULL) return;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    if (current_reader()) {
        snapshot->state = reader.directory_state;
        snapshot->revision = reader.directory_revision;
        if (reader.directory_page != NULL) snapshot->page = *reader.directory_page;
    } else {
        snapshot->state = !current_node() ? DIAL_AGENTS_OFFLINE :
            reader.status == ACCESS_NEEDED ? DIAL_AGENTS_ACCESS_NEEDED :
            reader.status == ERROR ? DIAL_AGENTS_ERROR : DIAL_AGENTS_CONNECTING;
    }
    xSemaphoreGive(reader.lock);
}
