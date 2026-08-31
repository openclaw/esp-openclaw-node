/* Compile the actual controller unchanged; statics are visible only to fixture
 * bootstrapping and observations. All call admission/event ingress uses the
 * registered production handlers. Talk is a separate real translation unit. */
#include "host/room_host_fakes.h"
#include "../esp_openclaw_room_node.c"

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
    host_require(xTaskCreate(talk_start_worker_task, "talk_start", 8192, NULL, 7, &talk_start_worker) == pdPASS,
        "start worker registered");
    host_require(xTaskCreate(talk_teardown_task, "talk_teardown", 4096, NULL, 6, NULL) == pdPASS,
        "teardown worker registered");
    media_ready = true;
    host_require(start_node_client() == ESP_OK, "real node command registration");
    host_emit_node(node_client, ESP_OPENCLAW_NODE_EVENT_CONNECTED);
    host_fire_operator_timer(operator_start_timer);
    host_run_task("operator_start");
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
    host_reply_create(true);
    host_require(host.create_requests == host.opens && host.ice_callbacks == host.opens && host.signaling_connected == host.opens,
        "real Talk accepted Gateway-owned voice-a");
    host_emit_peer(webrtc, ESP_WEBRTC_EVENT_CONNECTED);
    host_run_task("talk_teardown");
    host_require(talk_active && !talk_dialing && host.media_owned && !host.ambient && host.ui == ROOM_UI_SPEAKING,
        "connected media precondition");
}
static void drain(void) { host_run_task("talk_teardown"); }
static void stop(void) { host_command(node_client, "talk.stop"); }

static void expect_open(void)
{
    CHECK(host.closes == 0, "unrelated event must not close WebRTC");
    CHECK(host.media_ends == 0 && host.media_owned && !host.ambient, "unrelated event retains media ownership");
    CHECK(webrtc != NULL && talk_active, "unrelated event retains active call");
    CHECK(host.ui == ROOM_UI_SPEAKING, "unrelated event retains speaking UI");
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
    request_talk_teardown(old_generation, "old timeout");
    host_emit_gateway(operator_client, "talk.event", closed_a);
    wake_talk_teardown(); /* An old wake token is never a teardown action. */
    drain();
    CHECK(host.closes == 1 && talk_active && host.ui == ROOM_UI_SPEAKING && !talk_cancel_requested,
        "old callbacks, timeout, terminal and wakeup cannot change replacement");
    stop();
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
    host.fail_open = true;
    admit();
    host_run_task("talk_start");
    drain();
    CHECK(host.closes == 0 && host.media_ends == 1 && talk_call == NULL, "open failure releases admitted media once");
}
static void provider_failure(void)
{
    host.fail_provider = true;
    admit();
    host_run_task("talk_start");
    drain();
    CHECK(host.closes == 1 && host.media_ends == 1 && host.ambient_restores == 0 && talk_call == NULL,
        "pre-start failure closes with capture still ambient");
}
static void start_failure(void)
{
    host.fail_start = true;
    admit();
    host_run_task("talk_start");
    drain();
    expect_closed();
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
    CHECK(talk_cancel_requested && host.closes == 0, "timeout requests canonical deferred stop");
    drain();
    expect_closed();
}

static const struct { const char *name; void (*run)(void); } cases[] = {
    {"late-create-replacement", late_create_replacement},
    {"wake-admission-loss", wake_admission_loss},
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
            host_release_resources();
            printf("%s: %s (%u failed assertions)\n", cases[i].name, failures ? "FAIL" : "PASS", failures);
            return failures ? 1 : 0;
        }
    }
    fputs("Use --list or --case NAME\n", stderr);
    return 2;
}
