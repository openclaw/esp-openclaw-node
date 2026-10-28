#include "esp_openclaw_setup_wifi.h"

#include <stddef.h>

bool esp_openclaw_setup_wifi_init(esp_openclaw_setup_wifi_t *binding,
                                 esp_openclaw_setup_session_t *session)
{
    if (binding == NULL || session == NULL) return false;
    *binding = (esp_openclaw_setup_wifi_t){.session = session};
    return true;
}

esp_err_t esp_openclaw_setup_wifi_activate(esp_openclaw_setup_wifi_t *binding,
                                         uint64_t generation)
{
    if (binding == NULL || binding->session == NULL) return ESP_ERR_INVALID_ARG;
    esp_openclaw_setup_snapshot_t session = esp_openclaw_setup_snapshot(binding->session);
    if (generation == 0 || session.generation != generation || session.phase != ESP_OPENCLAW_SETUP_READY)
        return ESP_ERR_INVALID_STATE;
    if (binding->lease != 0)
        return binding->generation == generation ? ESP_OK : ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_openclaw_node_wifi_setup_acquire(&binding->lease);
    if (err == ESP_OK) {
        binding->generation = generation;
        binding->token = (esp_openclaw_setup_token_t){0};
        binding->attempt = (esp_openclaw_node_wifi_attempt_t){0};
    }
    return err;
}

typedef struct {
    esp_openclaw_setup_wifi_t *binding;
    const char *ssid;
    const char *passphrase;
    esp_err_t error;
} dispatch_context_t;

static void dispatch(esp_openclaw_setup_token_t token,
                     esp_openclaw_setup_operation_t operation, void *context)
{
    (void)operation;
    dispatch_context_t *request = context;
    esp_openclaw_setup_wifi_t *binding = request->binding;
    binding->token = token;
    binding->attempt = (esp_openclaw_node_wifi_attempt_t){0};
    binding->cancel_requested = false;
    request->error = esp_openclaw_node_wifi_setup_join_begin(
        binding->lease, request->ssid, request->passphrase, &binding->attempt.id);
    binding->active = request->error == ESP_OK;
    if (!binding->active) {
        binding->attempt.state = ESP_OPENCLAW_NODE_WIFI_FAILED;
        binding->attempt.error = request->error;
        esp_openclaw_setup_complete(binding->session, token, ESP_OPENCLAW_SETUP_FAILED, request->error);
    }
}

esp_openclaw_setup_result_t esp_openclaw_setup_wifi_submit(
    esp_openclaw_setup_wifi_t *binding, uint64_t generation, uint64_t peer,
    uint32_t operation_id, const char *ssid, const char *passphrase,
    esp_openclaw_setup_receipt_t *receipt)
{
    if (binding == NULL || binding->session == NULL || receipt == NULL) return ESP_OPENCLAW_SETUP_INVALID;
    if (binding->lease == 0 || generation != binding->generation) return ESP_OPENCLAW_SETUP_STALE;
    dispatch_context_t request = {.binding = binding, .ssid = ssid, .passphrase = passphrase};
    esp_openclaw_setup_result_t result = esp_openclaw_setup_submit(binding->session,
        generation, peer, operation_id, ESP_OPENCLAW_SETUP_WIFI_APPLY, dispatch, &request, receipt);
    /* submit() copied PENDING before dispatch. Return the synchronous rejection
     * just recorded, even if completion closed an expired session. */
    if (result == ESP_OPENCLAW_SETUP_ACCEPTED && request.error != ESP_OK) {
        receipt->outcome = ESP_OPENCLAW_SETUP_FAILED;
        receipt->detail = request.error;
    }
    return result;
}

void esp_openclaw_setup_wifi_cancel(esp_openclaw_setup_wifi_t *binding,
                                   esp_openclaw_setup_token_t token)
{
    if (binding != NULL && binding->active && token.generation == binding->token.generation &&
        token.operation_id == binding->token.operation_id) binding->cancel_requested = true;
}

static bool authorize(void *context)
{
    esp_openclaw_setup_wifi_t *binding = context;
    return esp_openclaw_setup_authorize_operation(binding->session, binding->token);
}

esp_err_t esp_openclaw_setup_wifi_poll(esp_openclaw_setup_wifi_t *binding)
{
    if (binding == NULL || binding->session == NULL) return ESP_ERR_INVALID_ARG;
    esp_openclaw_setup_poll(binding->session);
    if (binding->active) {
        if (binding->cancel_requested) {
            esp_err_t err = esp_openclaw_node_wifi_setup_join_cancel(binding->lease, binding->attempt.id);
            if (err != ESP_OK) return err;
            binding->cancel_requested = false;
        }
        esp_err_t err = esp_openclaw_node_wifi_setup_join_poll(binding->lease, binding->attempt.id,
            authorize, binding, &binding->attempt);
        if (err != ESP_OK) return err;
        esp_openclaw_node_wifi_attempt_state_t state = binding->attempt.state;
        if (state == ESP_OPENCLAW_NODE_WIFI_SUCCEEDED || state == ESP_OPENCLAW_NODE_WIFI_FAILED ||
            state == ESP_OPENCLAW_NODE_WIFI_CANCELLED) {
            esp_openclaw_setup_outcome_t outcome = state == ESP_OPENCLAW_NODE_WIFI_SUCCEEDED ?
                ESP_OPENCLAW_SETUP_SUCCEEDED : ESP_OPENCLAW_SETUP_FAILED;
            int32_t detail = state == ESP_OPENCLAW_NODE_WIFI_CANCELLED ?
                ESP_OPENCLAW_SETUP_WIFI_CANCELLED : binding->attempt.error;
            if (!esp_openclaw_setup_complete(binding->session, binding->token, outcome, detail))
                return ESP_ERR_INVALID_STATE;
            binding->active = false;
            binding->cancel_requested = false;
        }
    }
    esp_openclaw_setup_snapshot_t session = esp_openclaw_setup_snapshot(binding->session);
    if (binding->lease != 0 && !binding->active &&
        (session.phase == ESP_OPENCLAW_SETUP_CLOSED || session.generation != binding->generation)) {
        esp_err_t reconnect = ESP_OK;
        esp_err_t err = esp_openclaw_node_wifi_setup_release(binding->lease, &reconnect);
        if (err != ESP_OK) return err;
        binding->lease = 0;
        binding->generation = 0;
        return reconnect;
    }
    return ESP_OK;
}

esp_err_t esp_openclaw_setup_wifi_retry_drain(esp_openclaw_setup_wifi_t *binding)
{
    if (binding == NULL || !binding->active) return ESP_ERR_INVALID_STATE;
    return esp_openclaw_node_wifi_setup_join_retry_drain(binding->lease, binding->attempt.id);
}

esp_openclaw_setup_wifi_snapshot_t esp_openclaw_setup_wifi_snapshot(
    const esp_openclaw_setup_wifi_t *binding)
{
    if (binding == NULL) return (esp_openclaw_setup_wifi_snapshot_t){0};
    return (esp_openclaw_setup_wifi_snapshot_t){
        .lease_held = binding->lease != 0, .operation_pending = binding->active,
        .token = binding->token, .attempt = binding->attempt,
    };
}
