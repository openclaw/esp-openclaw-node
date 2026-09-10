#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_openclaw_node_transport_diag.h"
#include "esp_openclaw_room_node.h"
#include "freertos/task.h"

static esp_alloc_failed_hook_t callback;
static esp_err_t install_result;
static unsigned board_calls, room_calls;
static _Thread_local TaskHandle_t current_task = (void *)1;
static _Thread_local bool isr, in_callback, state_locked, sdk_returned, cleanup_done;
static _Thread_local unsigned queries, records, starts, cleanups;
static _Thread_local char record[768];
static _Thread_local const char *scenario;
static atomic_uint concurrent_ready;
static bool concurrent;

int xPortInIsrContext(void) { return isr; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { assert(!isr); return current_task; }
void *test_forbidden_malloc(size_t n) { (void)n; abort(); }
void *test_forbidden_calloc(size_t n, size_t size) { (void)n; (void)size; abort(); }
void *test_forbidden_realloc(void *p, size_t n) { (void)p; (void)n; abort(); }
int test_allocator_strncmp(const char *a, const char *b, size_t n)
{
    assert(!in_callback && sdk_returned);
    return strncmp(a, b, n);
}

static void fail_allocation(size_t size, uint32_t caps, const char *allocator)
{
    in_callback = true;
    callback(size, caps, allocator);
    in_callback = false;
}

esp_err_t heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t fn)
{
    assert(callback == NULL && board_calls == 0 && room_calls == 0);
    if (install_result == ESP_OK) {
        callback = fn;
    }
    return install_result;
}

esp_err_t tab5_room_board_config(esp_openclaw_room_node_config_t *config)
{
    (void)config;
    assert(callback != NULL || install_result != ESP_OK);
    ++board_calls;
    return ESP_OK;
}

esp_err_t esp_openclaw_room_node_start(const esp_openclaw_room_node_config_t *config)
{
    (void)config;
    assert(board_calls == 1);
    ++room_calls;
    return ESP_OK;
}

size_t heap_caps_get_free_size(uint32_t caps)
{
    assert(!in_callback && !state_locked && sdk_returned && !cleanup_done);
    assert(caps == 2052);
    ++queries;
    /* Neither heap queries nor logging may recapture after disarm. */
    fail_allocation(9999, 99, "secret-allocator-canary");
    return 30000;
}

size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    assert(!in_callback && !state_locked && sdk_returned && !cleanup_done);
    assert(caps == 2052);
    ++queries;
    return 4000;
}

void test_log(const char *tag, const char *format, ...)
{
    if (strcmp(tag, "tab5_alloc") != 0) {
        return;
    }
    assert(!in_callback && !state_locked && !cleanup_done);
    va_list args;
    va_start(args, format);
    int written = vsnprintf(record, sizeof(record), format, args);
    va_end(args);
    assert(written > 0 && (size_t)written < sizeof(record));
    assert(strstr(record, "secret-allocator-canary") == NULL);
    assert(strstr(record, "private-token-canary") == NULL);
    if (strstr(record, "alloc_diag phase=") != NULL) {
        assert(sdk_returned);
        ++records;
        fail_allocation(9999, 99, "secret-allocator-canary");
    }
}

typedef void *esp_websocket_client_handle_t;
typedef int esp_event_base_t;
typedef struct {
    const char *uri, *cert_pem, *cert_common_name;
    bool disable_auto_reconnect, keep_alive_enable, skip_cert_common_name_check;
    int network_timeout_ms, ping_interval_sec, pingpong_timeout_sec;
    int keep_alive_idle, keep_alive_interval, keep_alive_count, task_prio, task_stack, buffer_size;
    size_t cert_len;
    void *user_context;
} esp_websocket_client_config_t;
typedef struct {
    const char *role, *tls_cert_pem, *tls_common_name;
    size_t tls_cert_len;
    bool use_cert_bundle, skip_cert_common_name_check;
} config_t;
typedef struct {
    esp_websocket_client_handle_t (*client_init)(const esp_websocket_client_config_t *);
    esp_err_t (*register_events)(void *, int, void (*)(void *, int, int32_t, void *), void *);
    esp_err_t (*client_start)(void *);
} ops_t;
typedef struct node {
    config_t config;
    struct { int kind; char *gateway_uri; } active_connect_source;
    struct { char *gateway_uri; } persisted_session;
    const ops_t *websocket_client_ops;
    uint32_t next_transport_id, active_transport_id;
    void *ws, *transport_ctx;
    char *transport_gateway_uri;
    bool client_started, transport_connected;
} *esp_openclaw_node_handle_t;
typedef struct { esp_openclaw_node_handle_t node; uint32_t transport_id; } esp_openclaw_node_transport_event_ctx_t;
enum {
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_NONE,
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SAVED_SESSION,
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_BOOTSTRAP_TOKEN,
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_DEVICE_TOKEN,
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SHARED_TOKEN,
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_PASSWORD,
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_NO_AUTH,
};
#define ESP_OPENCLAW_NODE_TAG "node"
#define ESP_OPENCLAW_NODE_WS_PING_INTERVAL_SEC 20
#define ESP_OPENCLAW_NODE_WS_PINGPONG_TIMEOUT_SEC 20
#define ESP_OPENCLAW_NODE_TRANSPORT_TASK_PRIORITY 5
#define ESP_OPENCLAW_NODE_TRANSPORT_TASK_STACK_SIZE 8192
#define ESP_OPENCLAW_NODE_TRANSPORT_BUFFER_SIZE 2048
#define WEBSOCKET_EVENT_ANY 0
#define ESP_RETURN_ON_ERROR(call, tag, ...) do { esp_err_t e = (call); if (e) return e; } while (0)

static void esp_openclaw_node_lock_state(esp_openclaw_node_handle_t node)
{ (void)node; assert(!state_locked); state_locked = true; }
static void esp_openclaw_node_unlock_state(esp_openclaw_node_handle_t node)
{ (void)node; assert(state_locked); state_locked = false; }
static void esp_openclaw_node_clear_session_wait_state_locked(esp_openclaw_node_handle_t node)
{ (void)node; assert(state_locked); }
static const char *esp_openclaw_node_trimmed_or_null(const char *s) { return s; }
static char *esp_openclaw_node_duplicate_string(const char *s) { return strdup(s); }
static esp_err_t esp_openclaw_node_validate_tls_preflight(const config_t *c, const char *u)
{ (void)c; (void)u; return ESP_OK; }
static void websocket_event_handler(void *a, int b, int32_t c, void *d)
{ (void)a; (void)b; (void)c; (void)d; }

static void esp_openclaw_node_cleanup_transport_instance(esp_openclaw_node_handle_t node, bool stop)
{
    assert(!stop && !state_locked);
    if (starts != 0) {
        assert(records == 1);
    }
    cleanup_done = true;
    ++cleanups;
    free(node->transport_gateway_uri);
    free(node->transport_ctx);
    node->transport_gateway_uri = NULL;
    node->transport_ctx = NULL;
}

#include "transport_start_under_test.inc"

static void *client_init(const esp_websocket_client_config_t *config)
{
    assert(!state_locked && config->disable_auto_reconnect);
    assert(config->task_stack == ESP_OPENCLAW_NODE_TRANSPORT_TASK_STACK_SIZE);
    if (strcmp(scenario, "init-failure") == 0) {
        return NULL;
    }
    return (void *)1;
}

static esp_err_t register_events(void *ws, int event, void (*fn)(void *, int, int32_t, void *), void *ctx)
{
    (void)ws; (void)event; (void)fn; (void)ctx;
    assert(!state_locked);
    return strcmp(scenario, "register-failure") == 0 ? -77 : ESP_OK;
}

static esp_err_t client_start(void *ws)
{
    (void)ws;
    assert(!state_locked);
    ++starts;
    if (concurrent) {
        atomic_fetch_add(&concurrent_ready, 1);
        while (atomic_load(&concurrent_ready) != 2) {
            sched_yield();
        }
    }
    if (strcmp(scenario, "exclusions") == 0) {
        isr = true;
        fail_allocation(8000, 9, "heap_caps_malloc");
        isr = false;
        TaskHandle_t saved = current_task;
        current_task = (void *)99;
        fail_allocation(8000, 9, "heap_caps_malloc");
        current_task = saved;
        fail_allocation(1234, 2052, "secret-allocator-canary");
    } else if (strcmp(scenario, "uncaptured-failure") != 0 && strcmp(scenario, "success") != 0) {
        fail_allocation(1234, 2052, "heap_caps_malloc");
        fail_allocation(9000, 9, "heap_caps_calloc");
    }
    assert(queries == 0 && records == 0);
    /* Model the SDK's own cleanup before returning its original result. */
    sdk_returned = true;
    return strcmp(scenario, "success") == 0 || strcmp(scenario, "captured-success") == 0 ? ESP_OK : -91;
}

static void run_start(const char *role)
{
    queries = records = starts = cleanups = 0;
    sdk_returned = cleanup_done = false;
    record[0] = '\0';
    const ops_t ops = {client_init, register_events, client_start};
    struct node node = {
        .config.role = role, .websocket_client_ops = &ops,
        .active_connect_source = {
            .kind = ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_DEVICE_TOKEN,
            .gateway_uri = "ws://private-token-canary.invalid",
        },
    };
    if (strcmp(scenario, "no-source") == 0) {
        node.active_connect_source.kind = ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_NONE;
    }
    esp_err_t result = esp_openclaw_node_start_transport_for_active_source(&node);
    if (strcmp(scenario, "no-source") == 0 || strcmp(scenario, "init-failure") == 0 ||
        strcmp(scenario, "register-failure") == 0) {
        assert(result == (strcmp(scenario, "no-source") == 0 ? ESP_ERR_INVALID_STATE :
                          strcmp(scenario, "init-failure") == 0 ? ESP_FAIL : -77));
        assert(starts == 0 && records == 0 && queries == 0);
        assert(cleanups == (unsigned)(strcmp(scenario, "register-failure") == 0));
        return;
    }
    bool success = strcmp(scenario, "success") == 0 || strcmp(scenario, "captured-success") == 0;
    bool captured = strcmp(scenario, "uncaptured-failure") != 0 && strcmp(scenario, "success") != 0;
    assert(result == (success ? ESP_OK : -91));
    assert(cleanups == (unsigned)!success && node.client_started == success);
    assert(records == (unsigned)(captured || !success));
    assert(queries == (captured ? 2U : 0U));
    if (records) {
        char expected[768];
        snprintf(expected, sizeof(expected),
                 "alloc_diag phase=after_sdk_return_before_owner_cleanup role=%u sdk_err=%d "
                 "captured=%u requested=%u caps=%u allocator=%u origin=0 "
                 "heap_sampled=%u free=%u largest=%u",
                 strcmp(role, "node") == 0 ? 1U : 2U, result, (unsigned)captured,
                 captured ? 1234U : 0U, captured ? 2052U : 0U,
                 captured && strcmp(scenario, "exclusions") != 0 ? 1U : 0U,
                 (unsigned)captured, captured ? 30000U : 0U, captured ? 4000U : 0U);
        assert(strcmp(record, expected) == 0);
    }
    if (success) {
        free(node.transport_gateway_uri);
        free(node.transport_ctx);
    }
    unsigned before = records;
    fail_allocation(8888, 99, "heap_caps_malloc");
    esp_openclaw_node_transport_start_end(role, ESP_OK);
    assert(records == before);
}

static void *concurrent_start(void *task)
{
    current_task = task;
    scenario = "capture";
    run_start(task == (void *)1 ? "node" : "operator");
    return NULL;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    scenario = argv[1];
    if (strcmp(scenario, "install-failure") == 0) {
        install_result = -88;
    }
    extern void app_main(void);
    app_main();
    assert(board_calls == 1 && room_calls == 1);
    if (install_result != ESP_OK) {
        assert(strcmp(record, "alloc_diag_install err=-88") == 0);
        return 0;
    }
    fail_allocation(9999, 99, "heap_caps_malloc");
    if (strcmp(scenario, "concurrent") == 0) {
        concurrent = true;
        pthread_t node, operator;
        assert(pthread_create(&node, NULL, concurrent_start, (void *)1) == 0);
        assert(pthread_create(&operator, NULL, concurrent_start, (void *)2) == 0);
        assert(pthread_join(node, NULL) == 0 && pthread_join(operator, NULL) == 0);
    } else if (strcmp(scenario, "rearm") == 0) {
        scenario = "capture";
        run_start("node");
        scenario = "uncaptured-failure";
        run_start("node");
        scenario = "captured-success";
        run_start("operator");
    } else {
        run_start("node");
    }
    return 0;
}
