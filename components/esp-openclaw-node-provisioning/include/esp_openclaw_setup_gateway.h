#pragma once

#include "esp_openclaw_node.h"
#include "esp_openclaw_setup_session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_OPENCLAW_SETUP_GATEWAY_CANCELLED (-3)

/* Initialize once per boot. Calls belong to the session's serialized owner.
 * The node and session must remain alive until the lease is released. Session
 * clock/live-peer callbacks must also be bounded and thread-safe on the node
 * worker: they must not read mutable setup/UI state or call node/session APIs.
 * This binding exclusively completes the GATEWAY_APPLY tokens it dispatches.
 * Treat fields as private. */
typedef struct {
    esp_openclaw_setup_session_t *session;
    esp_openclaw_node_handle_t node;
    uint64_t generation;
    esp_openclaw_node_connect_lease_t lease;
    esp_openclaw_setup_token_t token;
    esp_openclaw_node_connect_attempt_t attempt;
    bool active;
    esp_err_t cancel_error;
    /* Immutable until the native attempt is terminal. */
    uint64_t peer;
    uint64_t opened_ms;
    uint64_t (*now_ms)(void *context);
    bool (*peer_is_current_authenticated)(uint64_t peer, void *context);
    void *authority_context;
} esp_openclaw_setup_gateway_t;

typedef struct {
    bool lease_held;
    bool operation_pending;
    esp_openclaw_setup_token_t token;
    esp_openclaw_node_connect_attempt_t attempt;
} esp_openclaw_setup_gateway_snapshot_t;

bool esp_openclaw_setup_gateway_init(esp_openclaw_setup_gateway_t *binding,
    esp_openclaw_setup_session_t *session, esp_openclaw_node_handle_t node);
/* Acquire control after local confirmation without changing the current link. */
esp_err_t esp_openclaw_setup_gateway_activate(esp_openclaw_setup_gateway_t *binding,
                                             uint64_t generation);
/* Wi-Fi must have a live IP. The native owner validates/copies the setup code;
 * this binding retains no credentials. Uses session auth and deduplication.
 * Synchronous rejection returns an ACCEPTED but FAILED receipt. */
esp_openclaw_setup_result_t esp_openclaw_setup_gateway_submit(
    esp_openclaw_setup_gateway_t *binding, uint64_t generation, uint64_t peer,
    uint32_t operation_id, const char *setup_code, esp_openclaw_setup_receipt_t *receipt);
/* Forward matching session cancellation here. Sets native cancellation under
 * the node lock before returning; an already completed request is left alone. */
void esp_openclaw_setup_gateway_cancel(esp_openclaw_setup_gateway_t *binding,
                                      esp_openclaw_setup_token_t token);
/* Poll through CLOSING. Complete only exact terminal results, then release the
 * lease when the session closes. released is required and resets on each call;
 * when true, wake any automatic reconnect owner suppressed during setup.
 * API/ownership errors leave the pending operation and lease held. */
esp_err_t esp_openclaw_setup_gateway_poll(esp_openclaw_setup_gateway_t *binding,
                                        bool *released);
/* Local metadata only. Retains the final attempt after receipts are cleared,
 * including credentials_saved; cancellation does not mean rollback. */
esp_openclaw_setup_gateway_snapshot_t esp_openclaw_setup_gateway_snapshot(
    const esp_openclaw_setup_gateway_t *binding);

#ifdef __cplusplus
}
#endif
