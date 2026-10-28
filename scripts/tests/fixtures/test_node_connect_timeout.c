#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int esp_err_t;
typedef void *esp_websocket_client_handle_t;
enum {
    ESP_OK, ESP_FAIL, ESP_ERR_INVALID_STATE, ESP_ERR_NO_MEM, ESP_ERR_TIMEOUT,
    ESP_OPENCLAW_NODE_INTERNAL_IDLE, ESP_OPENCLAW_NODE_INTERNAL_CONNECTING,
    ESP_OPENCLAW_NODE_INTERNAL_READY, ESP_OPENCLAW_NODE_INTERNAL_DESTROYING,
    ESP_OPENCLAW_NODE_INTERNAL_CLOSED,
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_NONE,
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SAVED_SESSION,
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_BOOTSTRAP_TOKEN,
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_DEVICE_TOKEN,
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SHARED_TOKEN,
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_PASSWORD,
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_NO_AUTH,
    ESP_OPENCLAW_NODE_CONNECT_FAILURE_CANCELED,
    ESP_OPENCLAW_NODE_CONNECT_FAILURE_TRANSPORT_START_FAILED,
    ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED,
    ESP_OPENCLAW_NODE_WORK_MSG_WS_FATAL,
    ESP_OPENCLAW_NODE_CONNECT_ATTEMPT_CONNECTING,
};
typedef int esp_openclaw_node_internal_state_t;
typedef struct { int kind; const char *gateway_uri; } source_t;
typedef struct node *esp_openclaw_node_handle_t;
typedef struct { esp_openclaw_node_handle_t node; uint32_t transport_id; } esp_openclaw_node_transport_event_ctx_t;
typedef struct {
    const char *uri, *cert_pem, *cert_common_name;
    bool disable_auto_reconnect, keep_alive_enable, skip_cert_common_name_check;
    int network_timeout_ms, ping_interval_sec, pingpong_timeout_sec;
    int keep_alive_idle, keep_alive_interval, keep_alive_count;
    int task_prio, task_stack, buffer_size, cert_len;
    void *user_context;
} esp_websocket_client_config_t;
typedef void (*handler_t)(void *, int, int32_t, void *);
typedef struct {
    esp_websocket_client_handle_t (*client_init)(const esp_websocket_client_config_t *);
    esp_err_t (*register_events)(void *, int, handler_t, void *);
    esp_err_t (*client_start)(void *);
} ops_t;
struct node {
    int state;
    uint64_t active_connect_attempt_id;
    struct { uint64_t id; int state; } connect_attempt;
    int64_t connect_started_ms;
    source_t active_connect_source;
    struct { const char *gateway_uri; } persisted_session;
    struct { const char *tls_cert_pem, *tls_common_name, *role; int tls_cert_len;
        bool use_cert_bundle, skip_cert_common_name_check; } config;
    const ops_t *websocket_client_ops;
    uint32_t next_transport_id, active_transport_id, fatal_transport_id;
    esp_err_t fatal_transport_err;
    esp_openclaw_node_transport_event_ctx_t *transport_ctx;
    char *transport_gateway_uri;
    void *ws, *work_queue;
    bool client_started, transport_connected, connect_request_pending;
    char pending_connect_id[32];
};
typedef struct { int type, local_err; uint32_t transport_id; uint64_t connect_attempt_id; source_t connect_source; } esp_openclaw_node_work_message_t;
#include "timeout_constant.inc"
#define ESP_OPENCLAW_NODE_TASK_POLL_TICKS 250
#define ESP_OPENCLAW_NODE_WS_PING_INTERVAL_SEC 10
#define ESP_OPENCLAW_NODE_WS_PINGPONG_TIMEOUT_SEC 10
#define ESP_OPENCLAW_NODE_TRANSPORT_TASK_PRIORITY 5
#define ESP_OPENCLAW_NODE_TRANSPORT_TASK_STACK_SIZE 8192
#define ESP_OPENCLAW_NODE_TRANSPORT_BUFFER_SIZE 2048
#define ESP_OPENCLAW_NODE_TAG "test"
#define WEBSOCKET_EVENT_ANY 0
#define pdTRUE 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_RETURN_ON_ERROR(expr, ...) do { int err_ = (expr); if (err_) return err_; } while (0)

static int64_t now_ms, initial_ms;
static int starts, stops, failures, receives, messages, expired_messages;
static bool busy, ready, destroying;
static int fake_client;
static void esp_openclaw_node_lock_state(esp_openclaw_node_handle_t node) { (void)node; }
static void esp_openclaw_node_unlock_state(esp_openclaw_node_handle_t node) { (void)node; }
static int64_t esp_timer_get_time(void) { return now_ms * 1000; }
static const char *esp_openclaw_node_trimmed_or_null(const char *text) { return text; }
static char *esp_openclaw_node_duplicate_string(const char *text) { return strdup(text); }
static esp_err_t esp_openclaw_node_validate_tls_preflight(const void *config, const char *uri)
{ (void)config; assert(strncmp(uri, "ws://", 5) == 0); return ESP_OK; }
static void websocket_event_handler(void *ctx, int base, int32_t event, void *data)
{ (void)ctx; (void)base; (void)event; (void)data; }
static void esp_openclaw_node_transport_start_begin(const char *role) { (void)role; }
static void esp_openclaw_node_transport_start_end(const char *role, esp_err_t err)
{ (void)role; assert(err == ESP_OK); }
#include "production_0.inc"
static void esp_openclaw_node_cleanup_transport_instance(esp_openclaw_node_handle_t node, bool stop)
{
    if (stop && node->client_started) stops++;
    free(node->transport_ctx);
    free(node->transport_gateway_uri);
    node->transport_ctx = NULL;
    node->transport_gateway_uri = NULL;
    node->ws = NULL;
    node->active_transport_id = 0;
    node->client_started = false;
    esp_openclaw_node_clear_session_wait_state_locked(node);
}
static void esp_openclaw_node_complete_connect_failed(esp_openclaw_node_handle_t node,
    int reason, int error, const char *detail, bool stop)
{
    assert(node->state == ESP_OPENCLAW_NODE_INTERNAL_CONNECTING);
    assert(reason == ESP_OPENCLAW_NODE_CONNECT_FAILURE_TRANSPORT_START_FAILED);
    assert(error == ESP_ERR_TIMEOUT && detail == NULL && stop);
    assert(now_ms - initial_ms >= ESP_OPENCLAW_NODE_CONNECT_TIMEOUT_MS);
    esp_openclaw_node_cleanup_transport_instance(node, stop);
    node->state = ESP_OPENCLAW_NODE_INTERNAL_IDLE;
    failures++;
}
static void esp_openclaw_node_complete_disconnected(esp_openclaw_node_handle_t node,
    int reason, int error, bool stop)
{ (void)node; (void)reason; (void)error; (void)stop; assert(false); }
static void esp_openclaw_node_clear_pending_control_locked(esp_openclaw_node_handle_t node) { (void)node; }
static void esp_openclaw_node_move_connect_source_struct(source_t *dst, source_t *src)
{ *dst = *src; memset(src, 0, sizeof(*src)); }
static bool esp_openclaw_node_state_is_connecting(int state)
{ return state == ESP_OPENCLAW_NODE_INTERNAL_CONNECTING; }
static void *client_init(const esp_websocket_client_config_t *config)
{ assert(strcmp(config->uri, "ws://synthetic.example/ws") == 0); return &fake_client; }
static int register_events(void *ws, int event, handler_t callback, void *ctx)
{ assert(ws == &fake_client && event == WEBSOCKET_EVENT_ANY && callback && ctx); return ESP_OK; }
static int client_start(void *ws) { assert(ws == &fake_client); starts++; return ESP_OK; }
#include "production_1.inc"
#include "production_2.inc"
#include "production_3.inc"

static int xQueueReceive(void *queue, esp_openclaw_node_work_message_t *message, int wait)
{
    (void)queue;
    receives++;
    if (receives == 1) {
        message->type = 1;
        message->connect_source = (source_t){ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_NO_AUTH, "ws://synthetic.example/ws"};
        return pdTRUE;
    }
    now_ms += wait ? wait : 100;
    if (now_ms - initial_ms > ESP_OPENCLAW_NODE_CONNECT_TIMEOUT_MS + 1000) {
        message->type = 3;
        return pdTRUE;
    }
    if (!busy && wait == 0) return 0;
    if (!busy) return 0;
    message->type = 2;
    return pdTRUE;
}
static bool process_work_message(esp_openclaw_node_handle_t node, esp_openclaw_node_work_message_t *message)
{
    if (message->type == 1) {
        handle_request_connect(node, message);
        assert(node->client_started && starts == 1);
        if (ready) {
            node->state = ESP_OPENCLAW_NODE_INTERNAL_READY;
            esp_openclaw_node_clear_session_wait_state_locked(node);
        }
        if (destroying) node->state = ESP_OPENCLAW_NODE_INTERNAL_DESTROYING;
    } else if (message->type == 2) {
        messages++;
        if (now_ms - initial_ms >= ESP_OPENCLAW_NODE_CONNECT_TIMEOUT_MS && !failures)
            expired_messages++;
    } else if (message->type == 3) return false;
    return true;
}
static void esp_openclaw_node_free_work_message_payload(esp_openclaw_node_work_message_t *message) { (void)message; }
static void exit_node_task(esp_openclaw_node_handle_t node) { (void)node; }
#include "production_4.inc"

int main(int argc, char **argv)
{
    assert(argc == 2);
    busy = strcmp(argv[1], "idle") != 0;
    ready = strcmp(argv[1], "ready") == 0;
    destroying = strcmp(argv[1], "destroying") == 0;
    now_ms = initial_ms = strcmp(argv[1], "zero-clock") == 0 ? 0 : 1000;
    const ops_t ops = {client_init, register_events, client_start};
    struct node node = {.state = ESP_OPENCLAW_NODE_INTERNAL_IDLE, .websocket_client_ops = &ops};
    esp_openclaw_node_task(&node);
    assert(starts == 1);
    assert(failures == (ready || destroying ? 0 : 1));
    if (!ready && !destroying) {
        assert(stops == 1);
        assert(expired_messages <= 1);
        assert(node.state == ESP_OPENCLAW_NODE_INTERNAL_IDLE);
        esp_openclaw_node_work_message_t retry = {
            .connect_source = {ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_NO_AUTH, "ws://synthetic.example/ws"},
        };
        handle_request_connect(&node, &retry);
        assert(starts == 2 && node.client_started);
    }
    esp_openclaw_node_cleanup_transport_instance(&node, true);
    printf("%s: deadline and retry ownership preserved (%d queued messages)\n", argv[1], messages);
    return 0;
}
