#include "esp_openclaw_setup_gateway.h"
#include "esp_openclaw_node_wifi.h"

#include <stddef.h>

bool esp_openclaw_setup_gateway_init(esp_openclaw_setup_gateway_t *binding,
    esp_openclaw_setup_session_t *session, esp_openclaw_node_handle_t node)
{
    if (binding == NULL || session == NULL || node == NULL) return false;
    *binding = (esp_openclaw_setup_gateway_t){.session = session, .node = node};
    return true;
}

esp_err_t esp_openclaw_setup_gateway_activate(esp_openclaw_setup_gateway_t *binding,
                                             uint64_t generation)
{
    if (binding == NULL || binding->session == NULL) return ESP_ERR_INVALID_ARG;
    esp_openclaw_setup_snapshot_t session = esp_openclaw_setup_snapshot(binding->session);
    if (generation == 0 || session.generation != generation || session.phase != ESP_OPENCLAW_SETUP_READY)
        return ESP_ERR_INVALID_STATE;
    if (binding->lease != 0)
        return binding->generation == generation ? ESP_OK : ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_openclaw_node_connect_lease_acquire(binding->node, &binding->lease);
    if (err == ESP_OK) {
        binding->generation = generation;
        binding->token = (esp_openclaw_setup_token_t){0};
        binding->attempt = (esp_openclaw_node_connect_attempt_t){0};
    }
    return err;
}

static bool authorize(void *context)
{
    esp_openclaw_setup_gateway_t *binding = context;
    /* Read only the immutable dispatch snapshot, never the owner-task session. */
    uint64_t now = binding->now_ms(binding->authority_context);
    return now >= binding->opened_ms && now - binding->opened_ms < ESP_OPENCLAW_SETUP_TIMEOUT_MS &&
        binding->peer_is_current_authenticated(binding->peer, binding->authority_context);
}

typedef struct {
    esp_openclaw_setup_gateway_t *binding;
    const char *setup_code;
    esp_err_t error;
} dispatch_context_t;

static void dispatch(esp_openclaw_setup_token_t token,
                     esp_openclaw_setup_operation_t operation, void *context)
{
    (void)operation;
    dispatch_context_t *request = context;
    esp_openclaw_setup_gateway_t *binding = request->binding;
    binding->token = token;
    binding->attempt = (esp_openclaw_node_connect_attempt_t){0};
    binding->cancel_error = ESP_OK;
    binding->peer = binding->session->peer;
    binding->opened_ms = binding->session->opened_ms;
    binding->now_ms = binding->session->callbacks.now_ms;
    binding->peer_is_current_authenticated = binding->session->callbacks.peer_is_current_authenticated;
    binding->authority_context = binding->session->callbacks.context;
    esp_openclaw_node_wifi_status_t wifi = {0};
    esp_openclaw_node_wifi_get_status(&wifi);
    const esp_openclaw_node_connect_request_t connect = {
        .source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_SETUP_CODE,
        .value = request->setup_code,
    };
    request->error = wifi.connected
        ? esp_openclaw_node_connect_begin_authorized(binding->node, binding->lease, &connect,
            authorize, binding, &binding->attempt.id)
        : ESP_ERR_INVALID_STATE;
    binding->active = request->error == ESP_OK;
    if (!binding->active) {
        binding->attempt.state = ESP_OPENCLAW_NODE_CONNECT_ATTEMPT_FAILED;
        binding->attempt.local_err = request->error;
        esp_openclaw_setup_complete(binding->session, token, ESP_OPENCLAW_SETUP_FAILED, request->error);
    }
}

esp_openclaw_setup_result_t esp_openclaw_setup_gateway_submit(
    esp_openclaw_setup_gateway_t *binding, uint64_t generation, uint64_t peer,
    uint32_t operation_id, const char *setup_code, esp_openclaw_setup_receipt_t *receipt)
{
    if (binding == NULL || binding->session == NULL || receipt == NULL) return ESP_OPENCLAW_SETUP_INVALID;
    if (binding->lease == 0 || generation != binding->generation) return ESP_OPENCLAW_SETUP_STALE;
    dispatch_context_t request = {.binding = binding, .setup_code = setup_code};
    esp_openclaw_setup_result_t result = esp_openclaw_setup_submit(binding->session,
        generation, peer, operation_id, ESP_OPENCLAW_SETUP_GATEWAY_APPLY, dispatch, &request, receipt);
    if (result == ESP_OPENCLAW_SETUP_ACCEPTED && request.error != ESP_OK) {
        receipt->outcome = ESP_OPENCLAW_SETUP_FAILED;
        receipt->detail = request.error;
    }
    return result;
}

void esp_openclaw_setup_gateway_cancel(esp_openclaw_setup_gateway_t *binding,
                                      esp_openclaw_setup_token_t token)
{
    if (binding != NULL && binding->active && token.generation == binding->token.generation &&
        token.operation_id == binding->token.operation_id) {
        binding->cancel_error = esp_openclaw_node_connect_cancel(
            binding->node, binding->lease, binding->attempt.id);
    }
}

esp_err_t esp_openclaw_setup_gateway_poll(esp_openclaw_setup_gateway_t *binding,
                                        bool *released)
{
    if (released == NULL) return ESP_ERR_INVALID_ARG;
    *released = false;
    if (binding == NULL || binding->session == NULL) return ESP_ERR_INVALID_ARG;
    esp_openclaw_setup_poll(binding->session);
    if (binding->active) {
        esp_err_t err = esp_openclaw_node_connect_snapshot(
            binding->node, binding->lease, binding->attempt.id, &binding->attempt);
        if (err != ESP_OK) return err;
        bool succeeded = binding->attempt.state == ESP_OPENCLAW_NODE_CONNECT_ATTEMPT_SUCCEEDED;
        if (succeeded || binding->attempt.state == ESP_OPENCLAW_NODE_CONNECT_ATTEMPT_FAILED) {
            int32_t detail = succeeded ? ESP_OK : binding->attempt.local_err;
            if (!succeeded && binding->attempt.failure_reason == ESP_OPENCLAW_NODE_CONNECT_FAILURE_CANCELED)
                detail = ESP_OPENCLAW_SETUP_GATEWAY_CANCELLED;
            else if (!succeeded && detail == ESP_OK) detail = ESP_FAIL;
            if (!esp_openclaw_setup_complete(binding->session, binding->token,
                succeeded ? ESP_OPENCLAW_SETUP_SUCCEEDED : ESP_OPENCLAW_SETUP_FAILED, detail))
                return ESP_ERR_INVALID_STATE;
            binding->active = false;
        } else if (binding->cancel_error != ESP_OK) {
            return binding->cancel_error;
        }
    }
    esp_openclaw_setup_snapshot_t session = esp_openclaw_setup_snapshot(binding->session);
    if (binding->lease != 0 && !binding->active &&
        (session.phase == ESP_OPENCLAW_SETUP_CLOSED || session.generation != binding->generation)) {
        esp_err_t err = esp_openclaw_node_connect_lease_release(binding->node, binding->lease);
        if (err != ESP_OK) return err;
        binding->lease = 0;
        binding->generation = 0;
        *released = true;
    }
    return ESP_OK;
}

esp_openclaw_setup_gateway_snapshot_t esp_openclaw_setup_gateway_snapshot(
    const esp_openclaw_setup_gateway_t *binding)
{
    if (binding == NULL) return (esp_openclaw_setup_gateway_snapshot_t){0};
    return (esp_openclaw_setup_gateway_snapshot_t){
        .lease_held = binding->lease != 0, .operation_pending = binding->active,
        .token = binding->token, .attempt = binding->attempt,
    };
}
