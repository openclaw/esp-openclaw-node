#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_openclaw_reader.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"

struct esp_openclaw_node { uint32_t id; const char *uri; };
static struct esp_openclaw_node node = {0, "ws://gateway-a.example"};
static struct esp_openclaw_node client = {0, "ws://gateway-a.example"};
static esp_openclaw_node_event_cb_t event_cb;
static esp_openclaw_node_gateway_event_cb_t gateway_event_cb;
static esp_openclaw_node_gateway_request_cb_t chat_response;
static void *chat_context;
static unsigned random_sequence;
void esp_fill_random(void *buffer, size_t length) { memset(buffer, ++random_sequence, length); }
static void (*worker)(void *);
static jmp_buf idle;
static int waits, locked, creates, connects, disconnects, destroys, scopes, delays;
static esp_err_t create_result = ESP_OK, connect_result = ESP_ERR_NOT_FOUND, disconnect_result = ESP_OK;
static bool disconnect_during_delay, in_callback;
static esp_openclaw_node_handle_t fail_uri;
static int64_t now;
static int queries;
static char last_method[32], last_params[1024];
static esp_err_t query_result = ESP_OK;
static esp_openclaw_node_gateway_request_cb_t response;
static void *response_context;
static esp_openclaw_node_gateway_request_cb_t devices_response;
static void *devices_context;
static esp_openclaw_node_gateway_request_cb_t approval_response;
static void *approval_context;
static int write_scopes;
static bool registered_write;
static esp_openclaw_node_connect_source_t last_source;
static struct fake_timer { void (*callback)(void *); void *arg; int64_t deadline; } timer;
int64_t esp_timer_get_time(void) { return now; }
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out) {
    assert(timer.callback == NULL); timer.callback = args->callback; timer.arg = args->arg;
    *out = &timer; return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t handle, uint64_t delay) {
    assert(handle == &timer); timer.deadline = now + delay; return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t handle) {
    assert(handle == &timer); timer.deadline = 0; return ESP_OK;
}
esp_err_t esp_openclaw_node_gateway_request_on_connection(esp_openclaw_node_handle_t handle,
    uint32_t connection, const char *method, const char *params,
    esp_openclaw_node_gateway_request_cb_t callback, void *context) {
    bool chat = strcmp(method, "chat.history") == 0 || strcmp(method, "chat.send") == 0;
    assert((chat || strcmp(method, "sessions.create") == 0 || !locked) && !in_callback && handle == &client && connection == client.id);
    assert(strlen(method) < sizeof(last_method) && strlen(params) < sizeof(last_params));
    strcpy(last_method, method);
    strcpy(last_params, params);
    assert(strcmp(method, "agents.list") != 0 || strcmp(params, "{}") == 0);
    ++queries;
    if (query_result == ESP_OK) {
        if (chat) {
            assert(chat_response == NULL); chat_response = callback; chat_context = context;
        } else if (strcmp(method, "node.list") == 0) {
            assert(devices_response == NULL && strcmp(params, "{}") == 0);
            devices_response = callback; devices_context = context;
        } else if (strncmp(method, "device.scopes.", 14) == 0) {
            assert(approval_response == NULL); approval_response = callback; approval_context = context;
        } else { assert(response == NULL); response = callback; response_context = context; }
    }
    return query_result;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    static bool reader_created;
    if (!reader_created) { reader_created = true; return &locked; }
    return calloc(1, sizeof(int));
}
void xSemaphoreTake(SemaphoreHandle_t sem, uint32_t ticks) {
    assert(sem != NULL && ticks == portMAX_DELAY && !*(int *)sem && !in_callback);
    *(int *)sem = 1;
}
void xSemaphoreGive(SemaphoreHandle_t sem) { assert(sem != NULL && *(int *)sem); *(int *)sem = 0; }
void vSemaphoreDelete(SemaphoreHandle_t sem) { assert(sem != NULL && !*(int *)sem); if (sem != &locked) free(sem); }
int xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, int priority, TaskHandle_t *out) {
    (void)name; (void)arg; (void)priority; assert(stack >= 4096);
    worker = fn; *out = &worker; return pdPASS;
}
uint32_t ulTaskNotifyTake(int clear, uint32_t ticks) {
    assert(clear == pdTRUE && ticks == 1000 && !locked);
    if (waits++) longjmp(idle, 1);
    return 1;
}
void xTaskNotifyGive(TaskHandle_t task) { assert(task == &worker); }
void vTaskDelay(uint32_t ticks) {
    assert(ticks == 1000 && !locked); ++delays;
    if (disconnect_during_delay) {
        disconnect_during_delay = false;
        node.id = 0;
        esp_openclaw_reader_node_event(ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    }
}
void esp_openclaw_node_config_init_default(esp_openclaw_node_config_t *config) { memset(config, 0, sizeof(*config)); }
esp_err_t esp_openclaw_node_create(const esp_openclaw_node_config_t *config, esp_openclaw_node_handle_t *out) {
    assert(!locked && strcmp(config->role, "operator") == 0 && config->event_cb != NULL);
    assert(strcmp(config->display_name, "Test Reader") == 0);
    assert(strcmp(config->model_identifier, "test-board") == 0);
    assert(strcmp(config->tls_common_name, "gateway.example") == 0);
    ++creates;
    if (create_result != ESP_OK) { esp_err_t err = create_result; create_result = ESP_OK; return err; }
    event_cb = config->event_cb; gateway_event_cb = config->gateway_event_cb; *out = &client; return ESP_OK;
}
esp_err_t esp_openclaw_node_register_scope(esp_openclaw_node_handle_t handle, const char *scope) {
    assert(handle == &client && client.id == 0);
    if (strcmp(scope, "operator.write") == 0) { ++write_scopes; registered_write = true; }
    else { assert(strcmp(scope, "operator.read") == 0); ++scopes; }
    return ESP_OK;
}
esp_err_t esp_openclaw_node_destroy(esp_openclaw_node_handle_t handle) {
    assert(handle == &client); registered_write = false; ++destroys; return ESP_OK;
}
esp_err_t esp_openclaw_node_request_connect(esp_openclaw_node_handle_t handle, const esp_openclaw_node_connect_request_t *request) {
    assert(!locked && handle == &client);
    last_source = request->source;
    if (request->source == ESP_OPENCLAW_NODE_CONNECT_SOURCE_DEVICE_TOKEN) {
        assert(registered_write && strcmp(request->gateway_uri, node.uri) == 0);
        assert(strcmp(request->value, "test-approved-token") == 0);
    } else {
        assert(request->source == ESP_OPENCLAW_NODE_CONNECT_SOURCE_SAVED_SESSION);
        assert(request->gateway_uri == NULL && request->value == NULL);
    }
    ++connects; return connect_result;
}
esp_err_t esp_openclaw_node_request_disconnect(esp_openclaw_node_handle_t handle) {
    assert(!locked && handle == &client); ++disconnects; return disconnect_result;
}
uint32_t esp_openclaw_node_get_connection_id(esp_openclaw_node_handle_t handle) { assert(!in_callback); return handle != NULL ? handle->id : 0; }
char *esp_openclaw_node_dup_gateway_uri(esp_openclaw_node_handle_t handle) { assert(!in_callback); if (handle == fail_uri) { fail_uri = NULL; return NULL; } return strdup(handle->uri); }

static void step(void) { waits = 0; if (!setjmp(idle)) worker(NULL); assert(!locked); }
static void status(const char *expected, bool ready) {
    cJSON *result = esp_openclaw_reader_status();
    assert(result != NULL);
    assert(strcmp(cJSON_GetObjectItem(result, "state")->valuestring, expected) == 0);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(result, "readConnected")) == ready);
    cJSON_Delete(result);
}
static void agent_status(esp_openclaw_reader_state_t expected, size_t count) {
    esp_openclaw_reader_agents_t snapshot;
    esp_openclaw_reader_agents_copy(&snapshot);
    assert(snapshot.state == expected && snapshot.page.count == count);
}
static void respond(const char *json) {
    assert(response != NULL);
    esp_openclaw_node_gateway_result_t result = {.ok = json != NULL, .payload_json = json};
    locked = 1; in_callback = true;
    response(&client, &result, response_context);
    in_callback = false; locked = 0; response = NULL; response_context = NULL;
}
static void respond_devices(const char *json) {
    assert(devices_response != NULL);
    esp_openclaw_node_gateway_result_t result = {.ok = json != NULL, .payload_json = json};
    locked = 1; in_callback = true;
    devices_response(&client, &result, devices_context);
    in_callback = false; locked = 0; devices_response = NULL; devices_context = NULL;
}
static void approve_response(const char *json) {
    assert(approval_response != NULL);
    esp_openclaw_node_gateway_result_t result = {.ok = json != NULL, .payload_json = json};
    locked = 1; in_callback = true;
    approval_response(&client, &result, approval_context);
    in_callback = false; locked = 0; approval_response = NULL; approval_context = NULL;
}
static esp_openclaw_reader_messaging_snapshot_t messaging(esp_openclaw_reader_messaging_state_t expected) {
    esp_openclaw_reader_messaging_snapshot_t snapshot;
    esp_openclaw_reader_messaging_copy(&snapshot);
    assert(snapshot.state == expected);
    return snapshot;
}
static void request_messaging(void) {
    esp_openclaw_reader_messaging_snapshot_t snapshot;
    esp_openclaw_reader_messaging_copy(&snapshot);
    assert(esp_openclaw_reader_messaging_request(snapshot.revision) == ESP_OK);
    messaging(ESP_OPENCLAW_READER_MESSAGING_REQUESTING); step();
    assert(strcmp(last_method, "device.scopes.requestUpgrade") == 0);
    assert(strcmp(last_params, "{\"scopes\":[\"operator.read\",\"operator.write\"]}") == 0);
    approve_response("{\"requestId\":\"approval-one\"}"); step();
    snapshot = messaging(ESP_OPENCLAW_READER_MESSAGING_WAITING);
    assert(strcmp(snapshot.request_id, "approval-one") == 0);
    assert(strcmp(last_method, "device.scopes.waitUpgrade") == 0);
    assert(strcmp(last_params, "{\"requestId\":\"approval-one\"}") == 0);
}
static void chat_reply(const char *json) {
    assert(chat_response != NULL);
    esp_openclaw_node_gateway_result_t result = {.ok = json != NULL, .payload_json = json};
    esp_openclaw_node_gateway_request_cb_t callback = chat_response;
    void *context = chat_context;
    chat_response = NULL; chat_context = NULL;
    callback(&client, &result, context);
}
static esp_openclaw_reader_reply_snapshot_t reply_state(esp_openclaw_reader_reply_state_t expected) {
    esp_openclaw_reader_reply_snapshot_t reply;
    esp_openclaw_reader_reply_copy(&reply);
    assert(reply.state == expected);
    return reply;
}
static unsigned reply_preview(void) {
    assert(esp_openclaw_reader_agents_refresh(0) == ESP_OK); step();
    respond("{\"agents\":[{\"id\":\"kitsune\",\"identity\":{\"name\":\"Kitsune\",\"emoji\":\"🦊\"}}]}"); step();
    esp_openclaw_reader_agents_t agents; esp_openclaw_reader_agents_copy(&agents);
    assert(esp_openclaw_reader_conversations_open("kitsune", agents.revision) == ESP_OK); step();
    respond("{\"sessions\":[{\"key\":\"agent:kitsune:main\",\"label\":\"Weekend plans\"}]}"); step();
    esp_openclaw_reader_conversations_t *view = malloc(sizeof(*view)); assert(view != NULL);
    esp_openclaw_reader_conversations_copy(view);
    assert(esp_openclaw_reader_preview_open(0, view->revision) == ESP_OK); step();
    respond("{\"previews\":[{\"key\":\"agent:kitsune:main\",\"status\":\"empty\",\"items\":[]}]}"); step();
    esp_openclaw_reader_conversations_copy(view);
    unsigned revision = view->preview_revision;
    free(view); return revision;
}
static esp_openclaw_reader_reply_snapshot_t prepare_reply(unsigned revision) {
    assert(esp_openclaw_reader_reply_begin(ESP_OPENCLAW_READER_REPLY_CONTINUE, revision) == ESP_OK); step();
    reply_state(ESP_OPENCLAW_READER_REPLY_PREPARING);
    assert(strcmp(last_method, "chat.history") == 0);
    chat_reply("{\"sessionId\":\"session-one\",\"sessionInfo\":{\"key\":\"agent:kitsune:main\",\"agentId\":\"kitsune\",\"sessionId\":\"session-one\",\"activeLeafEntryId\":\"leaf-one\"}}"); step();
    esp_openclaw_reader_reply_snapshot_t reply = reply_state(ESP_OPENCLAW_READER_REPLY_REVIEW);
    assert(reply.can_send && reply.can_read && strcmp(reply.text, "Please continue.") == 0);
    return reply;
}
static esp_openclaw_reader_creation_snapshot_t creation_state(esp_openclaw_reader_creation_state_t expected) {
    esp_openclaw_reader_creation_snapshot_t snapshot;
    esp_openclaw_reader_creation_copy(&snapshot);
    assert(snapshot.state == expected);
    return snapshot;
}
static unsigned empty_chats(void) {
    assert(esp_openclaw_reader_agents_refresh(0) == ESP_OK); step();
    respond("{\"agents\":[{\"id\":\"kitsune\",\"name\":\"Kitsune\"}]}"); step();
    esp_openclaw_reader_agents_t agents; esp_openclaw_reader_agents_copy(&agents);
    assert(esp_openclaw_reader_conversations_open("kitsune", agents.revision) == ESP_OK); step();
    respond("{\"sessions\":[]}"); step();
    esp_openclaw_reader_conversations_t *view = malloc(sizeof(*view)); assert(view != NULL);
    esp_openclaw_reader_conversations_copy(view);
    assert(view->state == ESP_OPENCLAW_READER_EMPTY);
    unsigned revision = view->revision; free(view); return revision;
}
static void node_connected(uint32_t id) {
    node.id = id; locked = 1; in_callback = true;
    esp_openclaw_reader_node_event(ESP_OPENCLAW_NODE_EVENT_CONNECTED);
    in_callback = false; locked = 0;
}
static void connected(uint32_t id) {
    client.id = id; locked = 1; in_callback = true;
    event_cb(&client, ESP_OPENCLAW_NODE_EVENT_CONNECTED, NULL, NULL);
    in_callback = false; locked = 0;
}
static void disconnected(esp_openclaw_node_disconnected_reason_t reason) {
    client.id = 0;
    esp_openclaw_node_disconnected_event_t event = {.reason = reason};
    locked = 1; in_callback = true;
    event_cb(&client, ESP_OPENCLAW_NODE_EVENT_DISCONNECTED, &event, NULL);
    in_callback = false; locked = 0;
}

int main(void) {
    status("unavailable", false);
    esp_openclaw_devices_snapshot_t devices;
    esp_openclaw_reader_devices_copy(&devices);
    assert(devices.state == ESP_OPENCLAW_DEVICES_ERROR);
    assert(esp_openclaw_reader_devices_refresh() == ESP_ERR_INVALID_STATE);
    assert(esp_openclaw_reader_start(NULL, NULL) == ESP_ERR_INVALID_ARG);
    char long_name[129]; memset(long_name, 'a', sizeof(long_name) - 1); long_name[128] = '\0';
    esp_openclaw_reader_config_t config = {.display_name = long_name};
    assert(esp_openclaw_reader_start(&node, &config) == ESP_ERR_INVALID_ARG);
    char name[] = "Test Reader", model[] = "test-board", tls[] = "gateway.example";
    config = (esp_openclaw_reader_config_t){.display_name = name, .model_identifier = model, .tls_server_name = tls};
    assert(esp_openclaw_reader_start(&node, &config) == ESP_OK);
    memset(name, 'x', sizeof(name) - 1); memset(model, 'x', sizeof(model) - 1); memset(tls, 'x', sizeof(tls) - 1);
    assert(esp_openclaw_reader_start(&node, &config) == ESP_ERR_INVALID_STATE);
    step(); status("offline", false); assert(creates == 0);
    assert(esp_openclaw_reader_devices_refresh() == ESP_OK); step();
    esp_openclaw_reader_devices_copy(&devices);
    assert(devices.state == ESP_OPENCLAW_DEVICES_OFFLINE && queries == 0);
    esp_openclaw_reader_devices_close();

    node_connected(11); create_result = ESP_ERR_NO_MEM;
    step(); status("error", false); assert(connects == 0);
    step(); status("access-needed", false); assert(creates == 2 && scopes == 1 && connects == 1);
    step(); assert(connects == 1); // Missing handoff does not become an auth retry loop.

    node_connected(12); connect_result = ESP_OK;
    step(); status("connecting", false); connected(71); step(); status("ready", true);
    assert(creates == 2 && scopes == 1 && destroys == 0); // Reuse the saved-session handle.
    node.id = 0; status("offline", false); // Readiness cannot outlive the primary transport.
    esp_openclaw_reader_node_event(ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    disconnect_result = ESP_ERR_INVALID_STATE;
    step(); assert(disconnects == 1);
    disconnect_result = ESP_OK;
    node_connected(13); step(); assert(disconnects == 2 && connects == 2);
    connected(72); status("connecting", false); // A late old-generation handshake is not current.
    step(); disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED);
    step(); connected(73); step(); status("ready", true);

    node.uri = "ws://gateway-b.example"; node_connected(14);
    step(); disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED);
    step(); connected(74); step(); status("access-needed", false); // Old saved Gateway cannot supply this directory.
    step(); disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED);
    int before = connects; step(); step(); assert(connects == before);

    node_connected(15); client.uri = node.uri;
    step(); connected(75); step(); status("ready", true);
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_CONNECTION_LOST);
    step(); status("connecting", false);
    esp_openclaw_node_connect_failed_event_t failure = {.reason = ESP_OPENCLAW_NODE_CONNECT_FAILURE_AUTH_REJECTED};
    event_cb(&client, ESP_OPENCLAW_NODE_EVENT_CONNECT_FAILED, &failure, NULL);
    step(); status("access-needed", false); before = connects; step(); step(); assert(connects == before);

    node_connected(16); disconnect_during_delay = true;
    step(); status("offline", false); assert(connects == before);
    node_connected(17); step(); connected(76); step(); status("ready", true);
    node_connected(18); fail_uri = &node;
    step(); status("error", false); // Allocation failure is not missing access.
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED);
    step(); connected(77); fail_uri = &client;
    step(); status("error", false);
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED);
    before = connects; step(); assert(connects == before + 1);
    connected(78); step(); status("ready", true);
    assert(delays >= connects && destroys == 0);
    agent_status(ESP_OPENCLAW_READER_IDLE, 0);
    assert(esp_openclaw_reader_agents_refresh(0) == ESP_OK); step();
    agent_status(ESP_OPENCLAW_READER_LOADING, 0); assert(queries == 1);
    char roster[] = "{\"agents\":[{\"id\":\"kitsune\",\"identity\":{\"name\":\"Kitsune\",\"emoji\":\"🦊\"}}]}";
    respond(roster); memset(roster, 'x', sizeof(roster) - 1); step();
    agent_status(ESP_OPENCLAW_READER_READY, 1);
    esp_openclaw_reader_agents_t snapshot; esp_openclaw_reader_agents_copy(&snapshot);
    assert(strcmp(snapshot.page.agents[0].name, "Kitsune") == 0);

    assert(esp_openclaw_reader_agents_refresh(1) == ESP_OK); step();
    assert(esp_openclaw_reader_agents_refresh(0) == ESP_OK);
    respond("{\"agents\":[{\"id\":\"old\"}]}"); step();
    agent_status(ESP_OPENCLAW_READER_LOADING, 0); assert(queries == 3);
    respond("{\"agents\":[]}"); step(); agent_status(ESP_OPENCLAW_READER_EMPTY, 0);

    assert(esp_openclaw_reader_agents_refresh(0) == ESP_OK); step();
    node.id = 0; esp_openclaw_reader_node_event(ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    agent_status(ESP_OPENCLAW_READER_OFFLINE, 0);
    node.uri = "ws://gateway-c.example"; node_connected(19); step();
    respond("{\"agents\":[{\"id\":\"old\"}]}");
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    client.uri = node.uri; connected(79); step(); agent_status(ESP_OPENCLAW_READER_LOADING, 0);
    respond("{\"agents\":[{\"id\":\"new\"}]}"); step();
    agent_status(ESP_OPENCLAW_READER_READY, 1); esp_openclaw_reader_agents_copy(&snapshot);
    assert(strcmp(snapshot.page.agents[0].id, "new") == 0);

    query_result = ESP_ERR_NO_MEM;
    assert(esp_openclaw_reader_agents_refresh(0) == ESP_OK); step(); step();
    agent_status(ESP_OPENCLAW_READER_ERROR, 0); query_result = ESP_OK;
    assert(esp_openclaw_reader_agents_refresh(0) == ESP_OK); step(); respond("invalid"); step();
    agent_status(ESP_OPENCLAW_READER_ERROR, 0);
    assert(esp_openclaw_reader_agents_refresh(0) == ESP_OK); step();
    char *oversized = malloc(ESP_OPENCLAW_DIRECTORY_MAX_JSON_BYTES + 2);
    memset(oversized, 'x', ESP_OPENCLAW_DIRECTORY_MAX_JSON_BYTES + 1);
    oversized[ESP_OPENCLAW_DIRECTORY_MAX_JSON_BYTES + 1] = '\0';
    respond(oversized); free(oversized); step(); agent_status(ESP_OPENCLAW_READER_TOO_LARGE, 0);

    assert(esp_openclaw_reader_agents_refresh(0) == ESP_OK); step(); now += 15000000;
    before = disconnects; step(); assert(disconnects == before + 1);
    respond(NULL); disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    connected(80); step(); agent_status(ESP_OPENCLAW_READER_ERROR, 0);
    before = queries; step(); assert(queries == before); // Explicit refresh after a timed-out request.
    assert(esp_openclaw_reader_agents_refresh(0) == ESP_OK); step();
    respond("{\"agents\":[]}"); step(); agent_status(ESP_OPENCLAW_READER_EMPTY, 0);
    /* Browse agent B without changing any node/control session state. */
    assert(esp_openclaw_reader_agents_refresh(0) == ESP_OK); step();
    respond("{\"agents\":[{\"id\":\"alpha\"},{\"id\":\"beta\",\"identity\":{\"emoji\":\"🦊\"}}]}"); step();
    esp_openclaw_reader_agents_copy(&snapshot);
    esp_openclaw_reader_home_t home;
    esp_openclaw_reader_home_copy(&home);
    assert(!home.available && home.agent.name[0] == '\0');
    assert(esp_openclaw_reader_conversations_open("beta", snapshot.revision - 1) == ESP_ERR_INVALID_STATE);
    assert(esp_openclaw_reader_conversations_open("missing", snapshot.revision) == ESP_ERR_INVALID_STATE);
    assert(esp_openclaw_reader_conversations_open("beta", snapshot.revision) == ESP_OK); step();
    assert(strcmp(last_method, "sessions.list") == 0);
    cJSON *params = cJSON_Parse(last_params);
    assert(strcmp(cJSON_GetObjectItem(params, "agentId")->valuestring, "beta") == 0);
    assert(cJSON_GetObjectItem(params, "limit")->valueint == 8);
    assert(cJSON_IsFalse(cJSON_GetObjectItem(params, "includeGlobal")));
    cJSON_Delete(params);
    respond("{\"sessions\":[{\"key\":\"agent:beta:thread:one\",\"label\":\"One\"},{\"key\":\"agent:beta:thread:two\",\"label\":\"Two\"}],\"offset\":0,\"hasMore\":true,\"nextOffset\":2}"); step();
    esp_openclaw_reader_conversations_t *view = malloc(sizeof(*view));
    assert(view != NULL);
    esp_openclaw_reader_conversations_copy(view);
    assert(view->state == ESP_OPENCLAW_READER_READY && view->page.count == 2);
    assert(strcmp(view->agent.id, "beta") == 0 && view->page.next_offset == 2);
    esp_openclaw_reader_home_copy(&home);
    assert(home.available && !home.preview && strcmp(home.agent.name, "beta") == 0);
    assert(strcmp(home.agent.id, "beta") == 0 && strcmp(home.agent.emoji, "🦊") == 0);
    assert(home.directory_revision == snapshot.revision && home.conversation_revision == view->revision);
    unsigned revision = view->revision;
    assert(esp_openclaw_reader_preview_open(2, revision) == ESP_ERR_INVALID_STATE);
    assert(esp_openclaw_reader_preview_open(0, revision - 1) == ESP_ERR_INVALID_STATE);
    assert(esp_openclaw_reader_preview_open(0, revision) == ESP_OK); step();
    assert(strcmp(last_method, "sessions.preview") == 0);
    params = cJSON_Parse(last_params);
    assert(strcmp(cJSON_GetArrayItem(cJSON_GetObjectItem(params, "keys"), 0)->valuestring, "agent:beta:thread:one") == 0);
    assert(cJSON_GetObjectItem(params, "limit")->valueint == 4);
    cJSON_Delete(params);
    assert(esp_openclaw_reader_preview_open(1, revision) == ESP_OK);
    respond("{\"previews\":[{\"key\":\"agent:beta:thread:one\",\"status\":\"ok\",\"items\":[{\"role\":\"assistant\",\"text\":\"stale\"}]}]}"); step();
    esp_openclaw_reader_conversations_copy(view);
    assert(view->selected == 1 && view->preview_state == ESP_OPENCLAW_READER_LOADING && view->preview.count == 0);
    esp_openclaw_reader_home_copy(&home);
    assert(home.available && home.preview && home.preview_revision == view->preview_revision);
    unsigned home_revision = home.preview_revision;
    respond("{\"previews\":[{\"key\":\"agent:beta:thread:two\",\"status\":\"missing\",\"items\":[]}]}"); step();
    esp_openclaw_reader_conversations_copy(view);
    assert(view->preview_state == ESP_OPENCLAW_READER_READY && view->preview.status == ESP_OPENCLAW_PREVIEW_MISSING && view->preview.count == 0);

    assert(esp_openclaw_reader_conversations_refresh(2) == ESP_OK); step();
    esp_openclaw_reader_home_copy(&home);
    assert(home.available && !home.preview && home.preview_revision != home_revision);
    params = cJSON_Parse(last_params);
    assert(cJSON_GetObjectItem(params, "offset")->valueint == 2);
    cJSON_Delete(params);
    assert(esp_openclaw_reader_conversations_open("alpha", snapshot.revision) == ESP_OK);
    respond("{\"sessions\":[{\"key\":\"agent:beta:old\"}],\"offset\":2}"); step();
    esp_openclaw_reader_conversations_copy(view);
    assert(strcmp(view->agent.id, "alpha") == 0 && view->state == ESP_OPENCLAW_READER_LOADING && view->page.count == 0);
    respond("{\"sessions\":[{\"key\":\"agent:alpha:main\"}]}"); step();
    esp_openclaw_reader_conversations_copy(view);
    assert(view->state == ESP_OPENCLAW_READER_READY && view->page.count == 1);
    assert(esp_openclaw_reader_preview_open(0, view->revision) == ESP_OK); step();
    esp_openclaw_reader_conversations_close();
    esp_openclaw_reader_home_copy(&home);
    assert(!home.available && home.agent.name[0] == '\0');
    respond("{\"previews\":[{\"key\":\"agent:alpha:main\",\"status\":\"empty\",\"items\":[]}]}"); step();
    assert(home.agent.id[0] == '\0' && home.agent.emoji[0] == '\0');
    esp_openclaw_reader_conversations_copy(view);
    assert(view->state == ESP_OPENCLAW_READER_IDLE && view->preview.count == 0);
    before = queries; step(); assert(queries == before);

    assert(esp_openclaw_reader_conversations_open("beta", snapshot.revision) == ESP_OK); step();
    node.id = 0; esp_openclaw_reader_node_event(ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    esp_openclaw_reader_home_copy(&home);
    assert(!home.available && home.agent.name[0] == '\0');
    esp_openclaw_reader_conversations_copy(view);
    assert(view->state == ESP_OPENCLAW_READER_OFFLINE && view->page.count == 0);
    respond("{\"sessions\":[{\"key\":\"agent:beta:stale\"}]}"); step();
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    node_connected(20); step(); connected(81); step();
    /* Reconnect refreshes agents, but never silently reopens the old agent. */
    respond("{\"agents\":[{\"id\":\"beta\"}]}"); step();
    esp_openclaw_reader_conversations_copy(view);
    assert(view->state == ESP_OPENCLAW_READER_IDLE && view->page.count == 0);
    esp_openclaw_reader_agents_copy(&snapshot);
    assert(esp_openclaw_reader_conversations_open("beta", snapshot.revision) == ESP_OK); step();
    respond(NULL); step();
    esp_openclaw_reader_conversations_copy(view);
    assert(view->state == ESP_OPENCLAW_READER_ERROR && view->page.count == 0);
    query_result = ESP_ERR_NO_MEM;
    assert(esp_openclaw_reader_conversations_refresh(0) == ESP_OK); step(); step();
    esp_openclaw_reader_conversations_copy(view);
    assert(view->state == ESP_OPENCLAW_READER_ERROR && view->page.count == 0);
    query_result = ESP_OK;
    assert(esp_openclaw_reader_conversations_refresh(0) == ESP_OK); step();
    assert(esp_openclaw_reader_agents_refresh(0) == ESP_OK);
    respond("{\"sessions\":[{\"key\":\"agent:beta:stale\"}]}"); step();
    esp_openclaw_reader_conversations_copy(view);
    assert(view->state == ESP_OPENCLAW_READER_IDLE && view->page.count == 0);
    respond("{\"agents\":[{\"id\":\"beta\"}]}"); step();
    free(view);
    before = scopes;
    assert(esp_openclaw_reader_devices_refresh() == ESP_OK); step();
    assert(strcmp(last_method, "node.list") == 0 && scopes == before);
    char nodes[] = "{\"nodes\":[{\"nodeId\":\"studio-dial\",\"displayName\":\"Studio Dial\",\"connected\":true}]}";
    respond_devices(nodes); memset(nodes, 'x', sizeof(nodes) - 1); step();
    esp_openclaw_reader_devices_copy(&devices);
    assert(devices.state == ESP_OPENCLAW_DEVICES_READY && devices.count == 1);
    assert(strcmp(devices.devices[0].name, "Studio Dial") == 0);
    assert(!devices.devices[0].paired_known);

    /* Agent browsing can finish while a canceled inventory callback drains. */
    assert(esp_openclaw_reader_devices_refresh() == ESP_OK); step();
    esp_openclaw_reader_devices_close();
    assert(esp_openclaw_reader_agents_refresh(0) == ESP_OK); step();
    respond("{\"agents\":[{\"id\":\"beta\"}]}"); step();
    respond_devices("{\"nodes\":[{\"nodeId\":\"stale\"}]}"); step();
    esp_openclaw_reader_devices_copy(&devices);
    assert(devices.state == ESP_OPENCLAW_DEVICES_IDLE && devices.count == 0);

    assert(esp_openclaw_reader_devices_refresh() == ESP_OK); step();
    esp_openclaw_reader_devices_close();
    assert(esp_openclaw_reader_devices_refresh() == ESP_OK);
    now += 15000000; timer.callback(timer.arg);
    before = disconnects; step();
    esp_openclaw_reader_devices_copy(&devices);
    assert(devices.state == ESP_OPENCLAW_DEVICES_TIMEOUT && disconnects == before);
    before = queries;
    respond_devices("{\"nodes\":[{\"nodeId\":\"late\"}]}"); step();
    assert(queries == before);
    esp_openclaw_reader_devices_copy(&devices); assert(devices.count == 0);

    assert(esp_openclaw_reader_devices_refresh() == ESP_OK); step();
    node.id = 0; esp_openclaw_reader_node_event(ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    esp_openclaw_reader_devices_copy(&devices);
    assert(devices.state == ESP_OPENCLAW_DEVICES_OFFLINE && devices.count == 0);
    step(); respond_devices("{\"nodes\":[{\"nodeId\":\"old-gateway\"}]}");
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    node_connected(21); step(); connected(82); step();
    respond("{\"agents\":[{\"id\":\"beta\"}]}"); step();
    respond_devices("{\"nodes\":[]}"); step();
    esp_openclaw_reader_devices_copy(&devices);
    assert(devices.state == ESP_OPENCLAW_DEVICES_EMPTY && devices.count == 0);
    esp_openclaw_reader_devices_close();
    /* Approval waits coexist with browsing and outlive normal read deadlines. */
    esp_openclaw_reader_messaging_snapshot_t access = messaging(ESP_OPENCLAW_READER_MESSAGING_OFF);
    assert(esp_openclaw_reader_messaging_request(access.revision + 1) == ESP_ERR_INVALID_STATE);
    request_messaging();
    access = messaging(ESP_OPENCLAW_READER_MESSAGING_WAITING);
    assert(esp_openclaw_reader_messaging_request(access.revision) == ESP_ERR_INVALID_STATE);
    before = disconnects;
    now += 16000000; step(); assert(disconnects == before);
    assert(esp_openclaw_reader_agents_refresh(0) == ESP_OK); step();
    respond("{\"agents\":[{\"id\":\"beta\"}]}"); step();
    agent_status(ESP_OPENCLAW_READER_READY, 1);
    approve_response("{\"requestId\":\"approval-one\",\"status\":\"rejected\"}"); step();
    messaging(ESP_OPENCLAW_READER_MESSAGING_REJECTED); status("ready", true); assert(write_scopes == 0);

    const char *invalid_approvals[] = {
        "{\"requestId\":\"other\",\"status\":\"approved\",\"deviceToken\":\"test-approved-token\",\"scopes\":[\"operator.read\",\"operator.write\"]}",
        "{\"requestId\":\"approval-one\",\"status\":\"approved\",\"deviceToken\":\"test-approved-token\",\"scopes\":[\"operator.read\"]}",
        "{\"requestId\":\"approval-one\",\"status\":\"approved\",\"deviceToken\":\"\",\"scopes\":[\"operator.read\",\"operator.write\"]}",
        "not JSON",
        "{\"requestId\":\"approval-one\",\"status\":\"approved\",\"deviceToken\":\"test-approved-token\",\"scopes\":[\"operator.read\",\"operator.write\"]} trailing",
        "{\"requestId\":\"approval-one\\u0000other\",\"status\":\"approved\",\"deviceToken\":\"test-approved-token\",\"scopes\":[\"operator.read\",\"operator.write\"]}",
        "{\"extra\":[[[[]]]]}",
    };
    for (size_t i = 0; i < sizeof(invalid_approvals) / sizeof(invalid_approvals[0]); ++i) {
        request_messaging(); approve_response(invalid_approvals[i]); step();
        messaging(ESP_OPENCLAW_READER_MESSAGING_ERROR); status("ready", true); assert(write_scopes == 0);
    }
    request_messaging();
    approve_response("{\"requestId\":\"approval-one\",\"status\":\"expired\"}"); step();
    messaging(ESP_OPENCLAW_READER_MESSAGING_EXPIRED); status("ready", true);

    const char *approved = "{\"requestId\":\"approval-one\",\"status\":\"approved\",\"deviceToken\":\"test-approved-token\",\"scopes\":[\"operator.read\",\"operator.write\"]}";
    request_messaging(); access = messaging(ESP_OPENCLAW_READER_MESSAGING_WAITING);
    assert(esp_openclaw_reader_messaging_cancel(access.revision - 1) == ESP_ERR_INVALID_STATE);
    assert(esp_openclaw_reader_messaging_cancel(access.revision) == ESP_OK);
    messaging(ESP_OPENCLAW_READER_MESSAGING_OFF); step();
    approve_response(approved); // Approval arriving after local cancel must never apply.
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    assert(write_scopes == 0); connected(83); step();
    respond("{\"agents\":[]}"); step(); status("ready", true);

    request_messaging(); approve_response(approved); step();
    access = messaging(ESP_OPENCLAW_READER_MESSAGING_CONNECTING);
    assert(write_scopes == 0); // Still on the original transport until it closes.
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    assert(write_scopes == 1 && last_source == ESP_OPENCLAW_NODE_CONNECT_SOURCE_DEVICE_TOKEN);
    messaging(ESP_OPENCLAW_READER_MESSAGING_CONNECTING); // Queue admission is not successful auth.
    connected(84); step(); messaging(ESP_OPENCLAW_READER_MESSAGING_READY); status("ready", true);
    respond("{\"agents\":[]}"); step();
    assert(esp_openclaw_reader_messaging_cancel(access.revision) == ESP_ERR_INVALID_STATE);

    client.id = 0; messaging(ESP_OPENCLAW_READER_MESSAGING_CONNECTING);
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_CONNECTION_LOST); step();
    messaging(ESP_OPENCLAW_READER_MESSAGING_CONNECTING);
    assert(last_source == ESP_OPENCLAW_NODE_CONNECT_SOURCE_SAVED_SESSION);
    connected(85); step(); messaging(ESP_OPENCLAW_READER_MESSAGING_READY);
    respond("{\"agents\":[]}"); step();

    // Primary reconnect invalidates write intent and reconstructs a read-only client.
    node_connected(22); messaging(ESP_OPENCLAW_READER_MESSAGING_OFF); step();
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    assert(!registered_write && destroys == 1);
    connected(86); step(); respond("{\"agents\":[]}"); step();
    messaging(ESP_OPENCLAW_READER_MESSAGING_OFF); status("ready", true);

    request_messaging(); now += 315000000; before = disconnects; step();
    messaging(ESP_OPENCLAW_READER_MESSAGING_EXPIRED); assert(disconnects == before + 1);
    approve_response(approved); disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    assert(write_scopes == 1); connected(87); step(); respond("{\"agents\":[]}"); step();
    messaging(ESP_OPENCLAW_READER_MESSAGING_EXPIRED); status("ready", true);

    request_messaging(); approve_response(approved); step();
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    access = messaging(ESP_OPENCLAW_READER_MESSAGING_CONNECTING);
    assert(esp_openclaw_reader_messaging_cancel(access.revision) == ESP_OK); step();
    // Even a late handshake cannot restore canceled authority.
    connected(88); step(); messaging(ESP_OPENCLAW_READER_MESSAGING_OFF);
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    assert(!registered_write && destroys == 2);
    connected(89); step(); respond("{\"agents\":[]}"); step();
    messaging(ESP_OPENCLAW_READER_MESSAGING_OFF); status("ready", true);

    request_messaging(); approve_response(approved); step();
    connect_result = ESP_ERR_NO_MEM;
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    messaging(ESP_OPENCLAW_READER_MESSAGING_CONNECTING);
    connect_result = ESP_OK; step();
    assert(last_source == ESP_OPENCLAW_NODE_CONNECT_SOURCE_DEVICE_TOKEN);
    connected(90); step(); respond("{\"agents\":[]}"); step();
    access = messaging(ESP_OPENCLAW_READER_MESSAGING_READY);
    assert(esp_openclaw_reader_messaging_cancel(access.revision) == ESP_OK);
    messaging(ESP_OPENCLAW_READER_MESSAGING_OFF); step();
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    assert(!registered_write && destroys == 3);
    connected(91); step(); respond("{\"agents\":[]}"); step();
    status("ready", true);

    request_messaging(); approve_response(approved); step();
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    failure.reason = ESP_OPENCLAW_NODE_CONNECT_FAILURE_AUTH_REJECTED;
    event_cb(&client, ESP_OPENCLAW_NODE_EVENT_CONNECT_FAILED, &failure, NULL); step();
    messaging(ESP_OPENCLAW_READER_MESSAGING_ERROR);
    assert(!registered_write && destroys == 4);
    assert(last_source == ESP_OPENCLAW_NODE_CONNECT_SOURCE_SAVED_SESSION);
    connected(92); step(); respond("{\"agents\":[]}"); step();
    status("ready", true);

    // The approved token is already rotated when its callback precedes a loss.
    request_messaging();
    approve_response(approved);
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_CONNECTION_LOST);
    step();
    messaging(ESP_OPENCLAW_READER_MESSAGING_CONNECTING);
    assert(last_source == ESP_OPENCLAW_NODE_CONNECT_SOURCE_DEVICE_TOKEN);
    connected(93); step(); respond("{\"agents\":[]}"); step();
    messaging(ESP_OPENCLAW_READER_MESSAGING_READY); status("ready", true);

    access = messaging(ESP_OPENCLAW_READER_MESSAGING_READY);
    assert(esp_openclaw_reader_messaging_cancel(access.revision) == ESP_OK); step();
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    connected(94); step(); respond("{\"agents\":[]}"); step();

    // A stalled read still reports its deadline without aborting a live grant.
    request_messaging();
    assert(esp_openclaw_reader_agents_refresh(0) == ESP_OK); step();
    before = disconnects; now += 15000000; step();
    agent_status(ESP_OPENCLAW_READER_ERROR, 0);
    messaging(ESP_OPENCLAW_READER_MESSAGING_WAITING); assert(disconnects == before);
    approve_response(approved); step();
    respond(NULL); disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    connected(95); step(); messaging(ESP_OPENCLAW_READER_MESSAGING_READY); status("ready", true);

    access = messaging(ESP_OPENCLAW_READER_MESSAGING_READY);
    assert(esp_openclaw_reader_messaging_cancel(access.revision) == ESP_OK); step();
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    connected(96); step();

    // Canceling a rejected connect queue restores a usable read-only session.
    request_messaging(); approve_response(approved); step();
    connect_result = ESP_ERR_NO_MEM;
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    access = messaging(ESP_OPENCLAW_READER_MESSAGING_CONNECTING);
    assert(esp_openclaw_reader_messaging_cancel(access.revision) == ESP_OK);
    connect_result = ESP_OK; step(); connected(97); step();
    messaging(ESP_OPENCLAW_READER_MESSAGING_OFF); status("ready", true);

    request_messaging();
    assert(esp_openclaw_reader_agents_refresh(0) == ESP_OK); step();
    now += 15000000; step();
    agent_status(ESP_OPENCLAW_READER_ERROR, 0); messaging(ESP_OPENCLAW_READER_MESSAGING_WAITING);
    before = disconnects;
    approve_response("{\"requestId\":\"approval-one\",\"status\":\"rejected\"}"); step();
    messaging(ESP_OPENCLAW_READER_MESSAGING_REJECTED); assert(disconnects == before + 1);
    respond(NULL); disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    connected(98); step(); status("ready", true);
    // A reply needs current write authority and an exact, still-visible preview.
    unsigned preview = reply_preview();
    assert(esp_openclaw_reader_reply_begin(ESP_OPENCLAW_READER_REPLY_CONTINUE, preview) == ESP_ERR_INVALID_STATE);
    request_messaging(); approve_response(approved); step();
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    connected(99); step(); respond("{\"agents\":[]}"); step();
    preview = reply_preview();
    assert(esp_openclaw_reader_reply_begin(ESP_OPENCLAW_READER_REPLY_CONTINUE, preview - 1) == ESP_ERR_INVALID_STATE);
    assert(esp_openclaw_reader_reply_begin(ESP_OPENCLAW_READER_REPLY_PRESET_COUNT, preview) == ESP_ERR_INVALID_ARG);

    // Closing a draft releases only its view; a late accepted history reply is safe.
    assert(esp_openclaw_reader_reply_begin(ESP_OPENCLAW_READER_REPLY_CONTINUE, preview) == ESP_OK); step();
    esp_openclaw_reader_reply_snapshot_t reply = reply_state(ESP_OPENCLAW_READER_REPLY_PREPARING);
    esp_openclaw_reader_reply_close(reply.revision); reply_state(ESP_OPENCLAW_READER_REPLY_IDLE);
    chat_reply(NULL); step(); reply_state(ESP_OPENCLAW_READER_REPLY_IDLE);

    reply = prepare_reply(preview);
    assert(strcmp(reply.agent.name, "Kitsune") == 0 && strcmp(reply.title, "Weekend plans") == 0);
    assert(esp_openclaw_reader_reply_send(reply.revision - 1) == ESP_ERR_INVALID_STATE);
    // A refreshed/changed preview revokes the reviewed draft before worker polling.
    assert(esp_openclaw_reader_reply_read(reply.revision) == ESP_OK);
    assert(esp_openclaw_reader_reply_send(reply.revision) == ESP_ERR_INVALID_STATE); step();
    reply_state(ESP_OPENCLAW_READER_REPLY_FAILED);
    respond("{\"previews\":[{\"key\":\"agent:kitsune:main\",\"status\":\"empty\",\"items\":[]}]}"); step();
    preview = reply_preview(); reply = prepare_reply(preview);
    query_result = ESP_ERR_NO_MEM;
    assert(esp_openclaw_reader_reply_send(reply.revision) == ESP_OK); step();
    query_result = ESP_OK; reply = reply_state(ESP_OPENCLAW_READER_REPLY_REVIEW); assert(reply.can_send);
    step(); reply = reply_state(ESP_OPENCLAW_READER_REPLY_REVIEW);
    assert(strcmp(reply.detail, "Could not queue. Try Send again.") == 0);
    assert(esp_openclaw_reader_reply_send(reply.revision) == ESP_OK);
    assert(esp_openclaw_reader_reply_send(reply.revision) == ESP_ERR_INVALID_STATE);
    reply = reply_state(ESP_OPENCLAW_READER_REPLY_SENDING);
    esp_openclaw_reader_reply_close(reply.revision); step(); // Back before worker admission retains Send.
    reply = reply_state(ESP_OPENCLAW_READER_REPLY_SENDING);
    assert(strcmp(last_method, "chat.send") == 0);
    params = cJSON_Parse(last_params); assert(params != NULL);
    assert(strcmp(cJSON_GetObjectItem(params, "agentId")->valuestring, "kitsune") == 0);
    assert(strcmp(cJSON_GetObjectItem(params, "sessionKey")->valuestring, "agent:kitsune:main") == 0);
    assert(strcmp(cJSON_GetObjectItem(params, "sessionId")->valuestring, "session-one") == 0);
    assert(strcmp(cJSON_GetObjectItem(params, "expectedLeafEntryId")->valuestring, "leaf-one") == 0);
    assert(strcmp(cJSON_GetObjectItem(params, "message")->valuestring, "Please continue.") == 0);
    char run[33]; strcpy(run, cJSON_GetObjectItem(params, "idempotencyKey")->valuestring);
    cJSON_Delete(params);
    assert(esp_openclaw_reader_reply_begin(ESP_OPENCLAW_READER_REPLY_SUMMARIZE, preview) == ESP_ERR_INVALID_STATE);
    char event[400]; snprintf(event, sizeof(event), "{\"runId\":\"%s\",\"status\":\"started\"}", run);
    chat_reply(event); step(); reply = reply_state(ESP_OPENCLAW_READER_REPLY_ACCEPTED);
    // Reading is independent; a stalled read must not sever a running reply.
    assert(esp_openclaw_reader_reply_read(reply.revision) == ESP_OK); step();
    before = disconnects; now += 15000000; step(); assert(disconnects == before);
    reply_state(ESP_OPENCLAW_READER_REPLY_ACCEPTED);
    snprintf(event, sizeof(event), "{\"runId\":\"%s\",\"sessionKey\":\"agent:kitsune:main\",\"agentId\":\"kitsune\",\"seq\":1,\"state\":\"final\"}", run);
    gateway_event_cb(&client, "chat", event, NULL); step();
    reply = reply_state(ESP_OPENCLAW_READER_REPLY_SENT); assert(disconnects == before + 1);
    respond(NULL); disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    reply_state(ESP_OPENCLAW_READER_REPLY_SENT); connected(100); step();

    // An admitted send with no conclusive response must never become retryable.
    preview = reply_preview(); reply = prepare_reply(preview);
    assert(esp_openclaw_reader_reply_send(reply.revision) == ESP_OK); step();
    before = queries;
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_CONNECTION_LOST); step();
    reply = reply_state(ESP_OPENCLAW_READER_REPLY_UNKNOWN); assert(!reply.can_send && !reply.can_read);
    esp_openclaw_reader_reply_close(reply.revision); reply_state(ESP_OPENCLAW_READER_REPLY_UNKNOWN);
    chat_reply(NULL); step(); assert(queries == before); // No automatic replay.
    connected(101); step(); respond("{\"agents\":[]}"); step();
    reply = reply_state(ESP_OPENCLAW_READER_REPLY_UNKNOWN);
    assert(esp_openclaw_reader_reply_read(reply.revision) == ESP_ERR_INVALID_STATE);
    preview = reply_preview(); reply = prepare_reply(preview);
    assert(esp_openclaw_reader_reply_send(reply.revision) == ESP_OK); step();
    params = cJSON_Parse(last_params); assert(params != NULL);
    strcpy(run, cJSON_GetObjectItem(params, "idempotencyKey")->valuestring); cJSON_Delete(params);
    snprintf(event, sizeof(event), "{\"runId\":\"%s\",\"status\":\"ok\"}", run);
    chat_reply(event); step(); reply = reply_state(ESP_OPENCLAW_READER_REPLY_SENT);
    assert(reply.can_read && !reply.can_send);
    assert(esp_openclaw_reader_reply_read(reply.revision) == ESP_OK); step();
    assert(strcmp(last_method, "sessions.preview") == 0);
    respond("{\"previews\":[{\"key\":\"agent:kitsune:main\",\"status\":\"ok\",\"items\":[{\"role\":\"assistant\",\"text\":\"Next step\"}]}]}"); step();
    reply = reply_state(ESP_OPENCLAW_READER_REPLY_SENT); assert(reply.preview_revision == preview);

    // A primary replacement between Send and worker admission cannot send on the old target.
    preview = reply_preview(); reply = prepare_reply(preview);
    assert(esp_openclaw_reader_reply_send(reply.revision) == ESP_OK);
    before = queries; node_connected(23); step();
    reply = reply_state(ESP_OPENCLAW_READER_REPLY_FAILED); assert(queries == before && chat_response == NULL);
    esp_openclaw_reader_reply_close(reply.revision); step(); reply_state(ESP_OPENCLAW_READER_REPLY_IDLE);
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    connected(102); step(); respond("{\"agents\":[]}"); step();
    unsigned chats = empty_chats();
    assert(esp_openclaw_reader_creation_begin(chats - 1) == ESP_ERR_INVALID_STATE);
    assert(esp_openclaw_reader_creation_begin(chats) == ESP_OK);
    esp_openclaw_reader_creation_snapshot_t creation = creation_state(ESP_OPENCLAW_READER_CREATION_REVIEW);
    assert(!creation.can_create && strcmp(creation.agent.id, "kitsune") == 0);
    assert(esp_openclaw_reader_creation_submit(creation.revision) == ESP_ERR_INVALID_STATE);
    // Write approval reconnects, then a fresh roster reattests the captured agent.
    request_messaging(); approve_response(approved); step();
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    connected(103); step();
    creation = creation_state(ESP_OPENCLAW_READER_CREATION_REVIEW); assert(!creation.can_create);
    respond("{\"agents\":[{\"id\":\"kitsune\",\"name\":\"Kitsune\"}]}"); step();
    creation = creation_state(ESP_OPENCLAW_READER_CREATION_REVIEW); assert(creation.can_create);
    assert(esp_openclaw_reader_creation_submit(creation.revision - 1) == ESP_ERR_INVALID_STATE);
    query_result = ESP_ERR_NO_MEM; before = queries;
    assert(esp_openclaw_reader_creation_submit(creation.revision) == ESP_OK); step(); step();
    assert(queries == before + 1 && strcmp(last_method, "sessions.create") == 0);
    query_result = ESP_OK;
    creation = creation_state(ESP_OPENCLAW_READER_CREATION_FAILED);
    assert(!creation.can_create && !creation.can_open);

    chats = empty_chats(); assert(esp_openclaw_reader_creation_begin(chats) == ESP_OK);
    creation = creation_state(ESP_OPENCLAW_READER_CREATION_REVIEW);
    assert(esp_openclaw_reader_creation_submit(creation.revision) == ESP_OK);
    assert(esp_openclaw_reader_creation_submit(creation.revision) == ESP_ERR_INVALID_STATE);
    creation = creation_state(ESP_OPENCLAW_READER_CREATION_CREATING);
    esp_openclaw_reader_creation_close(creation.revision); step();
    assert(strcmp(last_method, "sessions.create") == 0);
    params = cJSON_Parse(last_params); assert(params != NULL && cJSON_GetArraySize(params) == 2);
    assert(strcmp(cJSON_GetObjectItem(params, "agentId")->valuestring, "kitsune") == 0);
    assert(strlen(cJSON_GetObjectItem(params, "idempotencyKey")->valuestring) == 32);
    cJSON_Delete(params);
    assert(esp_openclaw_reader_creation_begin(chats) == ESP_ERR_INVALID_STATE);
    const char *created = "{\"ok\":true,\"key\":\"agent:kitsune:chat:server-generated\",\"sessionId\":\"new-session\",\"runStarted\":false}";
    // The response arrives before the loss. Preserve the confirmed outcome.
    respond(created); disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_CONNECTION_LOST); step();
    creation = creation_state(ESP_OPENCLAW_READER_CREATION_CREATED); assert(!creation.can_open);
    before = queries; step(); assert(queries == before);
    connected(104); step();
    respond("{\"agents\":[{\"id\":\"kitsune\",\"name\":\"Kitsune\"}]}"); step();
    creation = creation_state(ESP_OPENCLAW_READER_CREATION_CREATED); assert(creation.can_open);
    assert(esp_openclaw_reader_creation_open(creation.revision - 1) == ESP_ERR_INVALID_STATE);
    assert(esp_openclaw_reader_creation_open(creation.revision) == ESP_OK); step();
    assert(strcmp(last_method, "sessions.preview") == 0);
    assert(strstr(last_params, "agent:kitsune:chat:server-generated") != NULL);
    respond("{\"previews\":[{\"key\":\"agent:kitsune:chat:server-generated\",\"status\":\"empty\",\"items\":[]}]}"); step();
    esp_openclaw_reader_conversations_t *created_view = malloc(sizeof(*created_view)); assert(created_view != NULL);
    esp_openclaw_reader_conversations_copy(created_view);
    assert(created_view->state == ESP_OPENCLAW_READER_READY && created_view->page.count == 1);
    assert(strcmp(created_view->page.sessions[0].key, "agent:kitsune:chat:server-generated") == 0);
    assert(esp_openclaw_reader_reply_begin(ESP_OPENCLAW_READER_REPLY_CONTINUE, created_view->preview_revision) == ESP_OK); step();
    assert(strcmp(last_method, "chat.history") == 0);
    assert(strstr(last_params, "agent:kitsune:chat:server-generated") != NULL);
    free(created_view);
    chat_reply(NULL); step(); reply = reply_state(ESP_OPENCLAW_READER_REPLY_FAILED); esp_openclaw_reader_reply_close(reply.revision);

    // A selection change after confirmation, but before queue admission, sends nothing.
    chats = empty_chats(); assert(esp_openclaw_reader_creation_begin(chats) == ESP_OK);
    creation = creation_state(ESP_OPENCLAW_READER_CREATION_REVIEW);
    assert(esp_openclaw_reader_creation_submit(creation.revision) == ESP_OK);
    esp_openclaw_reader_conversations_close(); before = queries; step();
    creation_state(ESP_OPENCLAW_READER_CREATION_FAILED); assert(queries == before);

    // Once admitted, loss is unknown, including a late success after the loss.
    chats = empty_chats(); assert(esp_openclaw_reader_creation_begin(chats) == ESP_OK);
    creation = creation_state(ESP_OPENCLAW_READER_CREATION_REVIEW);
    assert(esp_openclaw_reader_creation_submit(creation.revision) == ESP_OK); step();
    before = queries;
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_CONNECTION_LOST); step();
    creation = creation_state(ESP_OPENCLAW_READER_CREATION_UNKNOWN);
    assert(!creation.can_create && !creation.can_open);
    respond(created); step(); creation_state(ESP_OPENCLAW_READER_CREATION_UNKNOWN); assert(queries == before);
    connected(105); step(); respond("{\"agents\":[]}"); step();
    // Explicit validation rejection is safe to review again; missing metadata
    // and transport failures still cannot prove that the write never committed.
    chats = empty_chats(); assert(esp_openclaw_reader_creation_begin(chats) == ESP_OK);
    creation = creation_state(ESP_OPENCLAW_READER_CREATION_REVIEW);
    assert(esp_openclaw_reader_creation_submit(creation.revision) == ESP_OK); step();
    assert(response != NULL);
    esp_openclaw_node_gateway_result_t denied = {.ok = false, .error_code = "FORBIDDEN"};
    response(&client, &denied, response_context); response = NULL; response_context = NULL; step();
    creation = creation_state(ESP_OPENCLAW_READER_CREATION_FAILED);
    assert(!creation.can_create && !creation.can_open);
    // A deadline also keeps the outcome unknown; it never replays the write.
    chats = empty_chats(); assert(esp_openclaw_reader_creation_begin(chats) == ESP_OK);
    creation = creation_state(ESP_OPENCLAW_READER_CREATION_REVIEW);
    assert(esp_openclaw_reader_creation_submit(creation.revision) == ESP_OK); step();
    now += 15000000; step(); creation_state(ESP_OPENCLAW_READER_CREATION_CREATING);
    now += 15000000; before = queries; step();
    creation_state(ESP_OPENCLAW_READER_CREATION_UNKNOWN); assert(queries == before);
    respond(created); disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    creation_state(ESP_OPENCLAW_READER_CREATION_UNKNOWN);
    connected(106); step(); respond("{\"agents\":[]}"); step();
    chats = empty_chats(); assert(esp_openclaw_reader_creation_begin(chats) == ESP_OK);
    creation = creation_state(ESP_OPENCLAW_READER_CREATION_REVIEW);
    assert(esp_openclaw_reader_creation_submit(creation.revision) == ESP_OK); step();
    // An invalid routing response cannot be opened or retried as a known failure.
    respond("{\"ok\":true,\"key\":\"agent:other:main\",\"sessionId\":\"s\",\"runStarted\":false}"); step();
    creation = creation_state(ESP_OPENCLAW_READER_CREATION_UNKNOWN);
    assert(esp_openclaw_reader_creation_open(creation.revision) == ESP_ERR_INVALID_STATE);
    chats = empty_chats(); assert(esp_openclaw_reader_creation_begin(chats) == ESP_OK);
    creation = creation_state(ESP_OPENCLAW_READER_CREATION_REVIEW);
    assert(esp_openclaw_reader_creation_submit(creation.revision) == ESP_OK); step(); respond(created); step();
    creation = creation_state(ESP_OPENCLAW_READER_CREATION_CREATED);
    node_connected(24);
    assert(esp_openclaw_reader_creation_open(creation.revision) == ESP_ERR_INVALID_STATE);
    step(); creation_state(ESP_OPENCLAW_READER_CREATION_CREATED);
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    connected(107); step(); respond("{\"agents\":[]}"); step();
    request_messaging(); approve_response(approved); step();
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    connected(108); step(); respond("{\"agents\":[]}"); step();
    chats = empty_chats(); assert(esp_openclaw_reader_creation_begin(chats) == ESP_OK);
    creation = creation_state(ESP_OPENCLAW_READER_CREATION_REVIEW);
    assert(esp_openclaw_reader_creation_submit(creation.revision) == ESP_OK);
    before = queries; node_connected(25); step();
    creation_state(ESP_OPENCLAW_READER_CREATION_FAILED); assert(queries == before);
    puts("Dial reader: creation, reviewed replies, approval, handoff, loss and stale-connection cases passed");
    return 0;
}
