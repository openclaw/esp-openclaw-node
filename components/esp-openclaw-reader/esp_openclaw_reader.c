#include "esp_openclaw_reader.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_openclaw_chat.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

typedef enum { IDLE, CONNECTING, CONNECTED, STOPPING } reader_phase_t;
typedef enum { OFFLINE, STARTING, READY, ACCESS_NEEDED, ERROR } reader_status_t;
typedef enum { NONE, UP, LOST, AUTH_REJECTED, FAILED, CLOSED } reader_event_t;

static atomic_uint node_generation;
static atomic_uint pending_event;

typedef enum { AGENTS, SESSIONS, PREVIEW, CREATE } request_kind_t;
typedef enum { REQUEST_ALLOC = 1, RESPONSE_MISSING, RESPONSE_COPY, RESPONSE_PARSE,
               REQUEST_QUEUE, RESPONSE_LIMIT } response_stage_t;

static void response_diagnostic(request_kind_t kind, response_stage_t stage, int result, size_t bytes)
{
    ESP_LOGW("openclaw_reader", "reader_response_diag kind=%d stage=%d result=%d bytes=%u",
             (int)kind, stage, result, (unsigned)bytes);
}

typedef struct {
    request_kind_t kind;
    char agent_id[65], key[ESP_OPENCLAW_SESSION_KEY_BYTES];
    unsigned generation, revision;
    uint32_t connection;
    size_t offset;
    int64_t started;
    bool ok, too_large, timed_out, queued, rejected;
    char *json;
    union {
        esp_openclaw_agent_page_t agents;
        esp_openclaw_session_page_t sessions;
        esp_openclaw_session_preview_t preview;
        esp_openclaw_session_created_t created;
    } page;
} reader_request_t;

static _Atomic(reader_request_t *) completed_request;

typedef struct {
    unsigned revision, generation;
    uint32_t connection;
    bool waiting, ok;
    char *json;
} approval_request_t;

static _Atomic(approval_request_t *) completed_approval;

static struct {
    char display_name[128], model_identifier[64], tls_server_name[254];
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    esp_openclaw_node_handle_t node, client;
    char *uri;
    uint32_t node_id, reader_id;
    unsigned generation, attempt;
    reader_phase_t phase;
    reader_status_t status;
    bool retry;
    char *setup_code;
    unsigned setup_generation;
    int64_t setup_started;
    esp_openclaw_node_connect_lease_t setup_lease;
    esp_openclaw_node_connect_attempt_id_t setup_attempt;
    bool devices_initialized, devices_available, identify_available;
    bool directory_enabled, directory_pending, force_disconnect;
    size_t directory_offset;
    size_t session_page_size;
    bool compact_gateway_snapshots;
    esp_openclaw_reader_state_t directory_state;
    unsigned directory_revision;
    esp_openclaw_agent_page_t *directory_page;
    reader_request_t *inflight;
    esp_openclaw_reader_conversations_t *conversations;
    unsigned conversations_revision, preview_revision;
    bool sessions_pending, preview_pending;
    esp_openclaw_reader_messaging_snapshot_t messaging;
    approval_request_t *approval;
    int64_t approval_started;
    char *approved_token;
    bool write_registered, reset_client, restore_write, probing_write;
    esp_openclaw_chat_handle_t chat;
    esp_openclaw_reader_reply_snapshot_t reply;
    char reply_key[ESP_OPENCLAW_SESSION_KEY_BYTES];
    unsigned reply_generation;
    uint32_t reply_connection;
    bool reply_prepare, reply_send, reply_submitted;
    esp_openclaw_reader_creation_snapshot_t creation;
    esp_openclaw_session_created_t created;
    unsigned creation_generation;
    uint32_t creation_connection, creation_review_connection;
    bool creation_pending, creation_submitted;
} reader;

static void wipe_free(char *value)
{
    if (value == NULL) return;
    volatile char *cursor = value;
    size_t remaining = strlen(value);
    while (remaining--) *cursor++ = 0;
    free(value);
}

static void messaging_state(esp_openclaw_reader_messaging_state_t state)
{
    reader.messaging.state = state;
    ++reader.messaging.revision;
}

static void clear_approved_token(void)
{
    if (reader.approved_token != NULL) {
        memset(reader.approved_token, 0, strlen(reader.approved_token));
        free(reader.approved_token);
        reader.approved_token = NULL;
    }
}

static bool messaging_pending(void)
{
    return reader.messaging.state == ESP_OPENCLAW_READER_MESSAGING_REQUESTING ||
        reader.messaging.state == ESP_OPENCLAW_READER_MESSAGING_WAITING ||
        reader.messaging.state == ESP_OPENCLAW_READER_MESSAGING_CONNECTING;
}

static void reset_messaging(esp_openclaw_reader_messaging_state_t state)
{
    messaging_state(state);
    reader.messaging.request_id[0] = '\0';
    clear_approved_token();
    reader.reset_client = reader.write_registered;
}

static void wake_reader(void)
{
    xTaskNotifyGive(reader.task);
}

static bool current_node(void)
{
    return reader.generation == atomic_load(&node_generation) &&
        reader.node_id != 0 && reader.uri != NULL &&
        reader.node_id == esp_openclaw_node_get_connection_id(reader.node);
}

static bool current_reader(void)
{
    return current_node() && reader.phase == CONNECTED && !reader.force_disconnect &&
        reader.setup_lease == 0 &&
        atomic_load(&pending_event) == NONE &&
        reader.status == READY && reader.attempt == reader.generation &&
        reader.reader_id != 0 && reader.reader_id == esp_openclaw_node_get_connection_id(reader.client);
}

static bool identify_current(bool write)
{
    return current_reader() && (!write || reader.messaging.state == ESP_OPENCLAW_READER_MESSAGING_READY);
}

static bool reply_active(void)
{
    return reader.reply.state == ESP_OPENCLAW_READER_REPLY_SENDING || reader.reply.state == ESP_OPENCLAW_READER_REPLY_ACCEPTED;
}

static bool creation_owner(void)
{
    if (!current_reader() || reader.creation_generation != reader.generation ||
        reader.directory_state != ESP_OPENCLAW_READER_READY || reader.directory_page == NULL) return false;
    for (size_t i = 0; i < reader.directory_page->count; ++i)
        if (strcmp(reader.directory_page->agents[i].id, reader.creation.agent.id) == 0) return true;
    return false;
}

static bool creation_target(void)
{
    if (!creation_owner()) return false;
    const esp_openclaw_reader_conversations_t *view = reader.conversations;
    if (reader.creation_review_connection == reader.reader_id)
        return view != NULL && view->revision == reader.creation.conversation_revision &&
            strcmp(view->agent.id, reader.creation.agent.id) == 0;
    /* The write-approval reconnect clears the view. A fresh roster must attest
     * the captured agent before a reviewed intent may use the new connection. */
    return view == NULL || strcmp(view->agent.id, reader.creation.agent.id) == 0;
}

static void creation_state(esp_openclaw_reader_creation_state_t state, const char *detail)
{
    reader.creation.state = state;
    strcpy(reader.creation.detail, detail);
    ++reader.creation.revision;
}

static void poll_creation(void)
{
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    if (reader.creation.state == ESP_OPENCLAW_READER_CREATION_CREATING &&
        (reader.creation_submitted ? !current_reader() ||
            reader.creation_generation != reader.generation ||
            reader.creation_connection != reader.reader_id : !creation_target() ||
            reader.messaging.state != ESP_OPENCLAW_READER_MESSAGING_READY)) {
        reader.creation_pending = false;
        creation_state(reader.creation_submitted ? ESP_OPENCLAW_READER_CREATION_UNKNOWN : ESP_OPENCLAW_READER_CREATION_FAILED,
            reader.creation_submitted ? "Outcome unknown. Check Chats before creating another." :
                "Selection or connection changed. Nothing was queued.");
    }
    xSemaphoreGive(reader.lock);
}

static bool reply_owner(void)
{
    return current_reader() && reader.reply_generation == reader.generation &&
        reader.reply_connection == reader.reader_id;
}

static bool reply_target(void)
{
    const esp_openclaw_reader_conversations_t *view = reader.conversations;
    return reply_owner() && view != NULL && view->state == ESP_OPENCLAW_READER_READY &&
        view->selected < view->page.count &&
        strcmp(view->agent.id, reader.reply.agent.id) == 0 &&
        strcmp(view->page.sessions[view->selected].key, reader.reply_key) == 0;
}

static bool reply_sendable(void)
{
    return reply_target() && reader.messaging.state == ESP_OPENCLAW_READER_MESSAGING_READY &&
        reader.reply.preview_revision == reader.preview_revision;
}

static void reply_state(esp_openclaw_reader_reply_state_t state, const char *detail)
{
    if (reader.reply.state != state || strcmp(reader.reply.detail, detail) != 0) {
        reader.reply.state = state;
        strcpy(reader.reply.detail, detail);
        ++reader.reply.revision;
    }
}

static void release_chat(void)
{
    if (reader.chat != NULL) esp_openclaw_chat_destroy(reader.chat);
    reader.chat = NULL;
    reader.reply_prepare = reader.reply_send = false;
}

/* The SDK releases its state lock before forwarding events. Serialize this
 * handle with UI actions and worker destruction; RPC callbacks own their refs. */
static void reply_event(esp_openclaw_node_handle_t client, const char *event,
                        const char *payload, void *context)
{
    (void)context;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    if (client == reader.client && reader.chat != NULL)
        esp_openclaw_chat_event(reader.chat, client, event, payload);
    xSemaphoreGive(reader.lock);
    wake_reader();
}

static void poll_reply(void)
{
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    if (reader.reply_prepare && reply_sendable()) {
        reader.reply_prepare = false;
        if (esp_openclaw_chat_prepare(reader.client, reader.reply_key, reader.reply.agent.id,
                                      &reader.chat) != ESP_OK)
            reply_state(ESP_OPENCLAW_READER_REPLY_FAILED, "Could not prepare. Choose a reply again.");
    }
    if (reader.chat != NULL) {
        esp_openclaw_chat_snapshot_t chat;
        esp_openclaw_chat_poll(reader.chat);
        esp_openclaw_chat_copy(reader.chat, &chat);
        switch (chat.state) {
        case ESP_OPENCLAW_CHAT_PREPARING: break;
        case ESP_OPENCLAW_CHAT_READY:
            if (!reader.reply_send && reader.reply.state != ESP_OPENCLAW_READER_REPLY_REVIEW)
                reply_state(ESP_OPENCLAW_READER_REPLY_REVIEW, "Review before sending.");
            break;
        case ESP_OPENCLAW_CHAT_SENDING:
            reply_state(ESP_OPENCLAW_READER_REPLY_SENDING, "Waiting for the Gateway."); break;
        case ESP_OPENCLAW_CHAT_RUNNING:
            reply_state(ESP_OPENCLAW_READER_REPLY_ACCEPTED, "Accepted. Waiting for the agent."); break;
        case ESP_OPENCLAW_CHAT_COMPLETE:
            reply_state(ESP_OPENCLAW_READER_REPLY_SENT, "Sent. Check the conversation; agent work may continue."); break;
        case ESP_OPENCLAW_CHAT_HANDED_OFF:
            reply_state(ESP_OPENCLAW_READER_REPLY_SENT, "Handed off. Work may continue; check the conversation."); break;
        case ESP_OPENCLAW_CHAT_ABORTED:
            reply_state(ESP_OPENCLAW_READER_REPLY_FAILED, "Agent stopped. Check the conversation before retrying."); break;
        case ESP_OPENCLAW_CHAT_FAILED:
            reply_state(ESP_OPENCLAW_READER_REPLY_FAILED, reader.reply_submitted ?
                "Agent reported a failure. Check the conversation before retrying." :
                "Could not prepare. Choose a reply again."); break;
        case ESP_OPENCLAW_CHAT_OUTCOME_UNKNOWN:
            reply_state(ESP_OPENCLAW_READER_REPLY_UNKNOWN, "Outcome unknown. Check the conversation before retrying."); break;
        }
    }
    /* Selection changes revoke unsent drafts, but cannot retarget or abort an
     * submitted turn. A completed result survives a later transport loss. */
    bool lost = !reply_owner() || reader.messaging.state != ESP_OPENCLAW_READER_MESSAGING_READY;
    if ((!reader.reply_submitted && !reply_sendable()) || lost) {
        if (reader.reply.state == ESP_OPENCLAW_READER_REPLY_PREPARING || reader.reply.state == ESP_OPENCLAW_READER_REPLY_REVIEW ||
            reply_active())
            reply_state(reader.reply_submitted ? ESP_OPENCLAW_READER_REPLY_UNKNOWN : ESP_OPENCLAW_READER_REPLY_FAILED,
                reader.reply_submitted ? "Outcome unknown. Check the conversation before retrying." :
                "Selection or connection changed. Choose a reply again.");
        release_chat();
    } else if (reader.reply_send) {
        reader.reply_send = false;
        unsigned char random[16];
        char id[33];
        static const char hex[] = "0123456789abcdef";
        esp_fill_random(random, sizeof(random));
        for (size_t i = 0; i < sizeof(random); ++i) {
            id[i * 2] = hex[random[i] >> 4]; id[i * 2 + 1] = hex[random[i] & 15];
        }
        id[32] = '\0';
        if (esp_openclaw_chat_send(reader.chat, reader.reply.text, id) == ESP_OK)
            reader.reply_submitted = true;
        else reply_state(ESP_OPENCLAW_READER_REPLY_REVIEW, "Could not queue. Try Send again.");
    }
    xSemaphoreGive(reader.lock);
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
    if (reader.directory_enabled || reader.directory_state != ESP_OPENCLAW_READER_ERROR)
        reader.directory_state = reader.directory_enabled ? ESP_OPENCLAW_READER_LOADING : ESP_OPENCLAW_READER_IDLE;
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
    if (reader.generation != generation) {
        wipe_free(reader.setup_code);
        reader.setup_code = NULL;
        invalidate_directory();
        reset_messaging(ESP_OPENCLAW_READER_MESSAGING_OFF);
        reader.probing_write = false;
    }
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
        if (reader.attempt == reader.generation && current_node() &&
            !reader.force_disconnect && !reader.reset_client) {
            /* A saved operator token can belong to the previous Gateway. A
             * failed URI allocation is retryable, not a credential failure. */
            bool same_gateway = uri != NULL && strcmp(uri, reader.uri) == 0;
            reader.reader_id = same_gateway ? id : 0;
            reader.status = uri == NULL || id == 0 ? ERROR : same_gateway ? READY : ACCESS_NEEDED;
            reader.retry = uri == NULL || id == 0;
            if (reader.messaging.state == ESP_OPENCLAW_READER_MESSAGING_CONNECTING &&
                reader.write_registered && !reader.reset_client && reader.status == READY) {
                clear_approved_token();
                messaging_state(ESP_OPENCLAW_READER_MESSAGING_READY);
                reader.restore_write = true;
                reader.probing_write = false;
            }
            if (reader.probing_write && reader.status == ACCESS_NEEDED) {
                reader.restore_write = reader.probing_write = false;
                reset_messaging(ESP_OPENCLAW_READER_MESSAGING_OFF);
                reader.force_disconnect = reader.retry = true;
            }
        }
    } else {
        bool restore_denied = reader.probing_write && event == AUTH_REJECTED;
        if (event == AUTH_REJECTED && reader.write_registered) reader.restore_write = false;
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
        if (reader.messaging.state == ESP_OPENCLAW_READER_MESSAGING_REQUESTING ||
            reader.messaging.state == ESP_OPENCLAW_READER_MESSAGING_WAITING) {
            reset_messaging(ESP_OPENCLAW_READER_MESSAGING_ERROR);
        } else if (reader.write_registered && !reader.reset_client) {
            if (event == LOST) messaging_state(ESP_OPENCLAW_READER_MESSAGING_CONNECTING);
            else if (event != CLOSED) {
                reset_messaging(ESP_OPENCLAW_READER_MESSAGING_ERROR);
                reader.retry = current_node();
            }
        }
        if (restore_denied) {
            reader.probing_write = false;
            reset_messaging(ESP_OPENCLAW_READER_MESSAGING_OFF);
            reader.status = current_node() ? STARTING : OFFLINE;
            reader.retry = current_node();
        }
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
    /* These validation/authority errors precede creation. Transport errors,
     * missing error metadata, and server availability failures remain unknown. */
    request->rejected = !result->ok && result->error_code != NULL &&
        (strcmp(result->error_code, "INVALID_REQUEST") == 0 ||
         strcmp(result->error_code, "FORBIDDEN") == 0 ||
         strcmp(result->error_code, "STALE_CONNECTION") == 0);
    if (!request->ok) response_diagnostic(request->kind, RESPONSE_MISSING, result->ok ? 1 : 0, 0);
    if (request->ok) {
        size_t length = strnlen(result->payload_json, ESP_OPENCLAW_DIRECTORY_MAX_JSON_BYTES + 1);
        request->too_large = length > ESP_OPENCLAW_DIRECTORY_MAX_JSON_BYTES;
        if (request->too_large) response_diagnostic(request->kind, RESPONSE_LIMIT, 0, length);
        if (!request->too_large) {
            request->json = malloc(length + 1);
            if (request->json != NULL) memcpy(request->json, result->payload_json, length + 1);
            else response_diagnostic(request->kind, RESPONSE_COPY, ESP_ERR_NO_MEM, length + 1);
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
    esp_openclaw_reader_state_t state = request->too_large ? ESP_OPENCLAW_READER_TOO_LARGE : ESP_OPENCLAW_READER_ERROR;
    if (request->ok && request->json != NULL) {
        esp_openclaw_directory_result_t parsed;
        size_t length = strlen(request->json);
        if (request->kind == AGENTS) {
            parsed = esp_openclaw_directory_parse(request->json, length, request->offset, &request->page.agents);
            if (parsed == ESP_OPENCLAW_DIRECTORY_OK)
                state = request->page.agents.count == 0 ? ESP_OPENCLAW_READER_EMPTY : ESP_OPENCLAW_READER_READY;
        } else if (request->kind == SESSIONS) {
            parsed = esp_openclaw_sessions_parse(request->json, length, request->agent_id,
                                                request->offset, &request->page.sessions);
            if (parsed == ESP_OPENCLAW_DIRECTORY_OK)
                state = request->page.sessions.count == 0 ? ESP_OPENCLAW_READER_EMPTY : ESP_OPENCLAW_READER_READY;
        } else if (request->kind == CREATE) {
            parsed = esp_openclaw_session_created_parse(request->json, length, request->agent_id,
                                                       &request->page.created);
            if (parsed == ESP_OPENCLAW_DIRECTORY_OK) state = ESP_OPENCLAW_READER_READY;
        } else {
            parsed = esp_openclaw_preview_parse(request->json, length, request->key, &request->page.preview);
            if (parsed == ESP_OPENCLAW_DIRECTORY_OK) state = ESP_OPENCLAW_READER_READY;
        }
        if (parsed == ESP_OPENCLAW_DIRECTORY_TOO_LARGE) state = ESP_OPENCLAW_READER_TOO_LARGE;
        if (parsed != ESP_OPENCLAW_DIRECTORY_OK) response_diagnostic(request->kind, RESPONSE_PARSE, parsed, length);
    }
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    reader.inflight = NULL;
    if (request->kind == CREATE) {
        /* A conclusive response already received survives a subsequent operator
         * disconnect. It never crosses a primary-node replacement or timeout. */
        if (reader.creation.state == ESP_OPENCLAW_READER_CREATION_CREATING &&
            request->revision == reader.creation.revision &&
            request->generation == reader.generation && current_node() &&
            request->connection == reader.creation_connection) {
            if (state == ESP_OPENCLAW_READER_READY) {
                reader.created = request->page.created;
                creation_state(ESP_OPENCLAW_READER_CREATION_CREATED, "Created. Open it to choose a reply.");
            } else {
                bool unknown = request->queued && !request->rejected;
                creation_state(unknown ? ESP_OPENCLAW_READER_CREATION_UNKNOWN : ESP_OPENCLAW_READER_CREATION_FAILED,
                    unknown ? "Outcome unknown. Check Chats before creating another." :
                    request->rejected ? "Gateway rejected creation. Check access or refresh." :
                        "Could not queue. Review and try again.");
            }
        }
    } else if (request->generation == reader.generation && current_reader() &&
               request->connection == reader.reader_id) {
        esp_openclaw_reader_conversations_t *view = reader.conversations;
        if (request->kind == AGENTS && request->revision == reader.directory_revision) {
            free(reader.directory_page);
            reader.directory_page = NULL;
            if (state == ESP_OPENCLAW_READER_READY || state == ESP_OPENCLAW_READER_EMPTY) {
                reader.directory_page = malloc(sizeof(*reader.directory_page));
                if (reader.directory_page != NULL) *reader.directory_page = request->page.agents;
                else state = ESP_OPENCLAW_READER_ERROR;
            }
            reader.directory_state = state;
        } else if (request->kind == SESSIONS && view != NULL &&
                   request->revision == reader.conversations_revision) {
            if (state == ESP_OPENCLAW_READER_READY || state == ESP_OPENCLAW_READER_EMPTY) view->page = request->page.sessions;
            view->state = state;
        } else if (request->kind == PREVIEW && view != NULL &&
                   request->revision == reader.preview_revision) {
            if (state == ESP_OPENCLAW_READER_READY) view->preview = request->page.preview;
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
    if (request->kind == CREATE) {
        ok = cJSON_AddStringToObject(root, "agentId", request->agent_id) != NULL &&
            cJSON_AddStringToObject(root, "idempotencyKey", request->key) != NULL;
    } else if (request->kind == SESSIONS) {
        ok = cJSON_AddStringToObject(root, "agentId", request->agent_id) != NULL &&
            cJSON_AddStringToObject(root, "rowMode", "dashboard") != NULL &&
            cJSON_AddNumberToObject(root, "limit", reader.session_page_size) != NULL &&
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
    bool pending = reader.creation_pending || reader.directory_pending || reader.sessions_pending || reader.preview_pending;
    if (!pending || reader.inflight != NULL || !current_reader()) {
        xSemaphoreGive(reader.lock);
        return;
    }
    request_kind_t kind = reader.creation_pending ? CREATE : reader.preview_pending ? PREVIEW : reader.sessions_pending ? SESSIONS : AGENTS;
    if (kind == CREATE && (!creation_target() || reader.messaging.state != ESP_OPENCLAW_READER_MESSAGING_READY || reply_active())) {
        reader.creation_pending = false;
        creation_state(ESP_OPENCLAW_READER_CREATION_FAILED, "Selection or connection changed. Nothing was queued.");
        xSemaphoreGive(reader.lock);
        return;
    }
    reader_request_t *request = calloc(1, sizeof(*request));
    if (request == NULL) {
        response_diagnostic(kind, REQUEST_ALLOC, ESP_ERR_NO_MEM, sizeof(*request));
        if (kind == CREATE) {
            reader.creation_pending = false;
            creation_state(ESP_OPENCLAW_READER_CREATION_FAILED, "Could not queue. Review and try again.");
        } else if (kind == AGENTS) {
            reader.directory_pending = false;
            reader.directory_state = ESP_OPENCLAW_READER_ERROR;
        } else if (kind == SESSIONS) {
            reader.sessions_pending = false;
            reader.conversations->state = ESP_OPENCLAW_READER_ERROR;
        } else {
            reader.preview_pending = false;
            reader.conversations->preview_state = ESP_OPENCLAW_READER_ERROR;
        }
        xSemaphoreGive(reader.lock);
        return;
    }
    request->kind = kind;
    request->generation = reader.generation;
    request->connection = reader.reader_id;
    request->started = esp_timer_get_time();
    if (kind == CREATE) {
        reader.creation_pending = false;
        reader.creation_connection = reader.reader_id;
        request->revision = reader.creation.revision;
        strcpy(request->agent_id, reader.creation.agent.id);
        unsigned char random[16];
        static const char hex[] = "0123456789abcdef";
        esp_fill_random(random, sizeof(random));
        for (size_t i = 0; i < sizeof(random); ++i) {
            request->key[i * 2] = hex[random[i] >> 4];
            request->key[i * 2 + 1] = hex[random[i] & 15];
        }
    } else if (kind == AGENTS) {
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
    const char *method = kind == CREATE ? "sessions.create" : kind == AGENTS ? "agents.list" :
        kind == SESSIONS ? "sessions.list" : "sessions.preview";
    esp_err_t err = ESP_ERR_NO_MEM;
    if (kind == CREATE) {
        /* Serialization allocates outside the owner lock. Revalidate at the
         * enqueue boundary; the SDK queues without waiting and callbacks only
         * publish the mailbox, so no callback needs this lock. */
        xSemaphoreTake(reader.lock, portMAX_DELAY);
        bool current = request->revision == reader.creation.revision && creation_target() &&
            reader.messaging.state == ESP_OPENCLAW_READER_MESSAGING_READY && !reply_active();
        if (!current) err = ESP_ERR_INVALID_STATE;
        else if (params != NULL) err = esp_openclaw_node_gateway_request_on_connection(
            reader.client, request->connection, method, params, reader_response, request);
        request->queued = err == ESP_OK;
        reader.creation_submitted = request->queued;
        xSemaphoreGive(reader.lock);
    } else {
        err = params != NULL ? esp_openclaw_node_gateway_request_on_connection(
            reader.client, request->connection, method, params, reader_response, request) : ESP_ERR_NO_MEM;
    }
    free(params);
    if (err != ESP_OK) {
        response_diagnostic(kind, REQUEST_QUEUE, err, 0);
        /* Immediate rejection owns no callback; use the same completion path. */
        atomic_store(&completed_request, request);
        xTaskNotifyGive(reader.task);
    }
}

static esp_err_t create_client(void)
{
    esp_openclaw_node_config_t config;
    esp_openclaw_node_config_init_default(&config);
    config.display_name = reader.display_name;
    config.model_identifier = reader.model_identifier[0] ? reader.model_identifier : NULL;
    config.tls_common_name = reader.tls_server_name[0] ? reader.tls_server_name : NULL;
    config.role = "operator";
    config.event_cb = reader_event;
    config.gateway_event_cb = reply_event;
    esp_err_t err = esp_openclaw_node_create(&config, &reader.client);
    if (err == ESP_OK) {
        if (reader.compact_gateway_snapshots) {
            static const char *const caps[] = {
                "health-summary", "hello-health-on-demand", "hello-presence-self", "chat-only-assistant-text",
            };
            for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]) && err == ESP_OK; ++i)
                err = esp_openclaw_node_register_capability(reader.client, caps[i]);
        }
        if (err == ESP_OK)
            err = esp_openclaw_node_register_scope(reader.client, "operator.read");
        if (err != ESP_OK) {
            esp_openclaw_node_destroy(reader.client);
            reader.client = NULL;
        }
    }
    return err;
}

static void approval_response(esp_openclaw_node_handle_t client,
                              const esp_openclaw_node_gateway_result_t *result, void *context)
{
    (void)client;
    approval_request_t *request = context;
    if (result->ok && result->payload_json != NULL) {
        size_t length = strnlen(result->payload_json, 4097);
        if (length <= 4096) {
            request->json = malloc(length + 1);
            if (request->json != NULL) {
                memcpy(request->json, result->payload_json, length + 1);
                request->ok = true;
            }
        }
    }
    atomic_store(&completed_approval, request);
    wake_reader();
}

static bool has_write_scopes(const cJSON *root)
{
    bool read = false, write = false;
    const cJSON *scopes = cJSON_GetObjectItemCaseSensitive(root, "scopes"), *scope;
    if (!cJSON_IsArray(scopes)) return false;
    cJSON_ArrayForEach(scope, scopes) {
        if (!cJSON_IsString(scope)) return false;
        read |= strcmp(scope->valuestring, "operator.read") == 0;
        write |= strcmp(scope->valuestring, "operator.write") == 0;
    }
    return read && write;
}

static cJSON *parse_approval(const char *json)
{
    if (json == NULL) return NULL;
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; json[i] != '\0'; ++i) {
        char c = json[i];
        if (quoted) {
            if (escaped) {
                /* Do not let a decoded NUL truncate an approval ID or token. */
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

static void consume_approval(void)
{
    approval_request_t *request = atomic_exchange(&completed_approval, NULL);
    if (request == NULL) return;
    cJSON *root = request->ok ? parse_approval(request->json) : NULL;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "requestId");
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    reader.approval = NULL;
    /* The SDK can deliver this reply and then close the socket before the
     * worker runs. Preserve its rotated token using the captured owner, while
     * a canceled intent or changed primary connection still invalidates it. */
    if (request->revision == reader.messaging.revision &&
        request->generation == reader.generation && current_node() &&
        reader.attempt == reader.generation && reader.phase == CONNECTED &&
        request->connection != 0 && request->connection == reader.reader_id) {
        bool valid_id = cJSON_IsString(id) && id->valuestring[0] != '\0' &&
            strlen(id->valuestring) < sizeof(reader.messaging.request_id);
        if (!request->waiting) {
            if (valid_id) {
                strcpy(reader.messaging.request_id, id->valuestring);
                messaging_state(ESP_OPENCLAW_READER_MESSAGING_WAITING);
            } else messaging_state(ESP_OPENCLAW_READER_MESSAGING_ERROR);
        } else {
            const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
            const cJSON *token = cJSON_GetObjectItemCaseSensitive(root, "deviceToken");
            bool matches = valid_id && strcmp(id->valuestring, reader.messaging.request_id) == 0 &&
                cJSON_IsString(status);
            if (matches && strcmp(status->valuestring, "rejected") == 0)
                messaging_state(ESP_OPENCLAW_READER_MESSAGING_REJECTED);
            else if (matches && strcmp(status->valuestring, "expired") == 0)
                messaging_state(ESP_OPENCLAW_READER_MESSAGING_EXPIRED);
            else if (matches && strcmp(status->valuestring, "approved") == 0 &&
                     cJSON_IsString(token) && token->valuestring[0] != '\0' &&
                     strlen(token->valuestring) <= 2048 && has_write_scopes(root) &&
                     (reader.approved_token = strdup(token->valuestring)) != NULL) {
                messaging_state(ESP_OPENCLAW_READER_MESSAGING_CONNECTING);
                reader.force_disconnect = reader.retry = true;
            } else messaging_state(ESP_OPENCLAW_READER_MESSAGING_ERROR);
        }
    }
    xSemaphoreGive(reader.lock);
    cJSON_Delete(root);
    if (request->json != NULL) memset(request->json, 0, strlen(request->json));
    free(request->json);
    free(request);
}

static void request_approval(void)
{
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    if (reader.approval != NULL || !current_reader() ||
        (reader.messaging.state != ESP_OPENCLAW_READER_MESSAGING_REQUESTING && reader.messaging.state != ESP_OPENCLAW_READER_MESSAGING_WAITING)) {
        xSemaphoreGive(reader.lock);
        return;
    }
    approval_request_t *request = calloc(1, sizeof(*request));
    if (request == NULL) {
        messaging_state(ESP_OPENCLAW_READER_MESSAGING_ERROR);
        xSemaphoreGive(reader.lock);
        return;
    }
    request->revision = reader.messaging.revision;
    request->generation = reader.generation;
    request->connection = reader.reader_id;
    request->waiting = reader.messaging.state == ESP_OPENCLAW_READER_MESSAGING_WAITING;
    cJSON *params = cJSON_CreateObject();
    bool ok;
    if (request->waiting)
        ok = cJSON_AddStringToObject(params, "requestId", reader.messaging.request_id) != NULL;
    else {
        const char *scopes[] = {"operator.read", "operator.write"};
        cJSON *array = cJSON_CreateStringArray(scopes, 2);
        ok = array != NULL && cJSON_AddItemToObject(params, "scopes", array);
        if (!ok) cJSON_Delete(array);
    }
    char *json = ok ? cJSON_PrintUnformatted(params) : NULL;
    cJSON_Delete(params);
    reader.approval = request;
    xSemaphoreGive(reader.lock);
    esp_err_t err = json != NULL ? esp_openclaw_node_gateway_request_on_connection(
        reader.client, request->connection,
        request->waiting ? "device.scopes.waitUpgrade" : "device.scopes.requestUpgrade",
        json, approval_response, request) : ESP_ERR_NO_MEM;
    free(json);
    if (err != ESP_OK) {
        atomic_store(&completed_approval, request);
        wake_reader();
    }
}

static void reset_idle_client(void)
{
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    bool reset = reader.reset_client && reader.phase == IDLE &&
        reader.inflight == NULL && reader.approval == NULL && reader.setup_lease == 0;
    if (reset && reader.devices_available)
        esp_openclaw_devices_set_owner_locked(NULL, reader.generation, 0, false);
    xSemaphoreGive(reader.lock);
    if (!reset) return;
    /* Registration is idle-only and has no unregister operation. Recreate the
     * operator after canceled/failed elevation; the independent node stays up. */
    esp_err_t err = esp_openclaw_node_destroy(reader.client);
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    if (err == ESP_OK) {
        reader.client = NULL;
        reader.write_registered = reader.reset_client = false;
        reader.force_disconnect = false;
    }
    xSemaphoreGive(reader.lock);
}

static bool setup_authorized(void *context)
{
    (void)context;
    /* Immutable until the tracked attempt is terminal. The primary callback
     * revokes this generation on any connection change, including a new URI.
     * Never acquire reader/node locks from this node-worker callback. */
    int64_t elapsed = esp_timer_get_time() - reader.setup_started;
    return reader.setup_generation == atomic_load(&node_generation) && elapsed >= 0 && elapsed < 60000000;
}

static void poll_setup(void)
{
    if (reader.setup_lease == 0) return;
    if (reader.setup_attempt != 0) {
        esp_openclaw_node_connect_attempt_t attempt;
        if (esp_openclaw_node_connect_snapshot(reader.client, reader.setup_lease,
                reader.setup_attempt, &attempt) != ESP_OK) return;
        bool terminal = attempt.state == ESP_OPENCLAW_NODE_CONNECT_ATTEMPT_SUCCEEDED ||
            attempt.state == ESP_OPENCLAW_NODE_CONNECT_ATTEMPT_FAILED;
        if (!terminal) {
            if (!setup_authorized(NULL))
                esp_openclaw_node_connect_cancel(reader.client, reader.setup_lease, reader.setup_attempt);
            return;
        }
    }
    if (esp_openclaw_node_connect_lease_release(reader.client, reader.setup_lease) == ESP_OK) {
        xSemaphoreTake(reader.lock, portMAX_DELAY);
        reader.setup_lease = 0;
        reader.setup_attempt = 0;
        xSemaphoreGive(reader.lock);
    }
}

static esp_err_t connect_reader(void)
{
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    char *code = reader.setup_code;
    reader.setup_code = NULL;
    xSemaphoreGive(reader.lock);
    esp_err_t err = reader.client != NULL ? ESP_OK : create_client();
    if (code != NULL) {
        if (err == ESP_OK) {
            reader.setup_started = esp_timer_get_time();
            if (!setup_authorized(NULL)) err = ESP_ERR_INVALID_STATE;
        }
        if (err == ESP_OK) {
            esp_openclaw_node_connect_lease_t lease = 0;
            err = esp_openclaw_node_connect_lease_acquire(reader.client, &lease);
            xSemaphoreTake(reader.lock, portMAX_DELAY);
            reader.setup_lease = lease;
            xSemaphoreGive(reader.lock);
        }
        if (err == ESP_OK) {
            const esp_openclaw_node_connect_request_t request = {
                .source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_SETUP_CODE, .value = code,
            };
            err = esp_openclaw_node_connect_begin_authorized(reader.client, reader.setup_lease,
                &request, setup_authorized, NULL, &reader.setup_attempt);
        }
        wipe_free(code);
        poll_setup();
        return err;
    }
    if (err != ESP_OK) return err;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    bool has_token = reader.approved_token != NULL;
    char *token = has_token ? strdup(reader.approved_token) : NULL;
    unsigned revision = reader.messaging.revision;
    bool restore = !has_token && !reader.write_registered && reader.restore_write;
    xSemaphoreGive(reader.lock);
    if (has_token && token == NULL) return ESP_ERR_NO_MEM;
    if (restore && !esp_openclaw_node_has_saved_session(reader.client)) {
        xSemaphoreTake(reader.lock, portMAX_DELAY);
        reader.restore_write = false;
        xSemaphoreGive(reader.lock);
        restore = false;
    }
    if (token != NULL || restore) {
        err = esp_openclaw_node_register_scope(reader.client, "operator.write");
        xSemaphoreTake(reader.lock, portMAX_DELAY);
        if (err == ESP_OK) reader.write_registered = true;
        bool current = current_node() && (restore ? reader.attempt == reader.generation :
            revision == reader.messaging.revision && reader.messaging.state == ESP_OPENCLAW_READER_MESSAGING_CONNECTING);
        if (!current) {
            reader.reset_client = reader.write_registered;
            err = ESP_ERR_INVALID_STATE;
        } else if (restore && err == ESP_OK) {
            reader.probing_write = true;
            messaging_state(ESP_OPENCLAW_READER_MESSAGING_CONNECTING);
        }
        xSemaphoreGive(reader.lock);
    }
    if (err == ESP_OK) {
        const esp_openclaw_node_connect_request_t request = {
            .source = token != NULL ? ESP_OPENCLAW_NODE_CONNECT_SOURCE_DEVICE_TOKEN :
                ESP_OPENCLAW_NODE_CONNECT_SOURCE_SAVED_SESSION,
            .gateway_uri = token != NULL ? reader.uri : NULL,
            .value = token,
        };
        err = esp_openclaw_node_request_connect(reader.client, &request);
    }
    if (restore && err != ESP_OK) {
        xSemaphoreTake(reader.lock, portMAX_DELAY);
        reader.restore_write = reader.restore_write &&
            (err == ESP_ERR_NO_MEM || err == ESP_ERR_INVALID_STATE || err == ESP_FAIL);
        reader.probing_write = false;
        reset_messaging(ESP_OPENCLAW_READER_MESSAGING_OFF);
        xSemaphoreGive(reader.lock);
    }
    if (token != NULL) {
        memset(token, 0, strlen(token));
        free(token);
        if (err != ESP_OK && err != ESP_ERR_NO_MEM && err != ESP_ERR_INVALID_STATE && err != ESP_FAIL) {
            xSemaphoreTake(reader.lock, portMAX_DELAY);
            if (revision == reader.messaging.revision) reset_messaging(ESP_OPENCLAW_READER_MESSAGING_ERROR);
            xSemaphoreGive(reader.lock);
        }
    }
    return err;
}

static void reader_task(void *context)
{
    (void)context;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    if (!reader.devices_initialized) {
        reader.devices_available = esp_openclaw_devices_init(reader.lock, wake_reader) == ESP_OK;
        if (reader.devices_available)
            reader.identify_available = esp_openclaw_identify_init(reader.lock, wake_reader, identify_current) == ESP_OK;
        reader.devices_initialized = true;
    }
    xSemaphoreGive(reader.lock);
    for (;;) {
        /* Also retry a rejected queue submission: it produces no SDK event.
         * This bounds reconnects without polling NVS or retrying auth failures. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        refresh_node();
        consume_approval();
        consume_response();
        consume_event();
        poll_creation();
        poll_reply();
        poll_setup();
        reset_idle_client();
        xSemaphoreTake(reader.lock, portMAX_DELAY);
        if ((reader.messaging.state == ESP_OPENCLAW_READER_MESSAGING_REQUESTING ||
             reader.messaging.state == ESP_OPENCLAW_READER_MESSAGING_WAITING) &&
            esp_timer_get_time() - reader.approval_started >=
                (reader.messaging.state == ESP_OPENCLAW_READER_MESSAGING_WAITING ? 315000000 : 15000000)) {
            reset_messaging(reader.messaging.state == ESP_OPENCLAW_READER_MESSAGING_WAITING ?
                ESP_OPENCLAW_READER_MESSAGING_EXPIRED : ESP_OPENCLAW_READER_MESSAGING_ERROR);
            reader.force_disconnect = reader.approval != NULL;
            reader.retry = true;
        }
        if (reader.inflight != NULL && !reader.inflight->timed_out &&
            esp_timer_get_time() - reader.inflight->started >=
                (reader.inflight->kind == CREATE ? 30000000 : 15000000)) {
            reader.inflight->timed_out = true;
            if (reader.inflight->kind == CREATE) {
                creation_state(ESP_OPENCLAW_READER_CREATION_UNKNOWN, "Outcome unknown. Check Chats before creating another.");
            } else {
                ++reader.directory_revision;
                clear_conversations();
                reader.directory_state = ESP_OPENCLAW_READER_ERROR;
                reader.directory_enabled = reader.directory_pending = false;
            }
        }
        if (reader.inflight != NULL && reader.inflight->timed_out &&
            reader.messaging.state != ESP_OPENCLAW_READER_MESSAGING_REQUESTING &&
            reader.messaging.state != ESP_OPENCLAW_READER_MESSAGING_WAITING && !reply_active()) {
            /* Report the read deadline immediately, but keep an outstanding
             * approval wait or submitted chat alive until its own deadline. Closing the
             * socket earlier can orphan a newly rotated operator token. */
            reader.force_disconnect = reader.retry = true;
        }
        bool stale = reader.attempt != reader.generation || !current_node();
        bool stop = reader.phase != IDLE && reader.phase != STOPPING &&
            (stale || reader.force_disconnect || (reader.phase == CONNECTED && reader.status != READY));
        bool start = reader.phase == IDLE && reader.retry && current_node() &&
            !reader.reset_client && reader.inflight == NULL && reader.approval == NULL && reader.setup_lease == 0;
        unsigned attempt = reader.attempt;
        if (start) {
            reader.attempt = reader.generation;
            attempt = reader.attempt;
            reader.phase = CONNECTING;
            reader.status = STARTING;
            reader.retry = false;
        }
        if (reader.devices_available)
            esp_openclaw_devices_set_owner_locked(reader.client, reader.generation,
                reader.reader_id, !stop && !start && current_reader());
        if (reader.identify_available)
            esp_openclaw_identify_set_owner_locked(reader.client, reader.generation,
                reader.reader_id, !stop && !start && current_reader(),
                reader.messaging.state == ESP_OPENCLAW_READER_MESSAGING_READY);
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
            esp_err_t err = connect_reader();
            if (err != ESP_OK) {
                xSemaphoreTake(reader.lock, portMAX_DELAY);
                reader.phase = IDLE;
                if (attempt == reader.generation) {
                    reader.status = err == ESP_ERR_NOT_FOUND ? ACCESS_NEEDED : ERROR;
                    reader.retry = reader.reset_client || err == ESP_ERR_NO_MEM || err == ESP_ERR_INVALID_STATE || err == ESP_FAIL;
                } else {
                    reader.retry = current_node();
                }
                xSemaphoreGive(reader.lock);
            }
        }
        request_approval();
        request_next();
        xSemaphoreTake(reader.lock, portMAX_DELAY);
        bool defer_devices = reader.inflight != NULL;
        bool devices_available = reader.devices_available;
        bool identify_available = reader.identify_available;
        xSemaphoreGive(reader.lock);
        if (devices_available) esp_openclaw_devices_poll(defer_devices);
        if (identify_available) esp_openclaw_identify_poll(defer_devices);
    }
}

esp_err_t esp_openclaw_reader_start(esp_openclaw_node_handle_t node,
                                   const esp_openclaw_reader_config_t *config)
{
    if (node == NULL) return ESP_ERR_INVALID_ARG;
    if (reader.lock != NULL) return ESP_ERR_INVALID_STATE;
    const char *name = config != NULL && config->display_name != NULL ?
        config->display_name : "OpenClaw Reader";
    const char *model = config != NULL && config->model_identifier != NULL ? config->model_identifier : "";
    const char *tls = config != NULL && config->tls_server_name != NULL ? config->tls_server_name : "";
    size_t page_size = config != NULL && config->session_page_size != 0 ?
        config->session_page_size : ESP_OPENCLAW_DIRECTORY_PAGE_SIZE;
    if (strlen(name) >= sizeof(reader.display_name) ||
        strlen(model) >= sizeof(reader.model_identifier) || strlen(tls) >= sizeof(reader.tls_server_name) ||
        page_size > ESP_OPENCLAW_DIRECTORY_PAGE_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    strcpy(reader.display_name, name);
    strcpy(reader.model_identifier, model);
    strcpy(reader.tls_server_name, tls);
    reader.session_page_size = page_size;
    reader.compact_gateway_snapshots = config != NULL && config->compact_gateway_snapshots;
    reader.lock = xSemaphoreCreateMutex();
    if (reader.lock == NULL) return ESP_ERR_NO_MEM;
    reader.node = node;
    reader.restore_write = true;
    if (xTaskCreate(reader_task, "openclaw_reader", 4096, NULL, 5, &reader.task) != pdPASS) {
        vSemaphoreDelete(reader.lock);
        reader.lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void esp_openclaw_reader_node_event(esp_openclaw_node_event_t event)
{
    (void)event;
    if (reader.task == NULL) return;
    atomic_fetch_add(&node_generation, 1);
    xTaskNotifyGive(reader.task);
}

cJSON *esp_openclaw_reader_status(void)
{
    const char *state = "unavailable";
    bool ready = false;
    bool setup_pending = false;
    if (reader.lock != NULL) {
        static const char *states[] = {"offline", "connecting", "ready", "access-needed", "error"};
        xSemaphoreTake(reader.lock, portMAX_DELAY);
        ready = current_reader();
        setup_pending = reader.setup_code != NULL || reader.setup_lease != 0;
        state = reader.generation != atomic_load(&node_generation) || reader.node_id == 0 ||
            reader.node_id != esp_openclaw_node_get_connection_id(reader.node) ? "offline" : reader.status == READY && !ready ? "connecting" : states[reader.status];
        xSemaphoreGive(reader.lock);
    }
    cJSON *status = cJSON_CreateObject();
    if (status == NULL) return NULL;
    if (cJSON_AddStringToObject(status, "state", state) == NULL ||
        cJSON_AddBoolToObject(status, "readConnected", ready) == NULL ||
        cJSON_AddBoolToObject(status, "setupPending", setup_pending) == NULL) {
        cJSON_Delete(status);
        return NULL;
    }
    return status;
}

esp_err_t esp_openclaw_reader_setup_code(const char *code)
{
    if (code == NULL || strnlen(code, ESP_OPENCLAW_NODE_BOOTSTRAP_CODE_MAX_LENGTH + 1) >
        ESP_OPENCLAW_NODE_BOOTSTRAP_CODE_MAX_LENGTH) return ESP_ERR_INVALID_ARG;
    if (reader.lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (current_node() && reader.phase == IDLE && reader.status != READY &&
        reader.setup_code == NULL && reader.setup_lease == 0 && !reader.reset_client &&
        reader.inflight == NULL && reader.approval == NULL && !messaging_pending() &&
        !reader.write_registered && !reply_active() && !reader.reply_prepare && !reader.reply_send &&
        !reader.creation_pending && !reader.creation_submitted) {
        err = esp_openclaw_node_validate_bootstrap_code(code, reader.uri);
        if (err == ESP_OK) {
            reader.setup_code = strdup(code);
            if (reader.setup_code == NULL) err = ESP_ERR_NO_MEM;
            else {
                reader.setup_generation = reader.generation;
                reader.retry = true;
            }
        }
    }
    xSemaphoreGive(reader.lock);
    if (err == ESP_OK) wake_reader();
    return err;
}

void esp_openclaw_reader_messaging_copy(esp_openclaw_reader_messaging_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    if (reader.lock == NULL) return;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    *snapshot = reader.messaging;
    /* Revoke displayed authority immediately, before the worker sees a loss. */
    if (snapshot->state == ESP_OPENCLAW_READER_MESSAGING_READY && !current_reader())
        snapshot->state = ESP_OPENCLAW_READER_MESSAGING_CONNECTING;
    if (!current_node()) {
        snapshot->state = ESP_OPENCLAW_READER_MESSAGING_OFF;
        snapshot->request_id[0] = '\0';
    }
    xSemaphoreGive(reader.lock);
}

esp_err_t esp_openclaw_reader_messaging_request(unsigned revision)
{
    if (reader.lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    if (revision != reader.messaging.revision || !current_reader() ||
        reader.reset_client || reader.approval != NULL || reader.write_registered ||
        reader.messaging.state == ESP_OPENCLAW_READER_MESSAGING_READY || messaging_pending()) {
        xSemaphoreGive(reader.lock);
        return ESP_ERR_INVALID_STATE;
    }
    reader.messaging.request_id[0] = '\0';
    messaging_state(ESP_OPENCLAW_READER_MESSAGING_REQUESTING);
    reader.approval_started = esp_timer_get_time();
    xSemaphoreGive(reader.lock);
    wake_reader();
    return ESP_OK;
}

esp_err_t esp_openclaw_reader_messaging_cancel(unsigned revision)
{
    if (reader.lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    if (revision != reader.messaging.revision ||
        (!messaging_pending() && reader.messaging.state != ESP_OPENCLAW_READER_MESSAGING_READY)) {
        xSemaphoreGive(reader.lock);
        return ESP_ERR_INVALID_STATE;
    }
    reader.restore_write = reader.probing_write = false;
    reset_messaging(ESP_OPENCLAW_READER_MESSAGING_OFF);
    /* Closing only the operator completes the outstanding wait callback. It
     * cannot revoke a grant the administrator may already have approved. */
    reader.force_disconnect = reader.phase != IDLE;
    reader.retry = true;
    xSemaphoreGive(reader.lock);
    wake_reader();
    return ESP_OK;
}

esp_err_t esp_openclaw_reader_reply_begin(esp_openclaw_reader_reply_preset_t preset, unsigned preview_revision)
{
    static const char *texts[] = {"Please continue.", "Please summarize the conversation so far.",
                                  "Please explain the last response in more detail."};
    if ((unsigned)preset >= ESP_OPENCLAW_READER_REPLY_PRESET_COUNT) return ESP_ERR_INVALID_ARG;
    return esp_openclaw_reader_reply_begin_text(texts[preset], strlen(texts[preset]), preview_revision);
}

esp_err_t esp_openclaw_reader_reply_begin_text(const char *text, size_t length, unsigned preview_revision)
{
    char draft[sizeof(reader.reply.text)];
    if (reader.lock == NULL || text == NULL || length >= sizeof(draft)) return ESP_ERR_INVALID_ARG;
    memcpy(draft, text, length);
    draft[length] = '\0';
    if (!esp_openclaw_chat_text_valid(draft, length)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    const esp_openclaw_reader_conversations_t *view = reader.conversations;
    if (!current_reader() || reader.messaging.state != ESP_OPENCLAW_READER_MESSAGING_READY || reply_active() ||
        reader.creation.state == ESP_OPENCLAW_READER_CREATION_CREATING ||
        view == NULL || view->state != ESP_OPENCLAW_READER_READY || view->selected >= view->page.count ||
        preview_revision != reader.preview_revision || view->preview_state != ESP_OPENCLAW_READER_READY ||
        (view->preview.status != ESP_OPENCLAW_PREVIEW_OK && view->preview.status != ESP_OPENCLAW_PREVIEW_EMPTY)) {
        xSemaphoreGive(reader.lock);
        return ESP_ERR_INVALID_STATE;
    }
    release_chat();
    unsigned revision = reader.reply.revision + 1;
    reader.reply = (esp_openclaw_reader_reply_snapshot_t){.state = ESP_OPENCLAW_READER_REPLY_PREPARING, .revision = revision,
        .preview_revision = preview_revision, .agent = view->agent};
    strcpy(reader.reply.title, view->page.sessions[view->selected].title);
    memcpy(reader.reply.text, draft, length + 1);
    strcpy(reader.reply.detail, "Checking this conversation.");
    strcpy(reader.reply_key, view->page.sessions[view->selected].key);
    reader.reply_generation = reader.generation;
    reader.reply_connection = reader.reader_id;
    reader.reply_submitted = false;
    reader.reply_prepare = true;
    xSemaphoreGive(reader.lock);
    wake_reader();
    return ESP_OK;
}

esp_err_t esp_openclaw_reader_reply_send(unsigned revision)
{
    if (reader.lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    if (revision != reader.reply.revision || reader.reply.state != ESP_OPENCLAW_READER_REPLY_REVIEW ||
        reader.chat == NULL || !reply_sendable()) {
        xSemaphoreGive(reader.lock);
        return ESP_ERR_INVALID_STATE;
    }
    reader.reply_send = true;
    reply_state(ESP_OPENCLAW_READER_REPLY_SENDING, "Waiting for the Gateway.");
    xSemaphoreGive(reader.lock);
    wake_reader();
    return ESP_OK;
}

void esp_openclaw_reader_reply_close(unsigned revision)
{
    if (reader.lock == NULL) return;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    if (revision == reader.reply.revision && !reader.reply_submitted && !reader.reply_send) {
        release_chat();
        reader.reply = (esp_openclaw_reader_reply_snapshot_t){.revision = revision + 1};
    }
    xSemaphoreGive(reader.lock);
}

void esp_openclaw_reader_reply_copy(esp_openclaw_reader_reply_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    if (reader.lock == NULL) return;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    *snapshot = reader.reply;
    snapshot->can_send = snapshot->state == ESP_OPENCLAW_READER_REPLY_REVIEW && reply_sendable();
    snapshot->can_read = snapshot->state != ESP_OPENCLAW_READER_REPLY_IDLE && reply_target();
    xSemaphoreGive(reader.lock);
}

esp_err_t esp_openclaw_reader_reply_read(unsigned revision)
{
    if (reader.lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    if (revision != reader.reply.revision || reader.reply.state == ESP_OPENCLAW_READER_REPLY_IDLE || !reply_target()) {
        xSemaphoreGive(reader.lock);
        return ESP_ERR_INVALID_STATE;
    }
    esp_openclaw_reader_conversations_t *view = reader.conversations;
    memset(&view->preview, 0, sizeof(view->preview));
    view->preview_revision = ++reader.preview_revision;
    view->preview_state = ESP_OPENCLAW_READER_LOADING;
    reader.preview_pending = true;
    xSemaphoreGive(reader.lock);
    wake_reader();
    return ESP_OK;
}

esp_err_t esp_openclaw_reader_devices_refresh(void)
{
    if (reader.lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    bool available = reader.devices_available;
    xSemaphoreGive(reader.lock);
    return available ? esp_openclaw_devices_refresh() : ESP_ERR_INVALID_STATE;
}

void esp_openclaw_reader_devices_close(void)
{
    if (reader.lock == NULL) return;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    bool available = reader.devices_available;
    xSemaphoreGive(reader.lock);
    if (available) esp_openclaw_devices_cancel();
}

void esp_openclaw_reader_devices_copy(esp_openclaw_devices_snapshot_t *snapshot)
{
    *snapshot = (esp_openclaw_devices_snapshot_t){.state = ESP_OPENCLAW_DEVICES_ERROR};
    if (reader.lock == NULL) return;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    bool available = reader.devices_available;
    unsigned generation = reader.generation;
    xSemaphoreGive(reader.lock);
    if (!available) return;
    esp_openclaw_devices_copy(snapshot);
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    /* SDK events revoke visible inventory before the worker handles reconnect. */
    if (!current_reader() || generation != reader.generation)
        *snapshot = (esp_openclaw_devices_snapshot_t){.state = ESP_OPENCLAW_DEVICES_OFFLINE};
    xSemaphoreGive(reader.lock);
}

esp_err_t esp_openclaw_reader_identify_begin(const char *node_id, unsigned inventory_revision)
{
    if (reader.lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    bool available = reader.identify_available;
    xSemaphoreGive(reader.lock);
    return available ? esp_openclaw_identify_begin(node_id, inventory_revision) : ESP_ERR_INVALID_STATE;
}

esp_err_t esp_openclaw_reader_identify_send(unsigned revision)
{
    if (reader.lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    bool available = reader.identify_available;
    xSemaphoreGive(reader.lock);
    return available ? esp_openclaw_identify_send(revision) : ESP_ERR_INVALID_STATE;
}

void esp_openclaw_reader_identify_copy(esp_openclaw_identify_snapshot_t *snapshot)
{
    *snapshot = (esp_openclaw_identify_snapshot_t){0};
    if (reader.lock == NULL) return;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    bool available = reader.identify_available;
    xSemaphoreGive(reader.lock);
    if (available) esp_openclaw_identify_copy(snapshot);
}

esp_err_t esp_openclaw_reader_agents_refresh(size_t offset)
{
    if (reader.lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    clear_conversations();
    reader.directory_enabled = reader.directory_pending = true;
    reader.directory_offset = offset;
    ++reader.directory_revision;
    reader.directory_state = ESP_OPENCLAW_READER_LOADING;
    free(reader.directory_page);
    reader.directory_page = NULL;
    xSemaphoreGive(reader.lock);
    xTaskNotifyGive(reader.task);
    return ESP_OK;
}

void esp_openclaw_reader_agents_copy(esp_openclaw_reader_agents_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = ESP_OPENCLAW_READER_ERROR;
    if (reader.lock == NULL) return;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    if (current_reader()) {
        snapshot->state = reader.directory_state;
        snapshot->revision = reader.directory_revision;
        if (reader.directory_page != NULL) snapshot->page = *reader.directory_page;
    } else {
        snapshot->state = !current_node() ? ESP_OPENCLAW_READER_OFFLINE :
            reader.status == ACCESS_NEEDED ? ESP_OPENCLAW_READER_ACCESS_NEEDED :
            reader.status == ERROR ? ESP_OPENCLAW_READER_ERROR : ESP_OPENCLAW_READER_CONNECTING;
    }
    xSemaphoreGive(reader.lock);
}

static void refresh_conversations(size_t offset)
{
    esp_openclaw_reader_conversations_t *view = reader.conversations;
    view->revision = ++reader.conversations_revision;
    view->preview_revision = ++reader.preview_revision;
    memset(&view->page, 0, sizeof(view->page));
    memset(&view->preview, 0, sizeof(view->preview));
    view->page.offset = offset;
    view->selected = 0;
    view->state = ESP_OPENCLAW_READER_LOADING;
    view->preview_state = ESP_OPENCLAW_READER_IDLE;
    reader.sessions_pending = true;
    reader.preview_pending = false;
}

esp_err_t esp_openclaw_reader_conversations_open(const char *agent_id, unsigned revision)
{
    if (reader.lock == NULL || agent_id == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    const esp_openclaw_agent_t *agent = NULL;
    if (current_reader() && revision == reader.directory_revision && reader.directory_page != NULL &&
        reader.directory_state == ESP_OPENCLAW_READER_READY) {
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

esp_err_t esp_openclaw_reader_conversations_refresh(size_t offset)
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

esp_err_t esp_openclaw_reader_preview_open(size_t index, unsigned revision)
{
    if (reader.lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    esp_openclaw_reader_conversations_t *view = reader.conversations;
    if (!current_reader() || view == NULL || view->state != ESP_OPENCLAW_READER_READY ||
        revision != reader.conversations_revision || index >= view->page.count) {
        xSemaphoreGive(reader.lock);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&view->preview, 0, sizeof(view->preview));
    view->selected = index;
    view->preview_revision = ++reader.preview_revision;
    view->preview_state = ESP_OPENCLAW_READER_LOADING;
    reader.preview_pending = true;
    xSemaphoreGive(reader.lock);
    xTaskNotifyGive(reader.task);
    return ESP_OK;
}

void esp_openclaw_reader_conversations_close(void)
{
    if (reader.lock == NULL) return;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    clear_conversations();
    xSemaphoreGive(reader.lock);
}

void esp_openclaw_reader_conversations_copy(esp_openclaw_reader_conversations_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = snapshot->preview_state = ESP_OPENCLAW_READER_ERROR;
    if (reader.lock == NULL) return;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    if (current_reader()) {
        if (reader.conversations != NULL) *snapshot = *reader.conversations;
        else snapshot->state = snapshot->preview_state = ESP_OPENCLAW_READER_IDLE;
    } else {
        snapshot->state = snapshot->preview_state = !current_node() ? ESP_OPENCLAW_READER_OFFLINE :
            reader.status == ACCESS_NEEDED ? ESP_OPENCLAW_READER_ACCESS_NEEDED :
            reader.status == ERROR ? ESP_OPENCLAW_READER_ERROR : ESP_OPENCLAW_READER_CONNECTING;
    }
    snapshot->page_size = reader.session_page_size;
    xSemaphoreGive(reader.lock);
}

void esp_openclaw_reader_home_copy(esp_openclaw_reader_home_t *context)
{
    memset(context, 0, sizeof(*context));
    if (reader.lock == NULL) return;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    const esp_openclaw_reader_conversations_t *view = reader.conversations;
    if (current_reader() && reader.directory_state == ESP_OPENCLAW_READER_READY &&
        reader.directory_page != NULL && view != NULL) {
        for (size_t i = 0; i < reader.directory_page->count; ++i) {
            if (strcmp(reader.directory_page->agents[i].id, view->agent.id) != 0) continue;
            context->available = true;
            context->directory_revision = reader.directory_revision;
            context->conversation_revision = view->revision;
            context->preview_revision = view->preview_revision;
            context->preview = view->state == ESP_OPENCLAW_READER_READY && view->selected < view->page.count &&
                view->preview_state != ESP_OPENCLAW_READER_IDLE;
            context->agent = view->agent;
            break;
        }
    }
    xSemaphoreGive(reader.lock);
}

esp_err_t esp_openclaw_reader_creation_begin(unsigned conversation_revision)
{
    if (reader.lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    const esp_openclaw_reader_conversations_t *view = reader.conversations;
    if (!current_reader() || view == NULL || conversation_revision != view->revision ||
        (view->state != ESP_OPENCLAW_READER_READY && view->state != ESP_OPENCLAW_READER_EMPTY) ||
        reply_active() || reader.creation.state == ESP_OPENCLAW_READER_CREATION_CREATING ||
        (reader.inflight != NULL && reader.inflight->kind == CREATE)) {
        xSemaphoreGive(reader.lock);
        return ESP_ERR_INVALID_STATE;
    }
    reader.creation = (esp_openclaw_reader_creation_snapshot_t){.state = ESP_OPENCLAW_READER_CREATION_REVIEW,
        .revision = reader.creation.revision + 1, .conversation_revision = view->revision,
        .agent = view->agent};
    strcpy(reader.creation.detail, "Creates an empty chat. No message is sent.");
    reader.creation_generation = reader.generation;
    reader.creation_connection = reader.creation_review_connection = reader.reader_id;
    reader.creation_pending = reader.creation_submitted = false;
    memset(&reader.created, 0, sizeof(reader.created));
    xSemaphoreGive(reader.lock);
    return ESP_OK;
}

esp_err_t esp_openclaw_reader_creation_submit(unsigned revision)
{
    if (reader.lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    if (revision != reader.creation.revision || reader.creation.state != ESP_OPENCLAW_READER_CREATION_REVIEW ||
        !creation_target() || reader.messaging.state != ESP_OPENCLAW_READER_MESSAGING_READY || reply_active()) {
        xSemaphoreGive(reader.lock);
        return ESP_ERR_INVALID_STATE;
    }
    creation_state(ESP_OPENCLAW_READER_CREATION_CREATING, "Creating an empty conversation.");
    reader.creation_pending = true;
    xSemaphoreGive(reader.lock);
    wake_reader();
    return ESP_OK;
}

void esp_openclaw_reader_creation_copy(esp_openclaw_reader_creation_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    if (reader.lock == NULL) return;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    *snapshot = reader.creation;
    snapshot->can_create = snapshot->state == ESP_OPENCLAW_READER_CREATION_REVIEW && creation_target() &&
        reader.messaging.state == ESP_OPENCLAW_READER_MESSAGING_READY && !reply_active();
    snapshot->can_open = snapshot->state == ESP_OPENCLAW_READER_CREATION_CREATED && creation_owner();
    if (snapshot->state == ESP_OPENCLAW_READER_CREATION_REVIEW && !creation_target())
        strcpy(snapshot->detail, "Refresh Chats and choose New chat again.");
    else if (snapshot->state == ESP_OPENCLAW_READER_CREATION_REVIEW && reader.messaging.state != ESP_OPENCLAW_READER_MESSAGING_READY)
        strcpy(snapshot->detail, "Enable messaging, then review and create. No message is sent.");
    xSemaphoreGive(reader.lock);
}

void esp_openclaw_reader_creation_close(unsigned revision)
{
    if (reader.lock == NULL) return;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    if (revision == reader.creation.revision &&
        (reader.creation.state == ESP_OPENCLAW_READER_CREATION_REVIEW || reader.creation.state == ESP_OPENCLAW_READER_CREATION_FAILED))
        reader.creation = (esp_openclaw_reader_creation_snapshot_t){.revision = revision + 1};
    xSemaphoreGive(reader.lock);
}

esp_err_t esp_openclaw_reader_creation_open(unsigned revision)
{
    if (reader.lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(reader.lock, portMAX_DELAY);
    if (revision != reader.creation.revision || reader.creation.state != ESP_OPENCLAW_READER_CREATION_CREATED ||
        !creation_owner()) {
        xSemaphoreGive(reader.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (reader.conversations == NULL) reader.conversations = calloc(1, sizeof(*reader.conversations));
    if (reader.conversations == NULL) { xSemaphoreGive(reader.lock); return ESP_ERR_NO_MEM; }
    esp_openclaw_reader_conversations_t *view = reader.conversations;
    memset(view, 0, sizeof(*view));
    view->state = ESP_OPENCLAW_READER_READY;
    view->preview_state = ESP_OPENCLAW_READER_LOADING;
    view->revision = ++reader.conversations_revision;
    view->preview_revision = ++reader.preview_revision;
    view->agent = reader.creation.agent;
    view->page.count = 1;
    strcpy(view->page.sessions[0].key, reader.created.key);
    strcpy(view->page.sessions[0].title, "New conversation");
    reader.sessions_pending = false;
    reader.preview_pending = true;
    xSemaphoreGive(reader.lock);
    wake_reader();
    return ESP_OK;
}
