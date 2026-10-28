#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_openclaw_node.h"

/* Only platform boundaries are replaced: API admission, NVS revalidation,
 * queue rollback, and the connect/disconnect worker handlers are production. */
enum { ESP_OPENCLAW_NODE_INTERNAL_IDLE, ESP_OPENCLAW_NODE_INTERNAL_CONNECTING,
       ESP_OPENCLAW_NODE_INTERNAL_READY, ESP_OPENCLAW_NODE_INTERNAL_DESTROYING,
       ESP_OPENCLAW_NODE_INTERNAL_CLOSED };
typedef int esp_openclaw_node_internal_state_t;
enum { ESP_OPENCLAW_NODE_PENDING_CONTROL_REQUEST_NONE,
       ESP_OPENCLAW_NODE_PENDING_CONTROL_REQUEST_CONNECT,
       ESP_OPENCLAW_NODE_PENDING_CONTROL_REQUEST_DISCONNECT };
typedef int esp_openclaw_node_pending_control_request_t;
enum { ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_NONE,
       ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SAVED_SESSION,
       ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_NO_AUTH };
enum { ESP_OPENCLAW_NODE_WORK_MSG_REQUEST_CONNECT, ESP_OPENCLAW_NODE_WORK_MSG_REQUEST_DISCONNECT };
typedef struct { int kind; char *gateway_uri, *secret; } esp_openclaw_node_connect_request_source_t;
typedef struct { char *gateway_uri; bool present; } esp_openclaw_node_persisted_session_t;
typedef struct {
    int type;
    esp_openclaw_node_connect_request_source_t connect_source;
} esp_openclaw_node_work_message_t;
struct esp_openclaw_node {
    esp_openclaw_node_config_t config;
    esp_openclaw_node_internal_state_t state;
    esp_openclaw_node_pending_control_request_t pending_control;
    esp_openclaw_node_connect_lease_t connect_lease;
    uint64_t connect_lease_generation;
    bool connect_request_pending;
    esp_openclaw_node_persisted_session_t persisted_session;
    esp_openclaw_node_connect_request_source_t active_connect_source;
    int64_t connect_started_ms;
    void *ws;
    uint32_t active_transport_id;
};
static bool locked, queued, queue_full;
static unsigned load_count, start_count;
static esp_openclaw_node_work_message_t queue_message;
static esp_openclaw_node_handle_t current;
static void (*during_load)(void), (*during_build)(void), (*terminal_callback)(void);
static esp_openclaw_node_connect_lease_t acquired;

static void esp_openclaw_node_lock_state(esp_openclaw_node_handle_t node)
{ (void)node; assert(!locked); locked = true; }
static void esp_openclaw_node_unlock_state(esp_openclaw_node_handle_t node)
{ (void)node; assert(locked); locked = false; }
static bool esp_openclaw_node_state_is_connecting(int state)
{ return state == ESP_OPENCLAW_NODE_INTERNAL_CONNECTING; }
static void esp_openclaw_node_set_pending_control_locked(esp_openclaw_node_handle_t node, int state)
{ assert(locked); node->pending_control = state; }
static void esp_openclaw_node_clear_pending_control_locked(esp_openclaw_node_handle_t node)
{ esp_openclaw_node_set_pending_control_locked(node, ESP_OPENCLAW_NODE_PENDING_CONTROL_REQUEST_NONE); }
static bool esp_openclaw_node_saved_session_is_present_locked(esp_openclaw_node_handle_t node)
{ assert(locked); return node->persisted_session.present; }
bool esp_openclaw_node_has_saved_session(esp_openclaw_node_handle_t node)
{
    esp_openclaw_node_lock_state(node);
    bool present = esp_openclaw_node_saved_session_is_present_locked(node);
    esp_openclaw_node_unlock_state(node);
    return present;
}
static void esp_openclaw_node_persisted_session_free(esp_openclaw_node_persisted_session_t *session)
{ free(session->gateway_uri); memset(session, 0, sizeof(*session)); }
static bool esp_openclaw_node_persisted_session_is_present(const esp_openclaw_node_persisted_session_t *session)
{ return session->present; }
static esp_err_t esp_openclaw_node_persisted_session_load(const char *role, esp_openclaw_node_persisted_session_t *out)
{
    (void)role; assert(!locked); ++load_count;
    out->gateway_uri = strdup("wss://new.example"); out->present = true;
    if (during_load) during_load();
    return ESP_OK;
}
static void esp_openclaw_node_clear_connect_source_struct(esp_openclaw_node_connect_request_source_t *source)
{ free(source->gateway_uri); free(source->secret); memset(source, 0, sizeof(*source)); }
static void esp_openclaw_node_move_connect_source_struct(esp_openclaw_node_connect_request_source_t *out,
                                                       esp_openclaw_node_connect_request_source_t *in)
{ esp_openclaw_node_clear_connect_source_struct(out); *out = *in; memset(in, 0, sizeof(*in)); }
static esp_err_t esp_openclaw_node_build_connect_source_from_request(const esp_openclaw_node_connect_request_t *in,
                                                                    esp_openclaw_node_connect_request_source_t *out)
{
    assert(!locked);
    out->kind = in->source == ESP_OPENCLAW_NODE_CONNECT_SOURCE_SAVED_SESSION
        ? ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SAVED_SESSION : ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_NO_AUTH;
    if (in->gateway_uri) out->gateway_uri = strdup(in->gateway_uri);
    if (during_build) during_build();
    return ESP_OK;
}
static esp_err_t validate_connect_source(const esp_openclaw_node_connect_request_source_t *source)
{ return source->kind ? ESP_OK : ESP_ERR_INVALID_ARG; }
static esp_err_t esp_openclaw_node_validate_tls_preflight(const esp_openclaw_node_config_t *config, const char *uri)
{ (void)config; assert(uri); return ESP_OK; }
#define esp_err_to_name(err) "fixture"
#define ESP_LOGW(...) ((void)0)
#define ESP_RETURN_ON_ERROR(expr, ...) do { esp_err_t err_ = (expr); if (err_ != ESP_OK) return err_; } while (0)
static esp_err_t esp_openclaw_node_enqueue_work_message(esp_openclaw_node_handle_t node,
                                                       esp_openclaw_node_work_message_t *message)
{
    (void)node; assert(!locked);
    if (queue_full) { esp_openclaw_node_clear_connect_source_struct(&message->connect_source); return ESP_ERR_NO_MEM; }
    assert(!queued); queued = true; queue_message = *message; return ESP_OK;
}
static int64_t esp_timer_get_time(void) { return 1000; }
static esp_err_t esp_openclaw_node_start_transport_for_active_source(esp_openclaw_node_handle_t node)
{ assert(!locked); ++start_count; node->ws = node; node->active_transport_id = start_count; return ESP_OK; }
static void finish_connection(esp_openclaw_node_handle_t node)
{
    assert(!locked); node->state = ESP_OPENCLAW_NODE_INTERNAL_IDLE;
    node->pending_control = ESP_OPENCLAW_NODE_PENDING_CONTROL_REQUEST_NONE;
    node->ws = NULL; node->active_transport_id = 0;
    if (terminal_callback) terminal_callback();
}
static void esp_openclaw_node_complete_connect_failed(esp_openclaw_node_handle_t node,
    esp_openclaw_node_connect_failure_reason_t reason, esp_err_t err, const char *detail, bool stop)
{ (void)reason; (void)err; (void)detail; (void)stop; finish_connection(node); }
static void esp_openclaw_node_complete_disconnected(esp_openclaw_node_handle_t node,
    esp_openclaw_node_disconnected_reason_t reason, esp_err_t err, bool stop)
{ (void)reason; (void)err; (void)stop; finish_connection(node); }
static esp_err_t esp_openclaw_node_submit_connect_request(esp_openclaw_node_handle_t node,
    esp_openclaw_node_connect_request_source_t *source, esp_openclaw_node_connect_lease_t lease, uint64_t generation);
static esp_err_t esp_openclaw_node_submit_disconnect_request(esp_openclaw_node_handle_t node,
    esp_openclaw_node_connect_lease_t lease);

#include "control.inc"
#include "admission.inc"

static const esp_openclaw_node_connect_request_t explicit_request = {
    .source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_NO_AUTH, .gateway_uri = "wss://gateway.example",
};
static const esp_openclaw_node_connect_request_t saved_request = {
    .source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_SAVED_SESSION,
};
static void initialize(esp_openclaw_node_handle_t node, int state)
{
    memset(node, 0, sizeof(*node)); current = node; node->state = state;
    node->persisted_session = (esp_openclaw_node_persisted_session_t){strdup("wss://old.example"), true};
    queued = queue_full = false; load_count = start_count = 0;
    during_load = during_build = terminal_callback = NULL; acquired = 0;
}
static void cleanup(esp_openclaw_node_handle_t node)
{
    assert(!locked); assert(!queued);
    esp_openclaw_node_persisted_session_free(&node->persisted_session);
    esp_openclaw_node_clear_connect_source_struct(&node->active_connect_source);
}
static void dispatch(esp_openclaw_node_handle_t node)
{
    assert(queued); queued = false;
    esp_openclaw_node_work_message_t message = queue_message;
    memset(&queue_message, 0, sizeof(queue_message));
    if (message.type == ESP_OPENCLAW_NODE_WORK_MSG_REQUEST_CONNECT) handle_request_connect(node, &message);
    else handle_request_disconnect(node);
    esp_openclaw_node_clear_connect_source_struct(&message.connect_source);
}
static void take_lease(void)
{ assert(esp_openclaw_node_connect_lease_acquire(current, &acquired) == ESP_OK); }
static void cycle_lease(void)
{ take_lease(); assert(esp_openclaw_node_connect_lease_release(current, acquired) == ESP_OK); }
static void reject_acquisition_from_old_terminal(void)
{
    esp_openclaw_node_connect_lease_t lease;
    assert(esp_openclaw_node_connect_lease_acquire(current, &lease) == ESP_ERR_INVALID_STATE);
    assert(esp_openclaw_node_request_connect(current, &explicit_request) == ESP_ERR_INVALID_STATE);
}
static void reject_release_from_old_terminal(void)
{
    assert(esp_openclaw_node_connect_lease_release(current, acquired) == ESP_ERR_INVALID_STATE);
    assert(esp_openclaw_node_request_connect_with_lease(current, acquired, &explicit_request) == ESP_ERR_INVALID_STATE);
    assert(esp_openclaw_node_request_disconnect_with_lease(current, acquired) == ESP_ERR_INVALID_STATE);
}

static void test_lease_lifetime(void)
{
    struct esp_openclaw_node node;
    initialize(&node, ESP_OPENCLAW_NODE_INTERNAL_IDLE);
    esp_openclaw_node_connect_lease_t lease = 99, newer;
    assert(esp_openclaw_node_connect_lease_acquire(NULL, &lease) == ESP_ERR_INVALID_ARG && lease == 0);
    assert(esp_openclaw_node_connect_lease_acquire(&node, NULL) == ESP_ERR_INVALID_ARG);
    for (int state = ESP_OPENCLAW_NODE_INTERNAL_CONNECTING; state <= ESP_OPENCLAW_NODE_INTERNAL_CLOSED; ++state) {
        if (state == ESP_OPENCLAW_NODE_INTERNAL_READY) continue;
        node.state = state;
        assert(esp_openclaw_node_connect_lease_acquire(&node, &lease) == ESP_ERR_INVALID_STATE && lease == 0);
    }
    node.state = ESP_OPENCLAW_NODE_INTERNAL_READY; node.active_transport_id = 42; node.ws = &node;
    take_lease();
    assert(node.state == ESP_OPENCLAW_NODE_INTERNAL_READY && node.active_transport_id == 42 && node.ws == &node);
    assert(esp_openclaw_node_connect_lease_acquire(&node, &lease) == ESP_ERR_INVALID_STATE);
    assert(esp_openclaw_node_request_connect(&node, &explicit_request) == ESP_ERR_INVALID_STATE);
    assert(esp_openclaw_node_request_connect(&node, &saved_request) == ESP_ERR_INVALID_STATE && load_count == 0);
    assert(esp_openclaw_node_request_disconnect(&node) == ESP_ERR_INVALID_STATE);
    assert(esp_openclaw_node_request_connect_with_lease(&node, 0, &explicit_request) == ESP_ERR_INVALID_ARG);
    assert(esp_openclaw_node_request_disconnect_with_lease(&node, 0) == ESP_ERR_INVALID_ARG);
    assert(esp_openclaw_node_connect_lease_release(&node, acquired + 1) == ESP_ERR_INVALID_STATE);
    assert(esp_openclaw_node_connect_lease_release(&node, acquired) == ESP_OK);
    assert(node.active_transport_id == 42 && node.ws == &node);
    assert(esp_openclaw_node_connect_lease_acquire(&node, &newer) == ESP_OK && newer != acquired);
    assert(esp_openclaw_node_request_connect_with_lease(&node, acquired, &explicit_request) == ESP_ERR_INVALID_STATE);
    assert(esp_openclaw_node_request_disconnect_with_lease(&node, acquired) == ESP_ERR_INVALID_STATE);
    assert(esp_openclaw_node_connect_lease_release(&node, acquired) == ESP_ERR_INVALID_STATE);
    assert(esp_openclaw_node_connect_lease_release(&node, newer) == ESP_OK);
    node.connect_lease_generation = UINT64_MAX;
    assert(esp_openclaw_node_connect_lease_acquire(&node, &lease) == ESP_ERR_INVALID_STATE && lease == 0);
    cleanup(&node);
}
static void test_owned_queue_and_replacement(void)
{
    struct esp_openclaw_node node;
    initialize(&node, ESP_OPENCLAW_NODE_INTERNAL_READY); take_lease();
    queue_full = true;
    assert(esp_openclaw_node_request_connect_with_lease(&node, acquired, &explicit_request) == ESP_ERR_NO_MEM);
    assert(esp_openclaw_node_connect_lease_release(&node, acquired) == ESP_OK);
    take_lease(); queue_full = false;
    assert(esp_openclaw_node_request_connect_with_lease(&node, acquired, &explicit_request) == ESP_OK);
    assert(esp_openclaw_node_connect_lease_release(&node, acquired) == ESP_ERR_INVALID_STATE);
    assert(esp_openclaw_node_request_disconnect_with_lease(&node, acquired) == ESP_ERR_INVALID_STATE);
    terminal_callback = reject_release_from_old_terminal;
    dispatch(&node); terminal_callback = NULL;
    assert(start_count == 1 && node.state == ESP_OPENCLAW_NODE_INTERNAL_CONNECTING);
    assert(esp_openclaw_node_connect_lease_release(&node, acquired) == ESP_ERR_INVALID_STATE);
    assert(esp_openclaw_node_request_disconnect_with_lease(&node, acquired) == ESP_OK);
    dispatch(&node);
    assert(esp_openclaw_node_connect_lease_release(&node, acquired) == ESP_OK);
    /* Releasing admission lets the application's explicit reconnect wake work. */
    assert(esp_openclaw_node_request_connect(&node, &saved_request) == ESP_OK);
    dispatch(&node); assert(start_count == 2 && load_count == 1);
    assert(strcmp(node.persisted_session.gateway_uri, "wss://new.example") == 0);
    cleanup(&node);
}
static void test_unleased_replacement_reserves_control(void)
{
    struct esp_openclaw_node node;
    initialize(&node, ESP_OPENCLAW_NODE_INTERNAL_READY);
    assert(esp_openclaw_node_request_connect(&node, &explicit_request) == ESP_OK);
    terminal_callback = reject_acquisition_from_old_terminal;
    dispatch(&node); terminal_callback = NULL;
    assert(start_count == 1 && node.state == ESP_OPENCLAW_NODE_INTERNAL_CONNECTING);
    cleanup(&node);
}
static void test_preparation_races(void)
{
    struct esp_openclaw_node node;
    initialize(&node, ESP_OPENCLAW_NODE_INTERNAL_IDLE);
    during_load = take_lease;
    assert(esp_openclaw_node_request_connect(&node, &saved_request) == ESP_ERR_INVALID_STATE);
    assert(strcmp(node.persisted_session.gateway_uri, "wss://old.example") == 0 && !queued);
    assert(esp_openclaw_node_connect_lease_release(&node, acquired) == ESP_OK);
    during_load = cycle_lease;
    assert(esp_openclaw_node_request_connect(&node, &saved_request) == ESP_ERR_INVALID_STATE);
    assert(strcmp(node.persisted_session.gateway_uri, "wss://old.example") == 0 && !queued);
    during_load = NULL; during_build = cycle_lease;
    assert(esp_openclaw_node_request_connect(&node, &explicit_request) == ESP_ERR_INVALID_STATE && !queued);
    during_build = NULL;
    assert(esp_openclaw_node_request_connect(&node, &explicit_request) == ESP_OK);
    esp_openclaw_node_connect_lease_t lease;
    assert(esp_openclaw_node_connect_lease_acquire(&node, &lease) == ESP_ERR_INVALID_STATE);
    dispatch(&node); cleanup(&node);
}
int main(void)
{
    test_lease_lifetime(); test_owned_queue_and_replacement();
    test_unleased_replacement_reserves_control(); test_preparation_races();
    puts("connection lease and admission: passed"); return 0;
}
