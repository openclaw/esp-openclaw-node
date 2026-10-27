#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dial_reader.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

struct esp_openclaw_node { uint32_t id; const char *uri; };
static struct esp_openclaw_node node = {0, "ws://gateway-a.example"};
static struct esp_openclaw_node client = {0, "ws://gateway-a.example"};
static esp_openclaw_node_event_cb_t event_cb;
static void (*worker)(void *);
static jmp_buf idle;
static int waits, locked, creates, connects, disconnects, destroys, scopes, delays;
static esp_err_t create_result = ESP_OK, connect_result = ESP_ERR_NOT_FOUND, disconnect_result = ESP_OK;
static bool disconnect_during_delay, in_callback;
static esp_openclaw_node_handle_t fail_uri;
static int64_t now;
static int queries;
static esp_err_t query_result = ESP_OK;
static esp_openclaw_node_gateway_request_cb_t response;
static void *response_context;
int64_t esp_timer_get_time(void) { return now; }
esp_err_t esp_openclaw_node_gateway_request_on_connection(esp_openclaw_node_handle_t handle,
    uint32_t connection, const char *method, const char *params,
    esp_openclaw_node_gateway_request_cb_t callback, void *context) {
    assert(!locked && !in_callback && handle == &client && connection == client.id);
    assert(strcmp(method, "agents.list") == 0 && strcmp(params, "{}") == 0);
    ++queries;
    if (query_result == ESP_OK) { assert(response == NULL); response = callback; response_context = context; }
    return query_result;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &locked; }
void xSemaphoreTake(SemaphoreHandle_t sem, uint32_t ticks) {
    assert(sem == &locked && ticks == portMAX_DELAY && !locked && !in_callback); locked = 1;
}
void xSemaphoreGive(SemaphoreHandle_t sem) { assert(sem == &locked && locked); locked = 0; }
void vSemaphoreDelete(SemaphoreHandle_t sem) { assert(sem == &locked && !locked); }
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
        dial_reader_node_event(ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    }
}
void esp_openclaw_node_config_init_default(esp_openclaw_node_config_t *config) { memset(config, 0, sizeof(*config)); }
esp_err_t esp_openclaw_node_create(const esp_openclaw_node_config_t *config, esp_openclaw_node_handle_t *out) {
    assert(!locked && strcmp(config->role, "operator") == 0 && config->event_cb != NULL);
    ++creates;
    if (create_result != ESP_OK) { esp_err_t err = create_result; create_result = ESP_OK; return err; }
    event_cb = config->event_cb; *out = &client; return ESP_OK;
}
esp_err_t esp_openclaw_node_register_scope(esp_openclaw_node_handle_t handle, const char *scope) {
    assert(handle == &client && strcmp(scope, "operator.read") == 0); ++scopes; return ESP_OK;
}
esp_err_t esp_openclaw_node_destroy(esp_openclaw_node_handle_t handle) {
    assert(handle == &client); ++destroys; return ESP_OK;
}
esp_err_t esp_openclaw_node_request_connect(esp_openclaw_node_handle_t handle, const esp_openclaw_node_connect_request_t *request) {
    assert(!locked && handle == &client && request->source == ESP_OPENCLAW_NODE_CONNECT_SOURCE_SAVED_SESSION);
    assert(request->gateway_uri == NULL && request->value == NULL); ++connects; return connect_result;
}
esp_err_t esp_openclaw_node_request_disconnect(esp_openclaw_node_handle_t handle) {
    assert(!locked && handle == &client); ++disconnects; return disconnect_result;
}
uint32_t esp_openclaw_node_get_connection_id(esp_openclaw_node_handle_t handle) { assert(!in_callback); return handle != NULL ? handle->id : 0; }
char *esp_openclaw_node_dup_gateway_uri(esp_openclaw_node_handle_t handle) { assert(!in_callback); if (handle == fail_uri) { fail_uri = NULL; return NULL; } return strdup(handle->uri); }

static void step(void) { waits = 0; if (!setjmp(idle)) worker(NULL); assert(!locked); }
static void status(const char *expected, bool ready) {
    cJSON *result = dial_reader_status();
    assert(result != NULL);
    assert(strcmp(cJSON_GetObjectItem(result, "state")->valuestring, expected) == 0);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(result, "readConnected")) == ready);
    cJSON_Delete(result);
}
static void agent_status(dial_agents_state_t expected, size_t count) {
    dial_agents_snapshot_t snapshot;
    dial_reader_agents_copy(&snapshot);
    assert(snapshot.state == expected && snapshot.page.count == count);
}
static void respond(const char *json) {
    assert(response != NULL);
    esp_openclaw_node_gateway_result_t result = {.ok = json != NULL, .payload_json = json};
    locked = 1; in_callback = true;
    response(&client, &result, response_context);
    in_callback = false; locked = 0; response = NULL; response_context = NULL;
}
static void node_connected(uint32_t id) {
    node.id = id; locked = 1; in_callback = true;
    dial_reader_node_event(ESP_OPENCLAW_NODE_EVENT_CONNECTED);
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
    assert(dial_reader_start(NULL) == ESP_ERR_INVALID_ARG);
    assert(dial_reader_start(&node) == ESP_OK);
    assert(dial_reader_start(&node) == ESP_ERR_INVALID_STATE);
    step(); status("offline", false); assert(creates == 0);

    node_connected(11); create_result = ESP_ERR_NO_MEM;
    step(); status("error", false); assert(connects == 0);
    step(); status("access-needed", false); assert(creates == 2 && scopes == 1 && connects == 1);
    step(); assert(connects == 1); // Missing handoff does not become an auth retry loop.

    node_connected(12); connect_result = ESP_OK;
    step(); status("connecting", false); connected(71); step(); status("ready", true);
    assert(creates == 2 && scopes == 1 && destroys == 0); // Reuse the saved-session handle.
    node.id = 0; status("offline", false); // Readiness cannot outlive the primary transport.
    dial_reader_node_event(ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
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
    agent_status(DIAL_AGENTS_IDLE, 0);
    assert(dial_reader_agents_refresh(0) == ESP_OK); step();
    agent_status(DIAL_AGENTS_LOADING, 0); assert(queries == 1);
    char roster[] = "{\"agents\":[{\"id\":\"kitsune\",\"identity\":{\"name\":\"Kitsune\",\"emoji\":\"🦊\"}}]}";
    respond(roster); memset(roster, 'x', sizeof(roster) - 1); step();
    agent_status(DIAL_AGENTS_READY, 1);
    dial_agents_snapshot_t snapshot; dial_reader_agents_copy(&snapshot);
    assert(strcmp(snapshot.page.agents[0].name, "Kitsune") == 0);

    assert(dial_reader_agents_refresh(1) == ESP_OK); step();
    assert(dial_reader_agents_refresh(0) == ESP_OK);
    respond("{\"agents\":[{\"id\":\"old\"}]}"); step();
    agent_status(DIAL_AGENTS_LOADING, 0); assert(queries == 3);
    respond("{\"agents\":[]}"); step(); agent_status(DIAL_AGENTS_EMPTY, 0);

    assert(dial_reader_agents_refresh(0) == ESP_OK); step();
    node.id = 0; dial_reader_node_event(ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    agent_status(DIAL_AGENTS_OFFLINE, 0);
    node.uri = "ws://gateway-c.example"; node_connected(19); step();
    respond("{\"agents\":[{\"id\":\"old\"}]}");
    disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    client.uri = node.uri; connected(79); step(); agent_status(DIAL_AGENTS_LOADING, 0);
    respond("{\"agents\":[{\"id\":\"new\"}]}"); step();
    agent_status(DIAL_AGENTS_READY, 1); dial_reader_agents_copy(&snapshot);
    assert(strcmp(snapshot.page.agents[0].id, "new") == 0);

    query_result = ESP_ERR_NO_MEM;
    assert(dial_reader_agents_refresh(0) == ESP_OK); step(); step();
    agent_status(DIAL_AGENTS_ERROR, 0); query_result = ESP_OK;
    assert(dial_reader_agents_refresh(0) == ESP_OK); step(); respond("invalid"); step();
    agent_status(DIAL_AGENTS_ERROR, 0);
    assert(dial_reader_agents_refresh(0) == ESP_OK); step();
    char *oversized = malloc(ESP_OPENCLAW_DIRECTORY_MAX_JSON_BYTES + 2);
    memset(oversized, 'x', ESP_OPENCLAW_DIRECTORY_MAX_JSON_BYTES + 1);
    oversized[ESP_OPENCLAW_DIRECTORY_MAX_JSON_BYTES + 1] = '\0';
    respond(oversized); free(oversized); step(); agent_status(DIAL_AGENTS_TOO_LARGE, 0);

    assert(dial_reader_agents_refresh(0) == ESP_OK); step(); now += 15000000;
    before = disconnects; step(); assert(disconnects == before + 1);
    respond(NULL); disconnected(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED); step();
    connected(80); step(); agent_status(DIAL_AGENTS_ERROR, 0);
    before = queries; step(); assert(queries == before); // Explicit refresh after a timed-out request.
    assert(dial_reader_agents_refresh(0) == ESP_OK); step();
    respond("{\"agents\":[]}"); step(); agent_status(DIAL_AGENTS_EMPTY, 0);
    puts("Dial reader: role, handoff, loss, auth, origin and stale-connection cases passed");
    return 0;
}
