/* Real adapter, real pthread locks and barriers; RPC and HTTP are synthetic.
 * Internal counters are observed only to prove the drain actually blocked. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/esp_openclaw_talk.c"

_Thread_local unsigned talk_host_critical_depth;

static struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool reached, released;
} gate = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, false, false};

enum pause_point { NONE, ICE, CONNECTED, FAILURE, ANSWER, HTTP, REPLY };
static enum pause_point pause_at;
static atomic_uint ice_count, connected_count, failure_count, answer_count, close_count, terminal_count;
static atomic_bool closed;
static bool reject_submission;
static bool reply_ok = true;
static bool use_config;
static unsigned config_count, create_count;
static char close_key[257], close_voice[129];
static esp_openclaw_node_handle_t close_node;
static struct {
    esp_openclaw_node_gateway_request_cb_t cb;
    esp_openclaw_node_handle_t node;
    void *ctx;
} pending;
static esp_openclaw_talk_call_handle_t call;
static esp_peer_signaling_handle_t signaling;
static const esp_peer_signaling_impl_t *implementation;
struct callback_context { unsigned magic; };
static struct callback_context *callback_context;

static void pause_here(enum pause_point point)
{
    if (pause_at != point) return;
    assert(pthread_mutex_lock(&gate.mutex) == 0);
    gate.reached = true;
    assert(pthread_cond_broadcast(&gate.changed) == 0);
    while (!gate.released) assert(pthread_cond_wait(&gate.changed, &gate.mutex) == 0);
    assert(pthread_mutex_unlock(&gate.mutex) == 0);
}
static void wait_for_pause(void)
{
    assert(pthread_mutex_lock(&gate.mutex) == 0);
    while (!gate.reached) assert(pthread_cond_wait(&gate.changed, &gate.mutex) == 0);
    assert(pthread_mutex_unlock(&gate.mutex) == 0);
}
static void resume(void)
{
    assert(pthread_mutex_lock(&gate.mutex) == 0);
    gate.released = true;
    assert(pthread_cond_broadcast(&gate.changed) == 0);
    assert(pthread_mutex_unlock(&gate.mutex) == 0);
}
static void touch_context(void *ctx)
{
    assert(talk_host_critical_depth == 0);
    assert(!atomic_load(&closed));
    assert(((struct callback_context *)ctx)->magic == 0xcafef00d);
}
static int on_ice(esp_peer_signaling_ice_info_t *info, void *ctx)
{
    assert(info->is_initiator);
    atomic_fetch_add(&ice_count, 1);
    pause_here(ICE);
    touch_context(ctx);
    return 0;
}
static int on_connected(void *ctx)
{
    atomic_fetch_add(&connected_count, 1);
    pause_here(CONNECTED);
    touch_context(ctx);
    return 0;
}
static int on_close(void *ctx)
{
    touch_context(ctx);
    atomic_fetch_add(&close_count, 1);
    return 0;
}
static int on_answer(esp_peer_signaling_msg_t *msg, void *ctx)
{
    assert(msg->size == 3 && memcmp(msg->data, "v=0", 3) == 0);
    atomic_fetch_add(&answer_count, 1);
    pause_here(ANSWER);
    touch_context(ctx);
    return 0;
}
static void on_failure(esp_openclaw_talk_setup_result_t result, void *ctx)
{
    assert(result == ESP_OPENCLAW_TALK_SETUP_FAILED);
    atomic_fetch_add(&failure_count, 1);
    pause_here(FAILURE);
    touch_context(ctx);
}
static void on_terminal(void *ctx)
{
    touch_context(ctx);
    atomic_fetch_add(&terminal_count, 1);
}

void talk_host_log_error(const char *tag, const char *format, ...)
{ (void)tag; (void)format; }
const char *esp_err_to_name(esp_err_t error)
{ (void)error; return "synthetic failure"; }

esp_err_t esp_openclaw_node_gateway_request(esp_openclaw_node_handle_t node,
    const char *method, const char *params, esp_openclaw_node_gateway_request_cb_t cb, void *ctx)
{
    assert(talk_host_critical_depth == 0);
    if (reject_submission) return ESP_FAIL;
    if (strcmp(method, "talk.client.close") == 0) {
        cJSON *json = cJSON_Parse(params);
        cJSON *key = cJSON_GetObjectItemCaseSensitive(json, "sessionKey");
        cJSON *voice = cJSON_GetObjectItemCaseSensitive(json, "voiceSessionId");
        assert(cJSON_IsString(key) && cJSON_IsString(voice));
        snprintf(close_key, sizeof(close_key), "%s", key->valuestring);
        snprintf(close_voice, sizeof(close_voice), "%s", voice->valuestring);
        close_node = node;
        cJSON_Delete(json);
        return ESP_OK;
    }
    if (strcmp(method, "talk.config") == 0) ++config_count;
    else { assert(strcmp(method, "talk.client.create") == 0); ++create_count; }
    assert(pending.cb == NULL);
    pending.cb = cb;
    pending.ctx = ctx;
    pending.node = node;
    return ESP_OK;
}

static void *reply_thread(void *arg)
{
    (void)arg;
    pause_here(REPLY);
    esp_openclaw_node_gateway_request_cb_t cb = pending.cb;
    void *ctx = pending.ctx;
    esp_openclaw_node_handle_t node = pending.node;
    pending.cb = NULL;
    const esp_openclaw_node_gateway_result_t result = {
        .ok = reply_ok,
        .error_code = reply_ok ? NULL : "UNAVAILABLE",
        .payload_json = use_config ? "{\"config\":{\"talk\":{\"agentId\":\"fixture\"}}}" :
            "{\"transport\":\"webrtc\",\"offerUrl\":\"/offer\",\"clientSecret\":\"synthetic\","
            "\"voiceSessionId\":\"voice-a\",\"clientControl\":{\"owner\":\"gateway\"}}",
    };
    assert(cb != NULL);
    cb(node, &result, ctx);
    return NULL;
}
static void *sdp_thread(void *arg)
{
    (void)arg;
    esp_peer_signaling_msg_t msg = {.type = ESP_PEER_SIGNALING_MSG_SDP, .data = (uint8_t *)"v=0", .size = 3};
    assert(implementation->send_msg(signaling, &msg) == ESP_PEER_ERR_NONE);
    return NULL;
}
static void *close_thread(void *arg)
{
    (void)arg;
    esp_openclaw_talk_call_quiesce(call);
    /* Emulate the pinned SDK ordering: peer/app context dies BEFORE stop. */
    atomic_store(&closed, true);
    free(callback_context);
    if (signaling != NULL) assert(implementation->stop(signaling) == ESP_PEER_ERR_NONE);
    esp_openclaw_talk_call_release(call);
    return NULL;
}
static void wait_for_drain(void)
{
    SemaphoreHandle_t sem = call->drained;
    assert(pthread_mutex_lock(&sem->mutex) == 0);
    while (sem->waiters == 0) assert(pthread_cond_wait(&sem->changed, &sem->mutex) == 0);
    assert(!atomic_load(&closed));
    assert(pthread_mutex_unlock(&sem->mutex) == 0);
}

static void prepare(bool automatic)
{
    callback_context = malloc(sizeof(*callback_context));
    assert(callback_context != NULL);
    callback_context->magic = 0xcafef00d;
    const esp_openclaw_talk_signaling_config_t config = {
        .operator_node = (esp_openclaw_node_handle_t)1,
        .gateway_http_base_url = "https://gateway.example",
        .session_key = automatic ? NULL : "agent:fixture:main",
        .setup_failed_cb = on_failure,
        .setup_failed_ctx = callback_context,
    };
    assert(esp_openclaw_talk_call_prepare(&config, &call) == ESP_OK);
    assert(esp_openclaw_talk_call_set_closed_handler(call, on_terminal, callback_context) == ESP_OK);
    implementation = esp_openclaw_talk_call_signaling_impl();
}
static int start(void)
{
    esp_peer_signaling_cfg_t cfg = {
        .extra_cfg = &call, .extra_size = sizeof(call),
        .on_ice_info = on_ice, .on_connected = on_connected, .on_msg = on_answer,
        .on_close = on_close, .ctx = callback_context,
    };
    return implementation->start(&cfg, &signaling);
}
static void race_dispatch(enum pause_point point)
{
    prepare(false);
    assert(start() == ESP_PEER_ERR_NONE);
    if (point == ANSWER || point == HTTP) reply_thread(NULL);
    if (point == FAILURE) reply_ok = false;
    pause_at = point;
    pthread_t dispatch, teardown;
    assert(pthread_create(&dispatch, NULL, point == ANSWER || point == HTTP ? sdp_thread : reply_thread, NULL) == 0);
    wait_for_pause();
    esp_openclaw_talk_call_cancel(call);
    assert(pthread_create(&teardown, NULL, close_thread, NULL) == 0);
    if (point == HTTP || point == REPLY) {
        /* Remote completion is paused, yet close must finish and free context. */
        assert(pthread_join(teardown, NULL) == 0);
        assert(atomic_load(&closed));
        resume();
    } else {
        wait_for_drain();
        resume();
        assert(pthread_join(teardown, NULL) == 0);
    }
    assert(pthread_join(dispatch, NULL) == 0);
    assert(atomic_load(&close_count) == 0);
    if (point == ICE || point == FAILURE || point == REPLY) assert(atomic_load(&connected_count) == 0);
    if (point == HTTP) assert(atomic_load(&answer_count) == 0);
    if (point == REPLY && reply_ok) {
        assert(atomic_load(&ice_count) == 0);
        assert(strcmp(close_voice, "voice-a") == 0);
        assert(strcmp(close_key, "agent:fixture:main") == 0);
        assert(close_node == (esp_openclaw_node_handle_t)1);
    }
}
static void config_after_close(void)
{
    use_config = true;
    prepare(true);
    assert(start() == ESP_PEER_ERR_NONE);
    pause_at = REPLY;
    pthread_t rpc;
    assert(pthread_create(&rpc, NULL, reply_thread, NULL) == 0);
    wait_for_pause();
    close_thread(NULL);
    resume();
    assert(pthread_join(rpc, NULL) == 0);
    assert(config_count == 1 && create_count == 0);
    assert(atomic_load(&ice_count) == 0 && atomic_load(&failure_count) == 0);
}
static void late_failure(void)
{
    reply_ok = false;
    race_dispatch(REPLY);
}
static void submission_failure(void)
{
    prepare(false);
    reject_submission = true;
    assert(start() == ESP_PEER_ERR_FAIL);
    assert(signaling == NULL && call->refs == 1);
    close_thread(NULL);
}
static void canceled_start(void)
{
    prepare(false);
    esp_openclaw_talk_call_cancel(call);
    assert(start() == ESP_PEER_ERR_FAIL);
    assert(signaling == NULL && pending.cb == NULL && call->refs == 1);
    close_thread(NULL);
}
static void lifecycle_isolation(void)
{
    prepare(false);
    esp_openclaw_talk_call_handle_t other;
    const esp_openclaw_talk_signaling_config_t config = {.operator_node = (esp_openclaw_node_handle_t)2};
    assert(esp_openclaw_talk_call_prepare(&config, &other) == ESP_OK);
    assert(esp_openclaw_talk_call_set_closed_handler(other, NULL, NULL) == ESP_OK);
    assert(start() == ESP_PEER_ERR_NONE);
    assert(esp_openclaw_talk_call_set_closed_handler(call, NULL, NULL) == ESP_ERR_INVALID_STATE);
    reply_thread(NULL);
    const char *event = "{\"voiceSessionId\":\"voice-a\",\"talkEvent\":{\"sessionId\":\"voice-a\",\"type\":\"session.closed\"}}";
    esp_openclaw_talk_call_gateway_event(other, (esp_openclaw_node_handle_t)1, "talk.event", event);
    assert(atomic_load(&terminal_count) == 0);
    esp_openclaw_talk_call_gateway_event(call, (esp_openclaw_node_handle_t)1, "talk.event", event);
    esp_openclaw_talk_call_gateway_event(call, (esp_openclaw_node_handle_t)1, "talk.event", event);
    assert(atomic_load(&terminal_count) == 1);
    close_thread(NULL);
    esp_openclaw_talk_call_quiesce(other);
    esp_openclaw_talk_call_release(other);
}

static void *racing_close_thread(void *arg)
{
    pause_here(REPLY);
    return close_thread(arg);
}

static void admission_race(void)
{
    for (unsigned i = 0; i < 200; ++i) {
        gate.reached = false;
        gate.released = false;
        signaling = NULL;
        atomic_store(&closed, false);
        atomic_store(&ice_count, 0);
        atomic_store(&connected_count, 0);
        atomic_store(&failure_count, 0);
        atomic_store(&close_count, 0);
        reply_ok = i % 2 == 0;
        prepare(false);
        assert(start() == ESP_PEER_ERR_NONE);
        pause_at = REPLY;
        pthread_t rpc, teardown;
        assert(pthread_create(&rpc, NULL, reply_thread, NULL) == 0);
        assert(pthread_create(&teardown, NULL, racing_close_thread, NULL) == 0);
        wait_for_pause();
        resume();
        assert(pthread_join(rpc, NULL) == 0);
        assert(pthread_join(teardown, NULL) == 0);
        assert(atomic_load(&ice_count) <= 1 && atomic_load(&failure_count) <= 1);
        assert(atomic_load(&connected_count) <= atomic_load(&ice_count));
    }
}

/* The actual HTTP API contract, synthetic response only. */
struct esp_http_client { esp_http_client_config_t config; };
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    esp_http_client_handle_t client = malloc(sizeof(*client));
    assert(client != NULL);
    client->config = *config;
    return client;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value)
{ (void)client; (void)key; (void)value; return ESP_OK; }
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *data, int length)
{ (void)client; (void)data; (void)length; return ESP_OK; }
esp_err_t esp_http_client_perform(esp_http_client_handle_t client)
{
    pause_here(HTTP);
    esp_http_client_event_t event = {
        .event_id = HTTP_EVENT_ON_DATA, .user_data = client->config.user_data, .data = "v=0", .data_len = 3,
    };
    return client->config.event_handler(&event);
}
int esp_http_client_get_status_code(esp_http_client_handle_t client)
{ (void)client; return 200; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{ free(client); return ESP_OK; }

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "ice-drain") == 0) race_dispatch(ICE);
    else if (strcmp(argv[1], "connected-drain") == 0) race_dispatch(CONNECTED);
    else if (strcmp(argv[1], "failure-drain") == 0) race_dispatch(FAILURE);
    else if (strcmp(argv[1], "answer-drain") == 0) race_dispatch(ANSWER);
    else if (strcmp(argv[1], "late-http") == 0) race_dispatch(HTTP);
    else if (strcmp(argv[1], "late-create") == 0) race_dispatch(REPLY);
    else if (strcmp(argv[1], "late-config") == 0) config_after_close();
    else if (strcmp(argv[1], "late-failure") == 0) late_failure();
    else if (strcmp(argv[1], "submission-failure") == 0) submission_failure();
    else if (strcmp(argv[1], "canceled-start") == 0) canceled_start();
    else if (strcmp(argv[1], "lifecycle-isolation") == 0) lifecycle_isolation();
    else if (strcmp(argv[1], "admission-race") == 0) admission_race();
    else abort();
    printf("%s: PASS\n", argv[1]);
    return 0;
}
