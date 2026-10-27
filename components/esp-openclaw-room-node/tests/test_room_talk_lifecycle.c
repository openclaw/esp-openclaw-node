/* Compile the actual controller unchanged; statics are visible only to fixture
 * bootstrapping and observations. All call admission/event ingress uses the
 * registered production handlers. Talk is a separate real translation unit. */
#include "host/room_host_fakes.h"
#include <stdarg.h>
#include <stdio.h>

static int console_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
#define printf(...) console_printf(__VA_ARGS__)
#include "../esp_openclaw_room_node.c"
#undef printf

static FILE *console_capture;
static bool console_competing_writer;
static const char competing_record[] = "I (1) synthetic: competing stdout record\n";

static int console_printf(const char *format, ...)
{
    FILE *output = console_capture != NULL ? console_capture : stdout;
    va_list args;
    va_start(args, format);
    int result = vfprintf(output, format, args);
    va_end(args);
    if (console_capture != NULL && console_competing_writer) {
        /* A normal writer can run after a complete stdio call, not inside it. */
        console_competing_writer = false;
        host_require(fputs(competing_record, output) >= 0, "competing stdio writer");
    }
    return result;
}

static unsigned failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, message); \
        ++failures; \
    } \
} while (0)

static const char *closed_a =
    "{\"voiceSessionId\":\"voice-a\",\"talkEvent\":{\"sessionId\":\"voice-a\","
    "\"type\":\"session.closed\",\"final\":true}}";

static void bootstrap(void)
{
    /* Bypass board/NVS/network/REPL startup; preserve actual registration and
     * worker entrypoints, queue capacity and timeout callback. */
    state_lock = xSemaphoreCreateMutex();
    talk_teardown_queue = xQueueCreate(1, sizeof(talk_teardown_request_t));
    host_require(create_connection_timers() == ESP_OK, "connection retry timers registered");
    host_require(xTaskCreate(talk_start_worker_task, "talk_start", 8192, NULL, 7, &talk_start_worker) == pdPASS,
        "start worker registered");
    host_require(xTaskCreate(talk_teardown_task, "talk_teardown", 6144, NULL, 6, NULL) == pdPASS,
        "teardown worker registered");
    media_ready = true;
    media_initialized = true;
    host_require(start_node_client() == ESP_OK, "real node command registration");
    host_emit_node(node_client, ESP_OPENCLAW_NODE_EVENT_CONNECTED);
    host_fire_operator_timer(operator_start_timer);
    host_run_task("talk_teardown");
    host_emit_node(operator_client, ESP_OPENCLAW_NODE_EVENT_CONNECTED);
    host_require(operator_ready && node_ready, "fixture clients connected");
}

static void admit(void)
{
    host_command(node_client, "talk.start");
    host_require(talk_start_in_flight && host.opens == host.closes, "command admits asynchronously");
}
static void start_pending_config(void)
{
    admit();
    host_run_task("talk_start");
    host_require(webrtc != NULL && !talk_start_in_flight && talk_dialing && host.config_requests == host.opens,
        "real room worker started real Talk config RPC");
}
static void connect_call(void)
{
    start_pending_config();
    host_reply_config();
    CHECK(strcmp(host.create_key, "agent:fixture:main") == 0, "default Talk uses Gateway routing");
    host_reply_create(true);
    host_require(host.create_requests == host.opens && host.ice_callbacks == host.opens && host.signaling_connected == host.opens,
        "real Talk accepted Gateway-owned voice-a");
    host_emit_peer(webrtc, ESP_WEBRTC_EVENT_CONNECTED);
    host_run_task("talk_teardown");
    host_require(talk_active && !talk_dialing && host.media_owned && !host.ambient && host.ui == ROOM_UI_SPEAKING,
        "connected media precondition");
}

static void operator_scopes(void)
{
    CHECK(host_node_scope_count(node_client) == 0, "node role requests no operator scopes");
    CHECK(host_node_has_scope(operator_client, "operator.read"), "Talk routing can read configuration");
    CHECK(host_node_has_scope(operator_client, "operator.talk"), "operator can open Talk sessions");
#if CONFIG_OPENCLAW_ROOM_VOICE_ACTIONS
    CHECK(host_node_scope_count(operator_client) == 4, "voice actions request read, talk, write, and admin");
    CHECK(host_node_has_scope(operator_client, "operator.write"), "opt-in requests write approval");
    CHECK(host_node_has_scope(operator_client, "operator.admin"), "opt-in requests owner approval");
#else
    CHECK(host_node_scope_count(operator_client) == 2, "default remains read and talk only");
    CHECK(!host_node_has_scope(operator_client, "operator.write"), "default connection does not request write access");
    CHECK(!host_node_has_scope(operator_client, "operator.admin"), "default connection does not request owner access");
#endif
}

static const char *approved_scope =
    "{\"requestId\":\"scope-a\",\"status\":\"approved\",\"deviceToken\":\"approved-test-token\","
    "\"scopes\":[\"operator.read\",\"operator.talk\",\"operator.write\",\"operator.admin\"]}";

static void voice_actions_provision(void)
{
    char *request[] = {"voice-actions", "request"};
    char *complete[] = {"voice-actions", "complete", "scope-a"};
    console_capture = tmpfile();
    host_require(console_capture != NULL, "capture credential-free console output");
    CHECK(voice_actions_console_command(1, request) != 0, "invalid usage submits no request");
    host_emit_node(operator_client, ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    CHECK(voice_actions_console_command(2, request) != 0, "disconnected operator cannot request approval");
    host_emit_node(operator_client, ESP_OPENCLAW_NODE_EVENT_CONNECTED);
    CHECK(host.scope_requests == 0, "invalid requests never reach the Gateway");
    CHECK(voice_actions_console_command(2, request) == 0, "USB explicitly requests owner approval");
    CHECK(strcmp(host.scope_method, "device.scopes.requestUpgrade") == 0, "uses the identity-bound upgrade API");
    cJSON *params = cJSON_Parse(host.scope_params);
    cJSON *scopes = cJSON_GetObjectItemCaseSensitive(params, "scopes");
    CHECK(cJSON_GetArraySize(scopes) == 4, "requests exactly four scopes");
    const char *expected[] = {"operator.read", "operator.talk", "operator.write", "operator.admin"};
    for (int i = 0; i < 4; ++i) {
        cJSON *scope = cJSON_GetArrayItem(scopes, i);
        CHECK(cJSON_IsString(scope) && strcmp(scope->valuestring, expected[i]) == 0,
            "approval explicitly includes owner access");
    }
    cJSON_Delete(params);
    host_reply_scope(true, "{\"requestId\":\"scope-a\"}");
    CHECK(host.token_connects == 0, "request ID is not approval or a credential");
    CHECK(voice_actions_console_command(3, complete) == 0, "completion waits for the exact approval");
    CHECK(strcmp(host.scope_method, "device.scopes.waitUpgrade") == 0, "uses the identity-bound wait API");
    params = cJSON_Parse(host.scope_params);
    cJSON *id = cJSON_GetObjectItemCaseSensitive(params, "requestId");
    CHECK(cJSON_IsString(id) && strcmp(id->valuestring, "scope-a") == 0, "completion carries the requested ID");
    cJSON_Delete(params);
    host_reply_scope(true, approved_scope);
    CHECK(host.token_connects == 1 && host.token_node == operator_client &&
        strcmp(host.token_value, "approved-test-token") == 0 &&
        strcmp(host.token_uri, "wss://gateway.example") == 0,
        "approved token enters the existing operator connection lifecycle at its original Gateway");
    const char *invalid[] = {
        "{}",
        "{\"requestId\":\"scope-b\",\"status\":\"approved\",\"deviceToken\":\"wrong\"}",
        "{\"requestId\":\"scope-a\",\"status\":\"rejected\"}",
        "{\"requestId\":\"scope-a\",\"status\":\"approved\",\"deviceToken\":\"wrong\",\"scopes\":[\"operator.write\"]}",
        "{\"requestId\":\"scope-a\",\"status\":\"approved\",\"deviceToken\":\"wrong\",\"scopes\":[\"operator.read\",\"operator.talk\",\"operator.write\"]}",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        CHECK(voice_actions_console_command(3, complete) == 0, "submit invalid-result case");
        host_reply_scope(true, invalid[i]);
    }
    CHECK(voice_actions_console_command(3, complete) == 0, "submit rejected RPC case");
    host_reply_scope(false, NULL);
    CHECK(host.token_connects == 1, "missing, mismatched, rejected, and insufficient results cannot reconnect");
    host.fail_scope_submit = true;
    CHECK(voice_actions_console_command(2, request) != 0, "failed submission releases its context");
    rewind(console_capture);
    char output[4096] = {0};
    size_t size = fread(output, 1, sizeof(output) - 1, console_capture);
    CHECK(size < sizeof(output) - 1 && strstr(output, "approved-test-token") == NULL,
        "console output never includes the replacement token");
    fclose(console_capture);
    console_capture = NULL;
}

static void voice_actions_stale(void)
{
    char *complete[] = {"voice-actions", "complete", "scope-a"};
    CHECK(voice_actions_console_command(3, complete) == 0, "submit before reconnect");
    host_emit_node(operator_client, ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    host_emit_node(operator_client, ESP_OPENCLAW_NODE_EVENT_CONNECTED);
    host_reply_scope(true, approved_scope);
    CHECK(host.token_connects == 0, "replacement connection invalidates outstanding approval work");
    CHECK(voice_actions_console_command(3, complete) == 0, "submit before Gateway rebind");
    host.operator_uri = "ws://replacement.example:18789";
    host_reply_scope(true, approved_scope);
    CHECK(host.token_connects == 0, "changed Gateway cannot receive the old Gateway credential");
    CHECK(voice_actions_console_command(3, complete) == 0, "submit before Talk admission");
    connect_call();
    host_reply_scope(true, approved_scope);
    CHECK(host.token_connects == 0, "approval cannot replace the operator during an admitted Talk call");
}

static void home_connection_facts(void)
{
    host_run_task("talk_teardown");
    CHECK(host.home.gateway == ROOM_UI_GATEWAY_CONNECTED && host.home.talk == ROOM_UI_TALK_READY,
        "node and operator connections independently make Gateway connected and Talk ready");
    host.wifi.has_saved_network = true;
    host.wifi.connected = true;
    home_network_event(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    CHECK(host.home.wifi == ROOM_UI_WIFI_CONNECTED, "IP acquisition updates Wi-Fi");
    home_network_event(NULL, IP_EVENT, IP_EVENT_STA_LOST_IP, NULL);
    CHECK(host.home.wifi == ROOM_UI_WIFI_OFFLINE && host.home.gateway == ROOM_UI_GATEWAY_CONNECTED,
        "IP loss is not a node-session or operator-session claim");
    host_emit_node(node_client, ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    CHECK(host.home.gateway == ROOM_UI_GATEWAY_OFFLINE && host.home.talk == ROOM_UI_TALK_READY,
        "node disconnect does not mean unpaired or revoke the operator");
    host_emit_node(operator_client, ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    host_run_task("talk_teardown");
    CHECK(host.home.talk == ROOM_UI_TALK_WAITING, "operator disconnect waits, not missing session");
    host.node_connect_result = ESP_ERR_NOT_FOUND;
    CHECK(request_node_connection() == ESP_ERR_NOT_FOUND && host.home.gateway == ROOM_UI_GATEWAY_NO_SESSION,
        "only a missing saved-session response requests node pairing");
    host.operator_connect_result = ESP_ERR_NOT_FOUND;
    host_fire_operator_timer(operator_start_timer);
    host_run_task("talk_teardown");
    CHECK(host.home.talk == ROOM_UI_TALK_NO_SESSION, "missing operator session is distinct from node pairing");
    const esp_err_t rejected[] = {ESP_ERR_INVALID_STATE, ESP_FAIL};
    for (size_t i = 0; i < sizeof(rejected) / sizeof(*rejected); ++i) {
        host.node_connect_result = rejected[i];
        CHECK(request_node_connection() == rejected[i] && host.home.gateway == ROOM_UI_GATEWAY_NO_SESSION,
            "busy or failed node requests do not disprove a missing session");
        host.operator_connect_result = rejected[i];
        host_fire_operator_timer(operator_start_timer);
        host_run_task("talk_teardown");
        CHECK(host.home.talk == ROOM_UI_TALK_NO_SESSION,
            "busy normalized to success and other operator failures retain the absence fact");
        host_emit_node(operator_client, ESP_OPENCLAW_NODE_EVENT_CONNECT_FAILED);
        host_run_task("talk_teardown");
    }
    host.node_connect_result = ESP_OK;
    CHECK(request_node_connection() == ESP_OK && host.home.gateway == ROOM_UI_GATEWAY_CONNECTING,
        "an accepted node request clears missing material, not connection readiness");
    host.operator_connect_result = ESP_OK;
    host_fire_operator_timer(operator_start_timer);
    host_run_task("talk_teardown");
    CHECK(host.home.talk == ROOM_UI_TALK_WAITING, "accepted operator reconnect is not Talk ready");
    host_emit_node(node_client, ESP_OPENCLAW_NODE_EVENT_CONNECT_FAILED);
    host_emit_node(operator_client, ESP_OPENCLAW_NODE_EVENT_CONNECT_FAILED);
    host_run_task("talk_teardown");
    CHECK(host.home.gateway == ROOM_UI_GATEWAY_OFFLINE && host.home.talk == ROOM_UI_TALK_WAITING,
        "failed accepted connections do not resurrect stale missing-session facts");
    host_emit_node(node_client, ESP_OPENCLAW_NODE_EVENT_CONNECTED);
    host_emit_node(operator_client, ESP_OPENCLAW_NODE_EVENT_CONNECTED);
    host_run_task("talk_teardown");
    CHECK(host.home.gateway == ROOM_UI_GATEWAY_CONNECTED && host.home.talk == ROOM_UI_TALK_READY,
        "authoritative connections clear prior missing-session facts");
}
static void drain(void) { host_run_task("talk_teardown"); }
static void stop(void) { host_command(node_client, "talk.stop"); }

static void expect_open(void)
{
    CHECK(host.closes == 0, "unrelated event must not close WebRTC");
    CHECK(host.media_ends == 0 && host.media_owned && !host.ambient, "unrelated event retains media ownership");
    CHECK(webrtc != NULL && talk_active, "unrelated event retains active call");
    CHECK(host.ui == ROOM_UI_SPEAKING, "unrelated event retains speaking UI");
    CHECK(host.home.talk == ROOM_UI_TALK_ACTIVE, "active Talk has its own home fact");
}
static void expect_closed(void)
{
    printf("OBSERVED close=%u release=%u ambient_restore=%u media_owned=%d active=%d peer_disconnects=%u\n",
        host.closes, host.media_ends, host.ambient_restores, host.media_owned, talk_active, host.peer_disconnects);
    CHECK(host.closes == 1, "canonical esp_webrtc_close must run exactly once");
    CHECK(host.media_ends == 1 && !host.media_owned, "canonical teardown must release Talk media ownership once");
    CHECK(host.ambient && host.ambient_restores == 1, "canonical teardown must restore ambient capture once");
    CHECK(webrtc == NULL && !talk_active && !talk_dialing && !talk_closing && !talk_start_in_flight,
        "controller must clear the call lifecycle");
    CHECK(!host_timer_active(talk_timeout_timer), "canonical teardown must stop call timer");
}
static void expect_voice_cleanup(void)
{
    CHECK(host.close_requests == 1, "real Talk stop closes the retained voice identity once");
    CHECK(strcmp(host.close_voice, "voice-a") == 0, "real Talk retains returned voice-a");
    CHECK(strcmp(host.close_key, "agent:fixture:main") == 0, "real Talk retains resolved session key");
    CHECK(host.close_node == operator_client, "real Talk close targets original operator");
}

static void operator_disconnect(void)
{
    connect_call();
    host_emit_node(operator_client, ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    CHECK(!operator_ready && operator_start_scheduled, "operator reconnect remains scheduled");
    CHECK(host.closes == 0, "operator callback must not synchronously close");
    drain();
    CHECK(host.peer_disconnects == 0, "media never emitted DISCONNECTED");
    expect_closed(); /* Confirmed red at the recorded baseline. */
}
static void terminal_event(void)
{
    connect_call();
    host_emit_gateway(operator_client, "talk.event", closed_a);
    CHECK(host.closes == 0, "Gateway callback must defer close");
    drain();
    CHECK(host.peer_disconnects == 0, "media never emitted DISCONNECTED");
    expect_closed(); /* Confirmed red at the recorded baseline. */
}
static void terminal_before_identity(void)
{
    start_pending_config();
    host_emit_gateway(operator_client, "talk.event", closed_a);
    drain();
    CHECK(!talk_cancel_requested && host.closes == 0, "no identity inferred while config is pending");
    host_reply_config();
    host_emit_gateway(operator_client, "talk.event", closed_a);
    drain();
    CHECK(!talk_cancel_requested && host.closes == 0, "no identity inferred from routing before create returns");
    host_reply_create(true);
    host_emit_gateway(operator_client, "talk.event", closed_a);
    drain();
    expect_closed();
}
static void node_disconnect(void)
{
    connect_call();
    host_emit_node(node_client, ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    drain();
    CHECK(!node_ready && node_reconnect_scheduled && operator_ready, "only node reconnects");
    expect_open();
}
static void unrelated_voice(void)
{
    connect_call();
    host_emit_gateway(operator_client, "talk.event",
        "{\"voiceSessionId\":\"voice-b\",\"talkEvent\":{\"sessionId\":\"voice-b\",\"type\":\"session.closed\",\"final\":true}}");
    drain();
    expect_open();
}
static void recoverable_error(void)
{
    connect_call();
    host_emit_gateway(operator_client, "talk.event",
        "{\"voiceSessionId\":\"voice-a\",\"talkEvent\":{\"sessionId\":\"voice-a\",\"type\":\"session.error\",\"final\":true}}");
    drain();
    expect_open();
}
static void stale_operator(void)
{
    connect_call();
    /* An old emitter with the same registered callback, distinct Node handle. */
    esp_openclaw_node_config_t config = {0};
    esp_openclaw_node_config_init_default(&config);
    config.event_cb = operator_event;
    config.gateway_event_cb = operator_gateway_event;
    esp_openclaw_node_handle_t old = NULL;
    host_require(esp_openclaw_node_create(&config, &old) == ESP_OK, "old operator fixture");
    host_emit_node(old, ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    host_emit_gateway(old, "talk.event", closed_a);
    drain();
    CHECK(operator_ready && !operator_start_scheduled, "stale operator must not alter current readiness/retry");
    expect_open();
}
static void explicit_stop(void)
{
    connect_call();
    stop();
    CHECK(host.closes == 0, "stop command defers teardown");
    drain();
    expect_closed();
    expect_voice_cleanup();
}
static void duplicate_media_terminal(void)
{
    connect_call();
    host_emit_peer(webrtc, ESP_WEBRTC_EVENT_DISCONNECTED);
    host_emit_peer(webrtc, ESP_WEBRTC_EVENT_DISCONNECTED);
    drain();
    expect_closed();
    expect_voice_cleanup();
}
static void duplicate_gateway_after_stop(void)
{
    explicit_stop();
    host_emit_gateway(operator_client, "talk.event", closed_a);
    host_emit_gateway(operator_client, "talk.event", closed_a);
    drain();
    expect_closed();
    expect_voice_cleanup();
}
static void cancel_before_worker(void)
{
    admit();
    stop();
    host_run_task("talk_start");
    drain();
    CHECK(!talk_start_in_flight && webrtc == NULL, "canceled admission is cleared");
    CHECK(host.opens == 0 && host.media_begins == 0 && host.config_requests == 0, "cancel before worker acquires nothing");
}
static void cancel_at_media_gate(void)
{
    host.at_media_begin = stop;
    admit();
    host_run_task("talk_start");
    host.at_media_begin = NULL;
    drain();
    CHECK(host.opens == 0 && host.config_requests == 0, "cancel at media gate never opens/signals");
    CHECK(host.media_begins == 1 && host.media_ends == 1 && !host.media_owned && host.ambient,
        "cancel after gate releases still-ambient ownership once");
    CHECK(!talk_start_in_flight && webrtc == NULL, "gate cancellation clears admission");
}
static void cancel_config_pending(void)
{
    start_pending_config();
    stop();
    drain();
    host_reply_config();
    CHECK(host.create_requests == 0 && host.close_requests == 0, "late config cannot create a voice after stop");
    expect_closed();
}
static void cancel_create_pending(void)
{
    start_pending_config();
    host_reply_config();
    stop();
    drain();
    host_reply_create(true);
    CHECK(host.ice_callbacks == 0 && host.signaling_connected == 0, "late create cannot touch closed WebRTC");
    expect_closed();
    expect_voice_cleanup();
}
static void stop_inside_start(void)
{
    host_reply_config();
    host_reply_create(true);
    stop();
    CHECK(host.closes == 0 && talk_cancel_requested, "stop during start only marks cancellation");
    host_emit_peer(webrtc, ESP_WEBRTC_EVENT_CONNECTED);
    CHECK(!talk_active, "CONNECTED during canceled start cannot revive call");
}
static void cancel_inside_start(void)
{
    host.inside_webrtc_start = stop_inside_start;
    admit();
    host_run_task("talk_start");
    host.inside_webrtc_start = NULL;
    CHECK(host.closes == 0, "start worker defers canceled close until it returns");
    drain();
    expect_closed();
    expect_voice_cleanup();
}

static void cancel_before_signaling_start(void)
{
    host.before_signaling_start = stop;
    admit();
    host_run_task("talk_start");
    drain();
    expect_closed();
    CHECK(host.config_requests == 0 && host.ui == ROOM_UI_IDLE,
        "a canceled signaling start fails gracefully without manufacturing setup failure");
}
static void setup_failure(void)
{
    start_pending_config();
    host_reply_config();
    host_reply_create(false);
    CHECK(host.closes == 0, "setup-failure RPC callback defers teardown");
    drain();
    CHECK(host.ui == ROOM_UI_ERROR, "setup failure leaves error UI");
    expect_closed();
}

static void lose_operator(void)
{
    host_emit_node(talk_operator, ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
}

static void loss_before_worker(void)
{
    admit();
    uint32_t generation = talk_generation;
    lose_operator();
    host_emit_node(operator_client, ESP_OPENCLAW_NODE_EVENT_CONNECTED);
    CHECK(talk_cancel_requested && talk_generation == generation, "same-handle reconnect cannot revive admission");
    host_run_task("talk_start");
    drain();
    CHECK(talk_call == NULL && host.opens == 0 && host.config_requests == 0, "lost admission never starts");
}

static void loss_at_media_gate(void)
{
    host.at_media_begin = lose_operator;
    admit();
    host_run_task("talk_start");
    drain();
    CHECK(host.opens == 0 && host.media_ends == 1 && talk_call == NULL, "bound loss at media wait releases once");
}

static void loss_config_pending(void)
{
    start_pending_config();
    lose_operator();
    host_reply_config(); /* Before teardown: cancel must already seal create. */
    CHECK(host.create_requests == 0, "authoritative cancel seals late config immediately");
    drain();
    expect_closed();
}

static void loss_create_pending(void)
{
    start_pending_config();
    host_reply_config();
    esp_openclaw_node_handle_t original = talk_operator;
    lose_operator();
    host_emit_node(operator_client, ESP_OPENCLAW_NODE_EVENT_CONNECTED);
    drain();
    host_reply_create(true);
    CHECK(host.ice_callbacks == 0 && host.close_node == original, "late create cleans frozen operator only");
    expect_closed();
    expect_voice_cleanup();
}

static void lose_inside_start(void)
{
    host_reply_config();
    host_reply_create(true);
    lose_operator();
    host_emit_peer(webrtc, ESP_WEBRTC_EVENT_CONNECTED);
    CHECK(!talk_active && talk_cancel_requested, "CONNECTED cannot clear loss latch");
}
static void loss_inside_start(void)
{
    host.inside_webrtc_start = lose_inside_start;
    admit();
    host_run_task("talk_start");
    drain();
    expect_closed();
}

static esp_openclaw_node_handle_t replacement_operator(void)
{
    esp_openclaw_node_config_t config = {.event_cb = operator_event, .gateway_event_cb = operator_gateway_event};
    esp_openclaw_node_handle_t replacement = NULL;
    host_require(esp_openclaw_node_create(&config, &replacement) == ESP_OK, "replacement operator allocation");
    return replacement;
}

static void displaced_owner_loss(void)
{
    connect_call();
    esp_openclaw_node_handle_t original = talk_operator;
    operator_client = replacement_operator();
    host_emit_node(operator_client, ESP_OPENCLAW_NODE_EVENT_CONNECTED);
    room_ui_state_t before = host.ui;
    host_emit_node(original, ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    CHECK(operator_ready && host.ui == before && !operator_start_scheduled,
        "displaced owner's callback cannot alter replacement readiness/UI");
    CHECK(talk_cancel_requested, "explicit displaced-owner loss still cancels its bound call");
    drain();
    expect_closed();
    CHECK(host.close_node == original, "cleanup retains displaced operator");
}

static void origin_rebind(void)
{
    connect_call();
    host.node_uri = "wss://replacement.example";
    host_emit_node(node_client, ESP_OPENCLAW_NODE_EVENT_CONNECTED);
    CHECK(talk_cancel_requested && !operator_ready, "origin rebind stops established call before disconnect arrives");
    drain();
    expect_closed();
}

static void malformed_events(void)
{
    connect_call();
    const char *events[] = {
        "null", "[]", "{", "{}",
        "{\"voiceSessionId\":\"voice-a\",\"talkEvent\":null}",
        "{\"voiceSessionId\":\"voice-a\",\"talkEvent\":{\"sessionId\":\"voice-b\",\"type\":\"session.closed\"}}",
        "{\"voiceSessionId\":\"voice-b\",\"talkEvent\":{\"sessionId\":\"voice-a\",\"type\":\"session.closed\"}}",
        "{\"voiceSessionId\":\"voice-a\",\"talkEvent\":{\"sessionId\":3,\"type\":\"session.closed\"}}",
        "{\"voiceSessionId\":\"voice-a\",\"talkEvent\":{\"sessionId\":\"voice-a\",\"type\":\"audio.done\",\"final\":true}}",
        "{\"voiceSessionId\":\"voice-a\",\"talkEvent\":{\"sessionId\":\"voice-a\",\"type\":\"session.replaced\",\"final\":true}}",
        "{\"voiceSessionId\":\"voice-a\",\"voiceSessionId\":\"voice-b\",\"talkEvent\":{\"sessionId\":\"voice-a\",\"type\":\"session.closed\"}}",
        "{\"voiceSessionId\":\"voice-a\",\"talkEvent\":{\"sessionId\":\"voice-a\",\"type\":\"session.closed\",\"type\":\"session.error\"}}",
        "{\"voiceSessionId\":\"voice-a\",\"talkEvent\":{\"sessionId\":\"voice-a\",\"type\":\"session.closed\"}} trailing",
        "{\"voiceSessionId\":\"voice-a\\u0000suffix\",\"talkEvent\":{\"sessionId\":\"voice-a\",\"type\":\"session.closed\"}}",
    };
    for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); ++i) {
        host_emit_gateway(operator_client, "talk.event", events[i]);
        drain();
        expect_open();
    }
    host_emit_gateway(operator_client, "other.event", closed_a);
    drain();
    expect_open();
}

static void stop_coalescing(void)
{
    connect_call();
    for (unsigned i = 0; i < 10000; ++i) {
        request_talk_teardown(talk_generation - 1, "stale");
        host_emit_gateway(operator_client, "talk.event", closed_a);
        host_emit_peer(webrtc, ESP_WEBRTC_EVENT_CONNECTED);
        stop();
    }
    CHECK(talk_cancel_requested, "saturated stop wakeup retains durable latch");
    drain();
    expect_closed();
    expect_voice_cleanup();
}

static void reject_replacement(void)
{
    host_command(node_client, "talk.start");
    CHECK(!host.last_started && talk_call != NULL && talk_closing,
        "replacement inadmissible through close, ambient restore, media release");
}
static void replacement_ordering(void)
{
    host.reuse_rtc = true;
    connect_call();
    esp_webrtc_handle_t old_rtc = webrtc;
    uint32_t old_generation = talk_generation;
    uint64_t old_incarnation = talk_operator_incarnation;
    host.inside_close = reject_replacement;
    host.inside_ambient_restore = reject_replacement;
    host.inside_media_end = reject_replacement;
    stop();
    drain();
    host.inside_close = NULL;
    host.inside_ambient_restore = NULL;
    host.inside_media_end = NULL;
    host_emit_node(operator_client, ESP_OPENCLAW_NODE_EVENT_CONNECTED);
    host.create_voice = "voice-b";
    connect_call();
    CHECK(webrtc == old_rtc && talk_generation != old_generation &&
        talk_operator_incarnation != old_incarnation, "replacement deliberately reuses rtc address with fresh identities");
    esp_webrtc_event_t connected = {.type = ESP_WEBRTC_EVENT_CONNECTED};
    esp_webrtc_event_t disconnected = {.type = ESP_WEBRTC_EVENT_DISCONNECTED};
    webrtc_event(&connected, (void *)(uintptr_t)old_generation);
    webrtc_event(&disconnected, (void *)(uintptr_t)old_generation);
    talk_setup_failed(ESP_OPENCLAW_TALK_SETUP_FAILED, (void *)(uintptr_t)old_generation);
    host_fire_timeout_generation(talk_timeout_timer, old_generation);
    host_emit_gateway(operator_client, "talk.event", closed_a);
    wake_talk_teardown(); /* An old wake token is never a teardown action. */
    drain();
    CHECK(host.closes == 1 && talk_active && host.ui == ROOM_UI_SPEAKING && !talk_cancel_requested,
        "old callbacks, timeout, terminal and wakeup cannot change replacement");
    host_fire_timeout(talk_timeout_timer);
    host_fire_timeout_generation(talk_timeout_timer, old_generation);
    drain();
    CHECK(host.closes == 2 && host.media_ends == 2 && host.ambient_restores == 2, "each generation closes once");
    CHECK(strcmp(host.close_voice, "voice-b") == 0, "replacement cleanup uses returned B identity");
}

static void late_create_replacement(void)
{
    start_pending_config();
    host_reply_config();
    uint32_t old_generation = talk_generation;
    esp_openclaw_node_handle_t original = talk_operator;
    stop();
    drain();
    operator_client = replacement_operator();
    host_emit_node(operator_client, ESP_OPENCLAW_NODE_EVENT_CONNECTED);
    start_pending_config();
    CHECK(talk_generation != old_generation, "B admitted while A create is still remote");
    host_reply_create(true);
    CHECK(host.close_node == original && strcmp(host.close_voice, "voice-a") == 0,
        "late A create closes original operator and returned ID");
    CHECK(host.ice_callbacks == 0 && host.closes == 1 && talk_dialing && !talk_cancel_requested,
        "late A result cannot dispatch into B or change its lifecycle");
    host_reply_config();
    host.create_voice = "voice-b";
    host_reply_create(true);
    host_emit_peer(webrtc, ESP_WEBRTC_EVENT_CONNECTED);
    drain();
    host_emit_node(original, ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    host_emit_gateway(original, "talk.event", closed_a);
    drain();
    CHECK(talk_active && operator_ready && host.ui == ROOM_UI_SPEAKING && host.closes == 1,
        "old-owner callbacks cannot change replacement readiness, UI or lifetime");
    stop();
    drain();
    CHECK(host.closes == 2 && host.close_node == operator_client && strcmp(host.close_voice, "voice-b") == 0,
        "replacement closes independently");
}

static void wake_admission_loss(void)
{
    on_wake("synthetic-wake", NULL);
    CHECK(talk_start_in_flight && talk_operator == operator_client, "wake freezes the same owner at admission");
    lose_operator();
    host_run_task("talk_start");
    drain();
    CHECK(host.opens == 0 && talk_call == NULL, "lost wake admission never opens media");
}

static void local_talk_control(void)
{
    request_local_talk(true);
    request_local_talk(true);
    CHECK(talk_call == NULL, "local input defers admission outside the display callback");
    drain();
    CHECK(talk_start_in_flight && talk_call != NULL, "local start admits one call");
    request_local_talk(false);
    drain();
    host_run_task("talk_start");
    drain();
    CHECK(talk_call == NULL && host.opens == 0, "local stop cancels before media acquisition");
    connect_call();
    request_local_talk(false);
    request_local_talk(false);
    drain();
    expect_closed();
    CHECK(host.closes == 1, "repeated local stops close once");
}

static void local_talk_readiness_loss(void)
{
    request_local_talk(true);
    host_emit_node(operator_client, ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    drain();
    CHECK(talk_call == NULL && host.opens == 0, "stale local start cannot use a lost operator");
}

static void immediate_config_failure(void)
{
    host.fail_config_submit = true;
    admit();
    host_run_task("talk_start");
    drain();
    expect_closed();
}
static void immediate_create_failure(void)
{
    start_pending_config();
    host.fail_create_submit = true;
    host_reply_config();
    drain();
    expect_closed();
}
static void open_failure(void)
{
    host.capture_diagnostics = true;
    host.fail_open = true;
    admit();
    host_run_task("talk_start");
    drain();
    CHECK(host.closes == 0 && host.media_ends == 1 && talk_call == NULL, "open failure releases admitted media once");
    CHECK(strstr(host.diagnostics, "stage=sdk_open phase=end result=-1") != NULL,
        "actual open failure classified before cleanup");
    CHECK(strstr(host.diagnostics, "stage=sdk_start") == NULL, "failed open never starts the SDK");
}
static void provider_failure(void)
{
    host.capture_diagnostics = true;
    host.fail_provider = true;
    admit();
    host_run_task("talk_start");
    drain();
    CHECK(host.closes == 1 && host.media_ends == 1 && host.ambient_restores == 0 && talk_call == NULL,
        "pre-start failure closes with capture still ambient");
    CHECK(strstr(host.diagnostics, "stage=sdk_media_provider phase=end result=-1") != NULL,
        "actual provider failure classified");
    CHECK(strstr(host.diagnostics, "stage=sdk_capture_policy") == NULL, "provider short circuit preserved");
}
static void start_failure(void)
{
    host.capture_diagnostics = true;
    host.fail_start = true;
    admit();
    host_run_task("talk_start");
    drain();
    expect_closed();
    CHECK(strstr(host.diagnostics, "stage=sdk_start phase=end result=-1") != NULL, "actual start failure classified");
    CHECK(strstr(host.diagnostics, "stage=timer_start") == NULL, "SDK failure still skips timer start");
}
static void timer_failure(void)
{
    host.fail_timer = true;
    admit();
    host_run_task("talk_start");
    drain();
    host_reply_config();
    expect_closed();
}
static void timeout_stop(void)
{
    connect_call();
    host_fire_timeout(talk_timeout_timer);
    host_fire_timeout(talk_timeout_timer);
    CHECK(!talk_cancel_requested && host.closes == 0, "timeout defers cancellation to the worker");
    drain();
    expect_closed();
}

static void diagnostic_boundaries(void)
{
    host.capture_diagnostics = true;
    start_pending_config();
    host_reply_config();
    host_reply_create(true);
    host_emit_peer(webrtc, ESP_WEBRTC_EVENT_CONNECTING);
    host_emit_peer(webrtc, ESP_WEBRTC_EVENT_PAIRED);
    CHECK(!talk_active, "connecting and paired are not connected");
    host_emit_peer(webrtc, ESP_WEBRTC_EVENT_CONNECTED);
    stop();
    drain();
    const char *stages[] = {"stage=peer_connecting", "stage=peer_paired", "stage=peer_connected",
        "stage=stop_request phase=accepted", "stage=quiesce phase=begin", "stage=quiesce phase=end",
        "stage=sdk_close phase=begin", "stage=sdk_close phase=end", "stage=ambient_restore phase=end",
        "stage=media_release phase=end"};
    const char *cursor = host.diagnostics;
    for (size_t i = 0; i < sizeof(stages) / sizeof(*stages); ++i) {
        const char *next = strstr(cursor, stages[i]);
        CHECK(next != NULL, "actual event and teardown boundaries recorded in order");
        if (next != NULL) cursor = next + strlen(stages[i]);
    }
    CHECK(strstr(host.diagnostics,
        "capture_ok=11 capture_err=2 feed_ok=13 feed_err=3 fetch_ok=17 fetch_err=5 render_ok=19 render_err=7") != NULL,
        "audio record uses actual existing snapshot fields, not inferred network delivery");
    CHECK(host.audio_snapshots == 6, "audio sampled at acquire, three peer events, teardown and release only");
    expect_closed();
}

static void sdk_log_policy_failure(void)
{
    host.capture_diagnostics = true;
    host.fail_log_policy = true;
    admit();
    host_run_task("talk_start");
    CHECK(host.opens == 0 && host.starts == 0 && host.config_requests == 0,
        "failed SDK tag suppression must stop before WebRTC open or negotiation");
    drain();
    CHECK(host.closes == 0 && host.media_ends == 1 && host.ambient && talk_call == NULL,
        "privacy failure uses existing local cleanup with capture still ambient");
    CHECK(strstr(host.diagnostics, "stage=sdk_log_policy phase=end result=-1") != NULL,
        "privacy refusal has a fixed local failure stage");
    CHECK(strstr(host.diagnostics, "stage=sdk_open") == NULL, "no SDK open marker after privacy refusal");
}

static void sdk_log_policy_level(esp_log_level_t level)
{
    host.capture_diagnostics = true;
    host.global_log_level = level;
    host.sdk_log_level = level;
    bool can_set_tag = CONFIG_LOG_DYNAMIC_LEVEL_CONTROL && !CONFIG_LOG_TAG_LEVEL_IMPL_NONE;
    bool allowed = CONFIG_LOG_DYNAMIC_LEVEL_CONTROL
        ? (can_set_tag || level <= ESP_LOG_WARN)
        : (level <= ESP_LOG_WARN && CONFIG_LOG_MAXIMUM_LEVEL < ESP_LOG_INFO);
    admit();
    host_run_task("talk_start");
    CHECK(host.global_log_level == level, "Talk must not mutate global logging");
    CHECK(host.log_set_calls == (unsigned)(can_set_tag && level > ESP_LOG_WARN),
        "set only a supported tag that needs lowering; never raise a quieter policy");
    CHECK(esp_log_level_get("webrtc") == (can_set_tag && level > ESP_LOG_WARN ? ESP_LOG_WARN : level),
        "effective policy matches the selected SDK logging mode");
    CHECK(host.opens == (unsigned)allowed && host.config_requests == (unsigned)allowed,
        "unsafe unsupported logging refuses before SDK open or negotiation");
    if (!allowed) {
        drain();
        CHECK(host.closes == 0 && host.media_ends == 1 && host.ambient && talk_call == NULL,
            "unsupported unsafe logging follows local setup cleanup");
        CHECK(strstr(host.diagnostics, "stage=sdk_log_policy phase=end result=-1") != NULL &&
            strstr(host.diagnostics, "stage=sdk_open") == NULL, "refusal retains fixed diagnostic");
    }
    if (host.config_requests != 0) {
        host_reply_config();
        host_reply_create(true);
    }
}
static void sdk_log_policy_info(void) { sdk_log_policy_level(ESP_LOG_INFO); }
static void sdk_log_policy_warn(void) { sdk_log_policy_level(ESP_LOG_WARN); }
static void sdk_log_policy_error(void) { sdk_log_policy_level(ESP_LOG_ERROR); }

static void check_console_header(room_media_tone_state_t state, room_media_tone_error_t error,
    const char *state_name, const char *error_name, const char *expected_tone, bool competing)
{
    host.console_snapshots = true;
    host.console_tone = (room_media_tone_snapshot_t){
        .state = state, .error = error, .requested_frames = 13,
        .enqueued_frames = 7, .renderer_accepted_frames = 3,
    };
    host.console_tone_state_name = state_name;
    host.console_tone_error_name = error_name;
    host.console_tone_error_name_calls = 0;
    console_capture = tmpfile();
    host_require(console_capture != NULL, "console capture file");
    console_competing_writer = competing;
    char command[] = "diagnostics";
    char subcommand[] = "status";
    char *argv[] = {command, subcommand};
    CHECK(diagnostics_console_command(2, argv) == 0, "diagnostic status remains successful");
    host_require(fflush(console_capture) == 0, "flush console capture");
    rewind(console_capture);
    char actual[2048] = {0};
    size_t length = fread(actual, 1, sizeof(actual) - 1, console_capture);
    host_require(!ferror(console_capture) && feof(console_capture), "complete bounded console capture");
    host_require(fclose(console_capture) == 0, "close console capture");
    console_capture = NULL;
    console_competing_writer = false;
    host.console_snapshots = false;

    char expected[2048];
    int count = snprintf(expected, sizeof(expected),
        "diagnostics=closed Talk=idle tone=%s (13/7/3 requested/queued/accepted)\n"
        "%s"
        "audio mic=0@0us afe=0@0us rx=0@0us renderer accepted/errors=19/7\n"
        "AFE feed accepted/errors=13/3 ring_free=25%% read_interval_max_us=200000 fetch_max_us=12000\n"
        "wifi=disconnected ssid=\"\" rssi=0 dBm heap=40000/16000 B psram=8000000 B\n"
        "Talk VAD silence=%u ms player queues=%u/%u KiB\n",
        expected_tone, competing ? competing_record : "",
        ROOM_TALK_VAD_SILENCE_MS, ROOM_PLAYER_RAW_FIFO_KIB, ROOM_PLAYER_RENDER_FIFO_KIB);
    host_require(count > 0 && (size_t)count < sizeof(expected), "bounded expected console output");
    CHECK(length == (size_t)count && strcmp(actual, expected) == 0,
        "complete diagnostic header and following lines preserve exact bytes under the chosen writer schedule");
    CHECK(host.console_tone_error_name_calls == (state == ROOM_MEDIA_TONE_ERROR ? 1U : 0U),
        "error name is queried only for the error tone state");
}

static void console_diagnostics_bytes(void)
{
    static const struct {
        room_media_tone_state_t state;
        room_media_tone_error_t error;
        const char *state_name, *error_name, *expected;
    } tones[] = {
        {ROOM_MEDIA_TONE_IDLE, ROOM_MEDIA_TONE_ERROR_NONE, "idle", "none", "idle"},
        {ROOM_MEDIA_TONE_RUNNING, ROOM_MEDIA_TONE_ERROR_TASK, "running", "worker task", "running"},
        {ROOM_MEDIA_TONE_DONE, ROOM_MEDIA_TONE_ERROR_NONE, "done", "none", "done"},
        {ROOM_MEDIA_TONE_BUSY, ROOM_MEDIA_TONE_ERROR_NONE, "busy", "none", "busy"},
        {ROOM_MEDIA_TONE_ERROR, ROOM_MEDIA_TONE_ERROR_NONE, "error", "none", "error/none"},
        {ROOM_MEDIA_TONE_ERROR, ROOM_MEDIA_TONE_ERROR_UNAVAILABLE, "error", "unavailable", "error/unavailable"},
        {ROOM_MEDIA_TONE_ERROR, ROOM_MEDIA_TONE_ERROR_TASK, "error", "worker task", "error/worker task"},
        {ROOM_MEDIA_TONE_ERROR, ROOM_MEDIA_TONE_ERROR_RESET, "error", "player reset", "error/player reset"},
        {ROOM_MEDIA_TONE_ERROR, ROOM_MEDIA_TONE_ERROR_STREAM, "error", "PCM stream", "error/PCM stream"},
        {ROOM_MEDIA_TONE_ERROR, ROOM_MEDIA_TONE_ERROR_FRAME_INFO, "error", "frame format", "error/frame format"},
        {ROOM_MEDIA_TONE_ERROR, ROOM_MEDIA_TONE_ERROR_FEED, "error", "frame enqueue", "error/frame enqueue"},
        {ROOM_MEDIA_TONE_ERROR, ROOM_MEDIA_TONE_ERROR_EOS, "error", "EOS enqueue", "error/EOS enqueue"},
        {ROOM_MEDIA_TONE_ERROR, ROOM_MEDIA_TONE_ERROR_RENDER_TIMEOUT, "error", "renderer timeout", "error/renderer timeout"},
    };
    for (size_t i = 0; i < sizeof(tones) / sizeof(*tones); ++i) {
        check_console_header(tones[i].state, tones[i].error,
            tones[i].state_name, tones[i].error_name, tones[i].expected, false);
    }
}

static void console_diagnostics_contention(void)
{
    check_console_header(ROOM_MEDIA_TONE_IDLE, ROOM_MEDIA_TONE_ERROR_NONE,
        "idle", "none", "idle", true);
    check_console_header(ROOM_MEDIA_TONE_ERROR, ROOM_MEDIA_TONE_ERROR_TASK,
        "error", "worker task", "error/worker task", true);
}

static void operator_reconnect_without_task_memory(void)
{
    connect_call();
    unsigned requests = host.operator_connect_requests;
    host.reject_task_creation = true;
    host_emit_node(operator_client, ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    host_fire_operator_timer(operator_start_timer);
    drain();
    CHECK(host.operator_connect_requests == requests + 1,
        "operator retry proceeds without allocating another task");
    expect_closed();
}

static void node_reconnect_without_task_memory(void)
{
    connect_call();
    host.reject_task_creation = true;
    host_emit_node(node_client, ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    CHECK(node_reconnect_scheduled, "node retry remains armed without another task");
    unsigned requests = host.node_connect_requests;
    host_fire_operator_timer(node_reconnect_timer);
    drain();
    CHECK(host.node_connect_requests == requests + 1, "node reconnect proceeds during an operator call");
    expect_open();

    host_emit_node(node_client, ESP_OPENCLAW_NODE_EVENT_CONNECT_FAILED);
    host_emit_node(node_client, ESP_OPENCLAW_NODE_EVENT_CONNECTED);
    host_fire_operator_timer(node_reconnect_timer);
    drain();
    CHECK(host.node_connect_requests == requests + 1, "completed explicit connection supersedes delayed retry");
    expect_open();

    host.node_connect_result = ESP_ERR_NOT_FOUND;
    host_emit_node(node_client, ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    host_fire_operator_timer(node_reconnect_timer);
    drain();
    CHECK(!node_reconnect_scheduled && host.home.gateway == ROOM_UI_GATEWAY_NO_SESSION,
        "missing node session stops retrying and remains visible in connection facts");
    expect_open();
}

static void coalesced_connection_retries(void)
{
    host.reject_task_creation = true;
    unsigned nodes = host.node_connect_requests;
    unsigned operators = host.operator_connect_requests;
    host_emit_node(node_client, ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    host_emit_node(operator_client, ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    host_fire_operator_timer(node_reconnect_timer);
    host_fire_operator_timer(operator_start_timer);
    drain();
    CHECK(host.node_connect_requests == nodes + 1 && host.operator_connect_requests == operators + 1,
        "one coalesced wake consumes both connection retries without allocating tasks");
    CHECK(!node_reconnect_scheduled && !operator_start_scheduled, "both retries release their pending ownership");

    host.node_connect_result = ESP_FAIL;
    host_emit_node(node_client, ESP_OPENCLAW_NODE_EVENT_CONNECT_FAILED);
    host_fire_operator_timer(node_reconnect_timer);
    drain();
    CHECK(node_reconnect_scheduled, "synchronous node failure rearms retry");
    host.node_connect_result = ESP_OK;
    host_fire_operator_timer(node_reconnect_timer);
    drain();
    CHECK(!node_reconnect_scheduled && host.node_connect_requests == nodes + 3,
        "rearmed node retry is consumed once");
}

static void speech_confirmation(void)
{
    const char *speech = "{\"text\":\"Set to 23 degrees.\"}";
    CHECK(host_command_params(node_client, "talk.say", speech) == ESP_ERR_INVALID_STATE,
        "speech requires a connected call");
    connect_call();
    CHECK(host_command_params(node_client, "talk.say", "{}") == ESP_ERR_INVALID_ARG,
        "speech requires text");
    CHECK(host_command_params(node_client, "talk.say", "{\"text\":\"a\",\"text\":\"b\"}") == ESP_ERR_INVALID_ARG,
        "duplicate speech fields are rejected");
    CHECK(host_command_params(node_client, "talk.say", speech) == ESP_OK,
        "connected speech request queues");
    CHECK(strstr(host.speech_params, "\"voiceSessionId\":\"voice-a\"") != NULL &&
        strstr(host.speech_params, "\"sessionKey\":\"agent:fixture:main\"") != NULL,
        "speech uses the current call's frozen identity");
    CHECK(host_command_params(node_client, "talk.say", speech) == ESP_ERR_INVALID_STATE,
        "pending speech is bounded");
    host_reply_speech(false);
    drain();
    CHECK(host.ui == ROOM_UI_ERROR && talk_active, "Gateway rejection is visible without ending Talk");
    CHECK(host_command_params(node_client, "talk.say", speech) == ESP_OK, "speech can retry after rejection");
    stop();
    drain();
    connect_call();
    host_reply_speech(false);
    drain();
    CHECK(host.ui == ROOM_UI_SPEAKING && talk_active,
        "a late speech rejection cannot replace the next call's UI");
}

static void capture_source_scheduler(void)
{
    const struct { const char *model; int source_priority; } boards[] = {
        {"waveshare-esp32-s3-touch-amoled-1.8", 15},
        {"m5stack-tab5", 10},
        {NULL, 10},
    };
    for (size_t i = 0; i < sizeof(boards) / sizeof(boards[0]); ++i) {
        host.board_model_identifier = boards[i].model;
        esp_capture_thread_schedule_cfg_t source = {
            .priority = 10, .stack_size = 4096, .core_id = 0,
        };
        capture_scheduler("aud_src", &source);
        CHECK(source.priority == boards[i].source_priority,
            "only the watch's GMF source pipeline shares the audio decoder priority");
        CHECK(source.core_id == 0 && source.stack_size == 4096 && source.stack_in_ext,
            "source pipeline keeps the capture manager's core and stack");
        capture_scheduler("AUD_SRC", &source);
        CHECK(source.core_id == 1 && source.priority == 15 && source.stack_size == 40 * 1024,
            "the distinct raw capture reader keeps its existing schedule");
    }
}

static const char *agent_roster = "{\"agents\":[{\"id\":\"fixture\",\"name\":\"Fixture\"}]}";
static const char *target_roster = "{\"mainKey\":\"work\",\"agents\":[{\"id\":\"fixture\",\"name\":\"Fixture\"},{\"id\":\"builder\",\"name\":\"Builder\"}]}";

static room_agents_snapshot_t agents_snapshot(void)
{
    room_agents_snapshot_t snapshot;
    room_agent_directory_copy(&snapshot);
    return snapshot;
}

static void directory_ready(void)
{
    request_local_agents(true, 0);
    CHECK(host.agent_requests == 0, "display intent does not submit inline");
    host_run_task("talk_teardown");
    CHECK(host.agent_requests == 1, "one bounded request");
    host_emit_node(node_client, ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    host_reply_agents(true, agent_roster);
    host_run_task("talk_teardown");
    room_agents_snapshot_t snapshot = agents_snapshot();
    CHECK(snapshot.state == ROOM_AGENTS_READY && snapshot.page.count == 1 &&
        strcmp(snapshot.page.agents[0].id, "fixture") == 0, "operator roster survives primary node loss");
    CHECK(operator_ready, "directory does not reconnect operator");
    request_local_agents(false, 0);
    host_run_task("talk_teardown");
    CHECK(agents_snapshot().state == ROOM_AGENTS_IDLE, "leaving browser releases directory interest");
}

static unsigned load_target_roster(const char *json)
{
    room_agent_directory_refresh(0);
    host_run_task("talk_teardown");
    host_reply_agents(true, json);
    host_run_task("talk_teardown");
    return agents_snapshot().revision;
}

static const char *conversation_page = "{\"sessions\":[{\"key\":\"agent:fixture:thread:one\",\"label\":\"One\"}],\"offset\":0,\"hasMore\":false,\"nextOffset\":null}";
static const char *conversation_preview = "{\"previews\":[{\"key\":\"agent:fixture:thread:one\",\"status\":\"ok\",\"items\":[{\"role\":\"user\",\"text\":\"A question\"}]}]}";

static room_conversations_snapshot_t conversations_snapshot(void)
{
    room_conversations_snapshot_t snapshot;
    room_conversations_copy(&snapshot);
    return snapshot;
}

static void conversations_read(void)
{
    unsigned revision = load_target_roster(agent_roster);
    CHECK(room_conversations_open("fixture", revision + 1) == ESP_ERR_INVALID_STATE,
        "stale roster cannot open a reader");
    CHECK(room_conversations_open("unknown", revision) == ESP_ERR_INVALID_STATE,
        "only a roster member can be read");
    CHECK(room_conversations_open("fixture", revision) == ESP_OK, "read does not require a Talk main key");
    host_run_task("talk_teardown");
    CHECK(strcmp(host.reader_method, "sessions.list") == 0 &&
        strstr(host.reader_params, "\"agentId\":\"fixture\"") != NULL &&
        strstr(host.reader_params, "\"includeGlobal\":false") != NULL &&
        strstr(host.reader_params, "\"limit\":8") != NULL, "bounded per-agent read contract");
    host_reply_agents(true, conversation_page);
    host_run_task("talk_teardown");
    room_conversations_snapshot_t snapshot = conversations_snapshot();
    CHECK(snapshot.state == ROOM_AGENTS_READY && snapshot.page.count == 1, "session page published");
    CHECK(room_conversations_preview(0, snapshot.revision) == ESP_OK, "open exact page row");
    host_run_task("talk_teardown");
    CHECK(strcmp(host.reader_method, "sessions.preview") == 0 &&
        strstr(host.reader_params, "agent:fixture:thread:one") != NULL &&
        strstr(host.reader_params, "\"limit\":4") != NULL, "preview carries exact returned key");
    host_reply_agents(true, conversation_preview);
    host_run_task("talk_teardown");
    snapshot = conversations_snapshot();
    CHECK(snapshot.preview_state == ROOM_AGENTS_READY && snapshot.preview.count == 1 &&
        strcmp(snapshot.preview.items[0].text, "A question") == 0, "preview published");
    CHECK(!talk_target.selected && host.config_requests == 0 && host.create_requests == 0,
        "read does not select or start Talk");
    room_conversations_close();
    snapshot = conversations_snapshot();
    CHECK(snapshot.state == ROOM_AGENTS_IDLE && snapshot.page.count == 0 && snapshot.preview.count == 0,
        "close releases all reader content");
}

static void conversations_stale(void)
{
    unsigned revision = load_target_roster(target_roster);
    room_conversations_open("fixture", revision);
    host_run_task("talk_teardown");
    room_conversations_open("builder", revision);
    host_reply_agents(true, conversation_page);
    host_run_task("talk_teardown");
    room_conversations_snapshot_t snapshot = conversations_snapshot();
    CHECK(snapshot.state == ROOM_AGENTS_LOADING && snapshot.page.count == 0 &&
        strcmp(snapshot.agent.id, "builder") == 0, "late old agent page cannot populate a replacement");
    CHECK(strstr(host.reader_params, "\"agentId\":\"builder\"") != NULL, "replacement request follows old completion");
    room_conversations_close();
    host_reply_agents(true, "{\"sessions\":[],\"offset\":0,\"hasMore\":false,\"nextOffset\":null}");
    host_run_task("talk_teardown");
    CHECK(conversations_snapshot().state == ROOM_AGENTS_IDLE, "late page after Back stays closed");
    room_conversations_open("fixture", revision);
    host_run_task("talk_teardown");
    host_reply_agents(true, conversation_page);
    host_run_task("talk_teardown");
    snapshot = conversations_snapshot();
    room_conversations_preview(0, snapshot.revision);
    host_run_task("talk_teardown");
    room_agent_directory_refresh(0);
    host_reply_agents(true, conversation_preview);
    host_run_task("talk_teardown");
    CHECK(conversations_snapshot().preview.count == 0, "roster replacement clears outstanding preview");
    host_reply_agents(true, target_roster);
    host_run_task("talk_teardown");
    revision = agents_snapshot().revision;
    room_conversations_open("fixture", revision);
    host_run_task("talk_teardown");
    host_emit_node(operator_client, ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    host_reply_agents(true, conversation_page);
    host_run_task("talk_teardown");
    CHECK(conversations_snapshot().state == ROOM_AGENTS_OFFLINE && conversations_snapshot().page.count == 0,
        "lost owner cannot publish a stale page");
}

static void conversations_suspend(void)
{
    unsigned revision = load_target_roster(target_roster);
    CHECK(select_talk_agent("builder", revision) == ESP_OK, "choose the next Talk agent independently");
    CHECK(room_conversations_open("fixture", revision) == ESP_OK, "open reader before hiding it");
    host_run_task("talk_teardown");
    room_agent_directory_suspend();
    host_reply_agents(true, conversation_page);
    host_run_task("talk_teardown");

    room_agents_snapshot_t agents;
    room_conversations_snapshot_t reader;
    unsigned epoch;
    CHECK(room_conversations_copy_resume(&agents, &reader, &epoch) == ESP_OK &&
        agents.revision == revision && reader.state == ROOM_AGENTS_READY &&
        reader.page.count == 1 && strcmp(reader.agent.id, "fixture") == 0,
        "hidden completion retains the matching roster and conversation page");
    CHECK(room_conversations_resume_current(epoch), "coherent copy has live authority");
    room_conversation_context_t context;
    CHECK(room_conversations_copy_context(&context) == ESP_OK && context.epoch == epoch &&
        strcmp(context.agent_name, reader.agent.name) == 0,
        "Home context uses the same live reader and epoch without copying its messages");
    CHECK(room_conversations_preview(0, reader.revision) == ESP_OK, "retained page can open its exact row");
    CHECK(!room_conversations_resume_current(context.epoch), "Home context expires with reader selection");
    CHECK(!room_conversations_resume_current(epoch), "a changed selection rejects an earlier copy");
    host_run_task("talk_teardown");
    CHECK(room_conversations_copy_resume(&agents, &reader, &epoch) == ESP_OK &&
        reader.preview_state == ROOM_AGENTS_LOADING, "pending preview can resume");
    host_reply_agents(true, conversation_preview);
    host_run_task("talk_teardown");
    CHECK(!room_conversations_resume_current(epoch), "completion invalidates the pending preview copy");
    CHECK(room_conversations_copy_resume(&agents, &reader, &epoch) == ESP_OK &&
        reader.preview_state == ROOM_AGENTS_READY && reader.preview.count == 1,
        "hidden preview completion is available on return");
    CHECK(room_conversations_copy_context(&context) == ESP_OK && context.epoch == epoch,
        "Home can republish the completed hidden preview");
    unsigned requests = host.conversation_requests;
    host_run_task("talk_teardown");
    CHECK(host.conversation_requests == requests && host.agent_requests == 1,
        "suspend and resume copies do not start another RPC");
    CHECK(talk_target.selected && strcmp(talk_target.id, "builder") == 0 &&
        host.config_requests == 0 && host.create_requests == 0,
        "reader resume preserves the chosen Talk target without starting a call");
    room_conversations_close();
    CHECK(!room_conversations_resume_current(epoch) &&
        room_conversations_copy_resume(&agents, &reader, &epoch) == ESP_ERR_INVALID_STATE &&
        reader.agent.id[0] == '\0', "explicit close revokes retained content and copied authority");
    CHECK(room_conversations_copy_context(&context) == ESP_ERR_INVALID_STATE &&
        context.epoch == 0 && context.agent_name[0] == '\0', "explicit close clears the Home projection");
}

static void conversations_resume_invalidation(void)
{
    unsigned revision = load_target_roster(target_roster);
    room_conversations_open("fixture", revision);
    host_run_task("talk_teardown");
    host_reply_agents(true, conversation_page);
    host_run_task("talk_teardown");
    room_agent_directory_suspend();
    room_agents_snapshot_t agents;
    room_conversations_snapshot_t reader;
    unsigned epoch;
    CHECK(room_conversations_copy_resume(&agents, &reader, &epoch) == ESP_OK, "reader ready to resume");
    room_conversations_refresh(0);
    CHECK(!room_conversations_resume_current(epoch), "page refresh fences an old copied page");
    CHECK(room_conversations_copy_resume(&agents, &reader, &epoch) == ESP_OK, "copy refreshed reader");
    room_agent_directory_refresh(0);
    CHECK(!room_conversations_resume_current(epoch) &&
        room_conversations_copy_resume(&agents, &reader, &epoch) == ESP_ERR_INVALID_STATE,
        "refreshing the roster discards its reader");
    host_run_task("talk_teardown");
    host_reply_agents(true, target_roster);
    host_run_task("talk_teardown");
    revision = agents_snapshot().revision;
    room_conversations_open("fixture", revision);
    CHECK(room_conversations_copy_resume(&agents, &reader, &epoch) == ESP_OK, "copy replacement reader");
    room_agent_directory_cancel();
    CHECK(!room_conversations_resume_current(epoch) &&
        room_conversations_copy_resume(&agents, &reader, &epoch) == ESP_ERR_INVALID_STATE,
        "explicit directory cancel still releases content");
    revision = load_target_roster(target_roster);
    room_conversations_open("fixture", revision);
    CHECK(room_conversations_copy_resume(&agents, &reader, &epoch) == ESP_OK, "copy before owner loss");
    host_emit_node(operator_client, ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    CHECK(!room_conversations_resume_current(epoch) &&
        room_conversations_copy_resume(&agents, &reader, &epoch) == ESP_ERR_INVALID_STATE &&
        agents.page.count == 0 && reader.page.count == 0,
        "owner loss rejects a copy taken before the UI acquired its display lock");
    room_conversation_context_t context = {.epoch = epoch, .agent_name = "stale"};
    CHECK(room_conversations_copy_context(&context) == ESP_ERR_INVALID_STATE &&
        context.epoch == 0 && context.agent_name[0] == '\0', "owner loss clears the Home projection");
}

static void directory_suspend_pending(void)
{
    room_agent_directory_refresh(0);
    host_run_task("talk_teardown");
    room_agent_directory_refresh(1);
    room_agent_directory_suspend();
    host_reply_agents(true, target_roster);
    host_run_task("talk_teardown");
    CHECK(host.agent_requests == 1 && agents_snapshot().page.count == 0,
        "hiding drops queued roster work and rejects a late initial roster");
}

static void conversations_local_intent(void)
{
    unsigned revision = load_target_roster(target_roster);
    request_local_conversation(ROOM_CONVERSATIONS_OPEN, "fixture", revision, 0, 1);
    CHECK(host.conversation_requests == 0, "display callback only records intent");
    host_run_task("talk_teardown");
    CHECK(host.conversation_requests == 1 && strcmp(host.reader_method, "sessions.list") == 0,
        "supervisor owns the conversation request");
    request_local_conversation(ROOM_CONVERSATIONS_CLOSE, "", 0, 0, 2);
    request_local_conversation(ROOM_CONVERSATIONS_OPEN, "builder", revision, 0, 2);
    host_reply_agents(true, conversation_page);
    host_run_task("talk_teardown");
    room_conversations_snapshot_t snapshot = conversations_snapshot();
    CHECK(snapshot.state == ROOM_AGENTS_LOADING && strcmp(snapshot.agent.id, "builder") == 0,
        "latest open supersedes an earlier close and response");
    host_reply_agents(true, "{\"sessions\":[],\"offset\":0,\"hasMore\":false,\"nextOffset\":null}");
    host_run_task("talk_teardown");
    request_local_conversation(ROOM_CONVERSATIONS_CLOSE, "", 0, 0, 2);
    host_run_task("talk_teardown");
    CHECK(conversations_snapshot().state == ROOM_AGENTS_IDLE && !talk_target.selected,
        "leaving releases the reader without choosing a Talk agent");
}

static void conversations_timeout_talk(void)
{
    unsigned revision = load_target_roster(target_roster);
    CHECK(select_talk_agent("builder", revision) == ESP_OK, "select next Talk target");
    room_conversations_open("fixture", revision);
    host_run_task("talk_teardown");
    admit();
    host_run_task("talk_start");
    host_reply_create(true);
    host_emit_peer(webrtc, ESP_WEBRTC_EVENT_CONNECTED);
    host_run_task("talk_teardown");
    host.now_us += 16000000;
    host_fire_directory_timer();
    host_run_task("talk_teardown");
    CHECK(conversations_snapshot().state == ROOM_AGENTS_TIMEOUT && talk_active && operator_ready,
        "read timeout leaves shared Talk connection and call alive");
    CHECK(strcmp(talk_target.id, "builder") == 0, "read leaves selected target unchanged");
    room_conversations_refresh(0);
    host_run_task("talk_teardown");
    CHECK(host.conversation_requests == 1, "timeout cannot accumulate requests");
    host_reply_agents(true, conversation_page);
    host_run_task("talk_teardown");
    CHECK(host.conversation_requests == 2 && conversations_snapshot().state == ROOM_AGENTS_LOADING,
        "late completion releases the queued refresh without publishing expired content");
    host_reply_agents(true, conversation_page);
    host_run_task("talk_teardown");
    CHECK(conversations_snapshot().state == ROOM_AGENTS_READY && talk_active, "new page coexists with the original call");
    stop();
    drain();
    expect_closed();
    CHECK(strcmp(host.close_key, "agent:builder:work") == 0, "stop retains the admitted Talk key");
}

static void target_selection(void)
{
    unsigned revision = load_target_roster(target_roster);
    unsigned refreshes = host.ui_refreshes;
    request_local_agent_selection("builder", revision);
    CHECK(!talk_target.selected && host.selection_completions == 0, "display callback only records intent");
    host_run_task("talk_teardown");
    CHECK(talk_target.selected && host.selection_completions == 1 && host.selection_result == ESP_OK,
        "owner acknowledges a current agent before Talk admission");
    CHECK(host.ui_target.selected && strcmp(host.ui_target.id, "builder") == 0 && host.ui_refreshes > refreshes,
        "selection is published and repainted in the same supervisor turn");
    host_emit_node(node_client, ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    CHECK(!talk_target.expired, "primary node loss does not replace operator authority");
    admit();
    CHECK(select_talk_agent("fixture", revision) == ESP_OK, "choose next agent after admission");
    request_local_agents(false, 0);
    host_run_task("talk_teardown");
    host_run_task("talk_start");
    CHECK(host.config_requests == 0 && strcmp(host.create_key, "agent:builder:work") == 0,
        "admission freezes explicit target and bypasses default discovery");
    host_reply_create(true);
    host_emit_peer(webrtc, ESP_WEBRTC_EVENT_CONNECTED);
    host_run_task("talk_teardown");
    CHECK(talk_active, "changing next target leaves current call active");
    host_command(node_client, "talk.stop");
    host_run_task("talk_teardown");
    CHECK(strcmp(host.close_key, "agent:builder:work") == 0, "close retains admitted owner");
    admit();
    host_run_task("talk_start");
    CHECK(strcmp(host.create_key, "agent:fixture:work") == 0 && host.config_requests == 0,
        "next choice survives closing the directory and previous call");
    host_reply_create(true);
    host_emit_peer(webrtc, ESP_WEBRTC_EVENT_CONNECTED);
}

static void target_stale(void)
{
    unsigned revision = load_target_roster(target_roster);
    CHECK(select_talk_agent("builder", revision) == ESP_OK, "select before reconnect");
    host_emit_node(operator_client, ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    host_emit_node(operator_client, ESP_OPENCLAW_NODE_EVENT_CONNECTED);
    CHECK(select_talk_agent("fixture", revision) == ESP_ERR_INVALID_STATE, "stale roster cannot retarget");
    room_agent_directory_cancel();
    CHECK(host_command_params(node_client, "talk.start", "{}") == ESP_ERR_INVALID_STATE && host.opens == 0,
        "expired choice cannot silently fall back to Gateway default");
    char *args[] = {"agents", "default"};
    CHECK(agents_console_command(2, args) == 0, "explicit reset chooses Gateway default");
    connect_call();
}

static void target_default(void)
{
    unsigned revision = load_target_roster(target_roster);
    CHECK(select_talk_agent("builder", revision) == ESP_OK, "select explicit agent");
    admit();
    host_run_task("talk_start");
    host_reply_create(true);
    host_emit_peer(webrtc, ESP_WEBRTC_EVENT_CONNECTED);
    host_run_task("talk_teardown");
    unsigned refreshes = host.ui_refreshes;
    request_local_agent_selection("", 0);
    CHECK(talk_target.selected, "default request waits for the owner");
    host_run_task("talk_teardown");
    CHECK(!talk_target.selected && !host.ui_target.selected && host.selection_result == ESP_OK &&
        host.ui_refreshes > refreshes, "owner acknowledges, clears and publishes default routing");
    CHECK(talk_active && strcmp(host.create_key, "agent:builder:work") == 0,
        "default selection leaves the current call with its admitted agent");
    host_command(node_client, "talk.stop");
    host_run_task("talk_teardown");
    CHECK(strcmp(host.close_key, "agent:builder:work") == 0, "close preserves the original agent");
    admit();
    host_run_task("talk_start");
    CHECK(host.config_requests == 1, "the next call asks the Gateway for its default");
    host_reply_config();
    CHECK(strcmp(host.create_key, "agent:fixture:main") == 0, "the next call uses Gateway default routing");
    host_reply_create(true);
    host_emit_peer(webrtc, ESP_WEBRTC_EVENT_CONNECTED);
}

static void target_metadata(void)
{
    unsigned revision = load_target_roster(agent_roster);
    CHECK(select_talk_agent("fixture", revision) == ESP_ERR_INVALID_STATE && !talk_target.selected,
        "missing mainKey cannot create a guessed target");
    revision = load_target_roster(target_roster);
    CHECK(select_talk_agent("missing", revision) == ESP_ERR_NOT_FOUND && !talk_target.selected,
        "target must belong to the visible roster");
}

static void target_origin(void)
{
    unsigned revision = load_target_roster(target_roster);
    CHECK(select_talk_agent("builder", revision) == ESP_OK, "select before Gateway change");
    host.node_uri = "wss://gateway.example/changed-path";
    host_emit_node(node_client, ESP_OPENCLAW_NODE_EVENT_CONNECTED);
    CHECK(talk_target.expired && !operator_ready, "full Gateway URI change revokes choice");
}

static void directory_reconnect(void)
{
    room_agent_directory_refresh(0);
    host_run_task("talk_teardown");
    host_emit_node(operator_client, ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
    host_emit_node(operator_client, ESP_OPENCLAW_NODE_EVENT_CONNECTED);
    host_reply_agents(true, agent_roster);
    host_run_task("talk_teardown");
    CHECK(agents_snapshot().state == ROOM_AGENTS_LOADING && host.agent_requests == 2,
        "old response cannot populate same-handle replacement connection");
    host_reply_agents(true, "{\"agents\":[]}");
    host_run_task("talk_teardown");
    CHECK(agents_snapshot().state == ROOM_AGENTS_EMPTY, "fresh response populates replacement");
}

static void directory_gateway_rebind(void)
{
    room_agent_directory_refresh(0);
    host_run_task("talk_teardown");
    host.node_uri = "wss://gateway.example/replacement";
    host_emit_node(node_client, ESP_OPENCLAW_NODE_EVENT_CONNECTED);
    CHECK(agents_snapshot().state == ROOM_AGENTS_OFFLINE, "full URI change immediately revokes directory");
    host_reply_agents(true, agent_roster);
    host_run_task("talk_teardown");
    CHECK(agents_snapshot().state == ROOM_AGENTS_OFFLINE && host.agent_requests == 1,
        "late old Gateway response cannot repopulate roster");
}

static void directory_superseded(void)
{
    room_agent_directory_refresh(0);
    host_run_task("talk_teardown");
    room_agent_directory_refresh(1);
    host_run_task("talk_teardown");
    CHECK(host.agent_requests == 1, "refresh cannot accumulate pending RPCs");
    host_reply_agents(true, agent_roster);
    host_run_task("talk_teardown");
    CHECK(host.agent_requests == 2 && agents_snapshot().state == ROOM_AGENTS_LOADING,
        "old response is discarded before latest offset starts");
    host_reply_agents(true, agent_roster);
    host_run_task("talk_teardown");
    room_agents_snapshot_t snapshot = agents_snapshot();
    CHECK(snapshot.state == ROOM_AGENTS_EMPTY && snapshot.page.offset == 1 && snapshot.page.total == 1,
        "latest requested page wins");
}

static void directory_timeout_during_talk(void)
{
    connect_call();
    room_agent_directory_refresh(0);
    host_run_task("talk_teardown");
    host.now_us += 15000000;
    host_fire_directory_timer();
    host_run_task("talk_teardown");
    CHECK(agents_snapshot().state == ROOM_AGENTS_TIMEOUT, "deadline has visible outcome");
    CHECK(talk_active && operator_ready && host.closes == 0, "directory deadline leaves Talk live");
    room_agent_directory_refresh(0);
    host_run_task("talk_teardown");
    CHECK(host.agent_requests == 1, "timeout retains context without duplicate RPC");
    host_reply_agents(true, agent_roster);
    host_run_task("talk_teardown");
    CHECK(host.agent_requests == 2, "late completion releases pending refresh");
    host_reply_agents(true, agent_roster);
    host_run_task("talk_teardown");
    CHECK(agents_snapshot().state == ROOM_AGENTS_READY && talk_active, "fresh roster keeps original Talk owner");
}

static void directory_cancel(void)
{
    room_agent_directory_refresh(0);
    host_run_task("talk_teardown");
    host_reply_agents(true, agent_roster);
    host_run_task("talk_teardown");
    CHECK(host.agents_state == ROOM_AGENTS_READY, "ready roster reaches display");
    room_agent_directory_cancel();
    host_run_task("talk_teardown");
    CHECK(host.agents_state == ROOM_AGENTS_IDLE, "cancel publishes paused state without another event");
    room_agent_directory_refresh(0);
    host_run_task("talk_teardown");
    room_agent_directory_cancel();
    host_reply_agents(true, agent_roster);
    host_run_task("talk_teardown");
    CHECK(agents_snapshot().state == ROOM_AGENTS_IDLE && host.agent_requests == 2 && operator_ready,
        "leaving directory discards late reply without closing shared connection");
}

static void directory_deferred_start(void)
{
    start_pending_config();
    room_agent_directory_refresh(0);
    host_run_task("talk_teardown");
    CHECK(host.agent_requests == 0, "Talk startup has priority over directory submission");
    host_reply_config();
    host_reply_create(true);
    host_emit_peer(webrtc, ESP_WEBRTC_EVENT_CONNECTED);
    host_run_task("talk_teardown");
    CHECK(host.agent_requests == 1 && talk_active, "directory can load alongside established Talk");
    host.now_us += 15000000;
    host_reply_agents(true, agent_roster);
    host_run_task("talk_teardown");
    CHECK(agents_snapshot().state == ROOM_AGENTS_TIMEOUT && talk_active,
        "late success cannot bypass the deadline before its timer is consumed");
}

static void directory_errors(void)
{
    host.fail_agent_submit = true;
    room_agent_directory_refresh(0);
    host_run_task("talk_teardown");
    CHECK(agents_snapshot().state == ROOM_AGENTS_ERROR, "immediate submission failure completes");
    host.fail_agent_submit = false;
    const char *payloads[] = {"{\"agents\":[{\"id\":\"invalid id\"}]}", NULL};
    for (unsigned i = 0; i < 2; ++i) {
        room_agent_directory_refresh(0);
        host_run_task("talk_teardown");
        host_reply_agents(i == 0, payloads[i]);
        host_run_task("talk_teardown");
        CHECK(agents_snapshot().state == ROOM_AGENTS_ERROR, "malformed or denied response stays empty");
    }
    char *large = malloc(ESP_OPENCLAW_DIRECTORY_MAX_JSON_BYTES + 2);
    host_require(large != NULL, "oversized response fixture allocation");
    memset(large, ' ', ESP_OPENCLAW_DIRECTORY_MAX_JSON_BYTES + 1);
    large[ESP_OPENCLAW_DIRECTORY_MAX_JSON_BYTES + 1] = '\0';
    room_agent_directory_refresh(0);
    host_run_task("talk_teardown");
    host_reply_agents(true, large);
    free(large);
    host_run_task("talk_teardown");
    CHECK(agents_snapshot().state == ROOM_AGENTS_TOO_LARGE, "oversized response is bounded");
}

static const struct { const char *name; void (*run)(void); } cases[] = {
    {"conversations-suspend", conversations_suspend},
    {"conversations-resume-invalidation", conversations_resume_invalidation},
    {"directory-suspend-pending", directory_suspend_pending},
    {"conversations-local-intent", conversations_local_intent},
    {"conversations-read", conversations_read},
    {"conversations-stale", conversations_stale},
    {"conversations-timeout-talk", conversations_timeout_talk},
    {"directory-ready", directory_ready},
    {"target-selection", target_selection},
    {"target-stale", target_stale},
    {"target-default", target_default},
    {"target-metadata", target_metadata},
    {"target-origin", target_origin},
    {"directory-reconnect", directory_reconnect},
    {"directory-gateway-rebind", directory_gateway_rebind},
    {"directory-superseded", directory_superseded},
    {"directory-timeout-during-talk", directory_timeout_during_talk},
    {"directory-cancel", directory_cancel},
    {"directory-deferred-start", directory_deferred_start},
    {"directory-errors", directory_errors},
    {"capture-source-scheduler", capture_source_scheduler},
    {"speech-confirmation", speech_confirmation},
    {"operator-scopes", operator_scopes},
    {"voice-actions-provision", voice_actions_provision},
    {"voice-actions-stale", voice_actions_stale},
    {"console-diagnostics-bytes", console_diagnostics_bytes},
    {"console-diagnostics-contention", console_diagnostics_contention},
    {"sdk-log-policy-info", sdk_log_policy_info},
    {"sdk-log-policy-warn", sdk_log_policy_warn},
    {"sdk-log-policy-error", sdk_log_policy_error},
    {"sdk-log-policy-failure", sdk_log_policy_failure},
    {"diagnostic-boundaries", diagnostic_boundaries},
    {"home-connection-facts", home_connection_facts},
    {"operator-reconnect-without-task-memory", operator_reconnect_without_task_memory},
    {"node-reconnect-without-task-memory", node_reconnect_without_task_memory},
    {"coalesced-connection-retries", coalesced_connection_retries},
    {"late-create-replacement", late_create_replacement},
    {"wake-admission-loss", wake_admission_loss},
    {"local-talk-control", local_talk_control},
    {"local-talk-readiness-loss", local_talk_readiness_loss},
    {"loss-before-worker", loss_before_worker},
    {"loss-at-media-gate", loss_at_media_gate},
    {"loss-config-pending", loss_config_pending},
    {"loss-create-pending", loss_create_pending},
    {"loss-inside-start", loss_inside_start},
    {"displaced-owner-loss", displaced_owner_loss},
    {"origin-rebind", origin_rebind},
    {"malformed-events", malformed_events},
    {"stop-coalescing", stop_coalescing},
    {"replacement-ordering", replacement_ordering},
    {"immediate-config-failure", immediate_config_failure},
    {"immediate-create-failure", immediate_create_failure},
    {"open-failure", open_failure},
    {"provider-failure", provider_failure},
    {"start-failure", start_failure},
    {"timer-failure", timer_failure},
    {"timeout-stop", timeout_stop},
    {"operator-disconnect", operator_disconnect},
    {"gateway-session-closed", terminal_event},
    {"terminal-before-identity", terminal_before_identity},
    {"node-disconnect", node_disconnect},
    {"unrelated-voice", unrelated_voice},
    {"recoverable-error", recoverable_error},
    {"stale-operator", stale_operator},
    {"explicit-stop", explicit_stop},
    {"duplicate-media-terminal", duplicate_media_terminal},
    {"duplicate-gateway-after-stop", duplicate_gateway_after_stop},
    {"cancel-before-worker", cancel_before_worker},
    {"cancel-at-media-gate", cancel_at_media_gate},
    {"cancel-config-pending", cancel_config_pending},
    {"cancel-create-pending", cancel_create_pending},
    {"cancel-inside-start", cancel_inside_start},
    {"cancel-before-signaling-start", cancel_before_signaling_start},
    {"setup-failure", setup_failure},
};

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) puts(cases[i].name);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--case") == 0) {
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            if (strcmp(argv[2], cases[i].name) != 0) continue;
            bootstrap();
            cases[i].run();
            /* Cleanup follows assertions; it cannot turn the measured red
             * teardown result into a pass. Also prove identity after red cases. */
            if (webrtc != NULL) {
                puts("CLEANUP: explicit talk.stop after measured assertions");
                stop();
                drain();
                if (host.opens == 1 && host.close_requests != 0) expect_voice_cleanup();
            }
            free(gateway_http_base);
            gateway_http_base = NULL;
            room_agent_directory_cancel();
            host_release_resources();
            printf("%s: %s (%u failed assertions)\n", cases[i].name, failures ? "FAIL" : "PASS", failures);
            return failures ? 1 : 0;
        }
    }
    fputs("Use --list or --case NAME\n", stderr);
    return 2;
}
