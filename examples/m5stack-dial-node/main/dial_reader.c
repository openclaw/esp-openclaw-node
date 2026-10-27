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

typedef enum { AGENTS, SESSIONS, PREVIEW } request_kind_t;

typedef struct {
    request_kind_t kind;
    char agent_id[65], key[ESP_OPENCLAW_SESSION_KEY_BYTES];
    unsigned generation, revision;
    uint32_t connection;
    size_t offset;
    int64_t started;
    bool ok, too_large;
    char *json;
    union {
        esp_openclaw_agent_page_t agents;
        esp_openclaw_session_page_t sessions;
        esp_openclaw_session_preview_t preview;
    } page;
} reader_request_t;

static _Atomic(reader_request_t *) completed_request;

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
    reader_request_t *inflight;
    dial_conversations_snapshot_t *conversations;
    unsigned conversations_revision, preview_revision;
    bool sessions_pending, preview_pending;
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

static void clear_conversations(void)
{
    ++reader.conversations_revision;
    ++reader.preview_revision;
    reader.sessions_pending = reader.preview_pending = false;
    free(reader.conversations);
    reader.conversations = NULL;
}

static void invalidate_directory(void)
{
    ++reader.directory_revision;
    clear_conversations();
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

static void reader_response(esp_openclaw_node_handle_t client,
                            const esp_openclaw_node_gateway_result_t *result, void *context)
{
    (void)client;
    reader_request_t *request = context;
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

static void consume_response(void)
{
    reader_request_t *request = atomic_exchange(&completed_request, NULL);
    if (request == NULL) return;
    dial_agents_state_t state = request->too_large ? DIAL_AGENTS_TOO_LARGE : DIAL_AGENTS_ERROR;
    if (request->ok && request->json != NULL) {
        esp_openclaw_directory_result_t parsed;
        size_t length = strlen(request->json);
        if (request->kind == AGENTS) {
            parsed = esp_openclaw_directory_parse(request->json, length, request->offset, &request->page.agents);
            if (parsed == ESP_OPENCLAW_DIRECTORY_OK)
                state = request->page.agents.count == 0 ? DIAL_AGENTS_EMPTY : DIAL_AGENTS_READY;
        } else if (request->kind == SESSIONS) {
            parsed = esp_openclaw_sessions_parse(request->json, length, request->agent_id,
                                                request->offset, &request->page.sessions);
            if (parsed == ESP_OPENCLAW_DIRECTORY_OK)
                state = request->page.sessions.count == 0 ? DIAL_AGENTS_EMPTY : DIAL_AGENTS_READY;
        } else {
            parsed = esp_openclaw_preview_parse(request->json, length, request->key, &request->page.preview);
            if (parsed == ESP_OPENCLAW_DIRECTORY_OK) state = DIAL_AGENTS_READY;
        }
        if (parsed == ESP_OPENCLAW_DIRECTORY_TOO_LARGE) state = DIAL_AGENTS_TOO_LARGE;
    }
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    reader.inflight = NULL;
    if (request->generation == reader.generation && current_reader() &&
        request->connection == reader.reader_id) {
        dial_conversations_snapshot_t *view = reader.conversations;
        if (request->kind == AGENTS && request->revision == reader.directory_revision) {
            free(reader.directory_page);
            reader.directory_page = NULL;
            if (state == DIAL_AGENTS_READY || state == DIAL_AGENTS_EMPTY) {
                reader.directory_page = malloc(sizeof(*reader.directory_page));
                if (reader.directory_page != NULL) *reader.directory_page = request->page.agents;
                else state = DIAL_AGENTS_ERROR;
            }
            reader.directory_state = state;
        } else if (request->kind == SESSIONS && view != NULL &&
                   request->revision == reader.conversations_revision) {
            if (state == DIAL_AGENTS_READY || state == DIAL_AGENTS_EMPTY) view->page = request->page.sessions;
            view->state = state;
        } else if (request->kind == PREVIEW && view != NULL &&
                   request->revision == reader.preview_revision) {
            if (state == DIAL_AGENTS_READY) view->preview = request->page.preview;
            view->preview_state = state;
        }
    }
    xSemaphoreGive(reader.lock);
    free(request->json);
    free(request);
}

static char *request_params(const reader_request_t *request)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return NULL;
    bool ok = true;
    if (request->kind == SESSIONS) {
        ok = cJSON_AddStringToObject(root, "agentId", request->agent_id) != NULL &&
            cJSON_AddNumberToObject(root, "limit", ESP_OPENCLAW_DIRECTORY_PAGE_SIZE) != NULL &&
            cJSON_AddNumberToObject(root, "offset", request->offset) != NULL &&
            cJSON_AddBoolToObject(root, "includeGlobal", false) != NULL &&
            cJSON_AddBoolToObject(root, "includeDerivedTitles", true) != NULL &&
            cJSON_AddBoolToObject(root, "includeLastMessage", true) != NULL;
    } else if (request->kind == PREVIEW) {
        cJSON *keys = cJSON_AddArrayToObject(root, "keys");
        cJSON *key = cJSON_CreateString(request->key);
        if (keys == NULL || key == NULL || !cJSON_AddItemToArray(keys, key)) {
            cJSON_Delete(key);
            ok = false;
        }
        ok = ok && cJSON_AddNumberToObject(root, "limit", ESP_OPENCLAW_PREVIEW_ITEMS) != NULL &&
            cJSON_AddNumberToObject(root, "maxChars", 160) != NULL;
    }
    char *json = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    return json;
}

static void request_next(void)
{
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    bool pending = reader.directory_pending || reader.sessions_pending || reader.preview_pending;
    if (!pending || reader.inflight != NULL || !current_reader()) {
        xSemaphoreGive(reader.lock);
        return;
    }
    request_kind_t kind = reader.preview_pending ? PREVIEW : reader.sessions_pending ? SESSIONS : AGENTS;
    reader_request_t *request = calloc(1, sizeof(*request));
    if (request == NULL) {
        if (kind == AGENTS) {
            reader.directory_pending = false;
            reader.directory_state = DIAL_AGENTS_ERROR;
        } else if (kind == SESSIONS) {
            reader.sessions_pending = false;
            reader.conversations->state = DIAL_AGENTS_ERROR;
        } else {
            reader.preview_pending = false;
            reader.conversations->preview_state = DIAL_AGENTS_ERROR;
        }
        xSemaphoreGive(reader.lock);
        return;
    }
    request->kind = kind;
    request->generation = reader.generation;
    request->connection = reader.reader_id;
    request->started = esp_timer_get_time();
    if (kind == AGENTS) {
        reader.directory_pending = false;
        request->revision = reader.directory_revision;
        request->offset = reader.directory_offset;
    } else if (kind == SESSIONS) {
        reader.sessions_pending = false;
        request->revision = reader.conversations_revision;
        request->offset = reader.conversations->page.offset;
        memcpy(request->agent_id, reader.conversations->agent.id, sizeof(request->agent_id));
    } else {
        reader.preview_pending = false;
        request->revision = reader.preview_revision;
        memcpy(request->key, reader.conversations->page.sessions[reader.conversations->selected].key,
               sizeof(request->key));
    }
    reader.inflight = request;
    xSemaphoreGive(reader.lock);
    char *params = request_params(request);
    const char *method = kind == AGENTS ? "agents.list" : kind == SESSIONS ? "sessions.list" : "sessions.preview";
    esp_err_t err = params != NULL ? esp_openclaw_node_gateway_request_on_connection(
        reader.client, request->connection, method, params, reader_response, request) : ESP_ERR_NO_MEM;
    free(params);
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
        consume_response();
        xSemaphoreTake(reader.lock, portMAX_DELAY);
        if (reader.inflight != NULL && !reader.force_disconnect &&
            esp_timer_get_time() - reader.inflight->started >= 15000000) {
            /* SDK requests have no response deadline. Closing this read-only
             * transport completes its callbacks before another request starts. */
            ++reader.directory_revision;
            clear_conversations();
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
        request_next();
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
    clear_conversations();
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

static void refresh_conversations(size_t offset)
{
    dial_conversations_snapshot_t *view = reader.conversations;
    view->revision = ++reader.conversations_revision;
    view->preview_revision = ++reader.preview_revision;
    memset(&view->page, 0, sizeof(view->page));
    memset(&view->preview, 0, sizeof(view->preview));
    view->page.offset = offset;
    view->selected = 0;
    view->state = DIAL_AGENTS_LOADING;
    view->preview_state = DIAL_AGENTS_IDLE;
    reader.sessions_pending = true;
    reader.preview_pending = false;
}

esp_err_t dial_reader_conversations_open(const char *agent_id, unsigned revision)
{
    if (reader.lock == NULL || agent_id == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    const esp_openclaw_agent_t *agent = NULL;
    if (current_reader() && revision == reader.directory_revision && reader.directory_page != NULL &&
        reader.directory_state == DIAL_AGENTS_READY) {
        for (size_t i = 0; i < reader.directory_page->count; ++i) {
            if (strcmp(reader.directory_page->agents[i].id, agent_id) == 0) agent = &reader.directory_page->agents[i];
        }
    }
    if (agent == NULL) { xSemaphoreGive(reader.lock); return ESP_ERR_INVALID_STATE; }
    if (reader.conversations == NULL) reader.conversations = calloc(1, sizeof(*reader.conversations));
    if (reader.conversations == NULL) { xSemaphoreGive(reader.lock); return ESP_ERR_NO_MEM; }
    reader.conversations->agent = *agent;
    refresh_conversations(0);
    xSemaphoreGive(reader.lock);
    xTaskNotifyGive(reader.task);
    return ESP_OK;
}

esp_err_t dial_reader_conversations_refresh(size_t offset)
{
    if (reader.lock == NULL) return ESP_ERR_INVALID_STATE;
    if (offset > 2147483647u) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    if (!current_reader() || reader.conversations == NULL) {
        xSemaphoreGive(reader.lock);
        return ESP_ERR_INVALID_STATE;
    }
    refresh_conversations(offset);
    xSemaphoreGive(reader.lock);
    xTaskNotifyGive(reader.task);
    return ESP_OK;
}

esp_err_t dial_reader_preview_open(size_t index, unsigned revision)
{
    if (reader.lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    dial_conversations_snapshot_t *view = reader.conversations;
    if (!current_reader() || view == NULL || view->state != DIAL_AGENTS_READY ||
        revision != reader.conversations_revision || index >= view->page.count) {
        xSemaphoreGive(reader.lock);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&view->preview, 0, sizeof(view->preview));
    view->selected = index;
    view->preview_revision = ++reader.preview_revision;
    view->preview_state = DIAL_AGENTS_LOADING;
    reader.preview_pending = true;
    xSemaphoreGive(reader.lock);
    xTaskNotifyGive(reader.task);
    return ESP_OK;
}

void dial_reader_conversations_close(void)
{
    if (reader.lock == NULL) return;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    clear_conversations();
    xSemaphoreGive(reader.lock);
}

void dial_reader_conversations_copy(dial_conversations_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = snapshot->preview_state = DIAL_AGENTS_ERROR;
    if (reader.lock == NULL) return;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    if (current_reader()) {
        if (reader.conversations != NULL) *snapshot = *reader.conversations;
        else snapshot->state = snapshot->preview_state = DIAL_AGENTS_IDLE;
    } else {
        snapshot->state = snapshot->preview_state = !current_node() ? DIAL_AGENTS_OFFLINE :
            reader.status == ACCESS_NEEDED ? DIAL_AGENTS_ACCESS_NEEDED :
            reader.status == ERROR ? DIAL_AGENTS_ERROR : DIAL_AGENTS_CONNECTING;
    }
    xSemaphoreGive(reader.lock);
}

void dial_reader_home_copy(dial_home_context_t *context)
{
    memset(context, 0, sizeof(*context));
    if (reader.lock == NULL) return;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    const dial_conversations_snapshot_t *view = reader.conversations;
    if (current_reader() && reader.directory_state == DIAL_AGENTS_READY &&
        reader.directory_page != NULL && view != NULL) {
        for (size_t i = 0; i < reader.directory_page->count; ++i) {
            if (strcmp(reader.directory_page->agents[i].id, view->agent.id) != 0) continue;
            context->available = true;
            context->directory_revision = reader.directory_revision;
            context->conversation_revision = view->revision;
            context->preview_revision = view->preview_revision;
            context->preview = view->state == DIAL_AGENTS_READY && view->selected < view->page.count &&
                view->preview_state != DIAL_AGENTS_IDLE;
            memcpy(context->agent_name, view->agent.name, sizeof(context->agent_name));
            break;
        }
    }
    xSemaphoreGive(reader.lock);
}
