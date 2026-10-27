#include "dial_reader.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

typedef enum { IDLE, CONNECTING, CONNECTED, STOPPING } reader_phase_t;
typedef enum { OFFLINE, STARTING, READY, ACCESS_NEEDED, ERROR } reader_status_t;
typedef enum { NONE, UP, LOST, AUTH_REJECTED, FAILED, CLOSED } reader_event_t;

static atomic_uint node_generation;
static atomic_uint pending_event;

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
} reader;

static bool current_node(void)
{
    return reader.generation == atomic_load(&node_generation) &&
        reader.node_id != 0 && reader.uri != NULL &&
        reader.node_id == esp_openclaw_node_get_connection_id(reader.node);
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
        xSemaphoreTake(reader.lock, portMAX_DELAY);
        bool stale = reader.attempt != reader.generation || !current_node();
        bool stop = reader.phase != IDLE && reader.phase != STOPPING &&
            (stale || (reader.phase == CONNECTED && reader.status != READY));
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
        bool current = current_node();
        ready = current && atomic_load(&pending_event) == NONE &&
            reader.status == READY && reader.attempt == reader.generation &&
            reader.reader_id != 0 && reader.reader_id == esp_openclaw_node_get_connection_id(reader.client);
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
