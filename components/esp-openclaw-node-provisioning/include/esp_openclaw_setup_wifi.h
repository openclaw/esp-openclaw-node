#pragma once

#include "esp_openclaw_node_wifi.h"
#include "esp_openclaw_setup_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A cancelled Wi-Fi operation is FAILED with this detail, distinct from ESP
 * driver errors. It describes a drained cancellation, not credential rollback. */
#define ESP_OPENCLAW_SETUP_WIFI_CANCELLED (-2)

/* Initialize once per boot. All calls belong to the session's serialized owner
 * task, outside the default event loop. Treat fields as private. This binding
 * exclusively completes the WIFI_APPLY tokens it dispatches. */
typedef struct {
    esp_openclaw_setup_session_t *session;
    uint64_t generation;
    esp_openclaw_node_wifi_setup_lease_t lease;
    esp_openclaw_setup_token_t token;
    esp_openclaw_node_wifi_attempt_t attempt;
    bool active;
    bool cancel_requested;
} esp_openclaw_setup_wifi_t;

typedef struct {
    bool lease_held;
    bool operation_pending;
    esp_openclaw_setup_token_t token;
    esp_openclaw_node_wifi_attempt_t attempt;
} esp_openclaw_setup_wifi_snapshot_t;

bool esp_openclaw_setup_wifi_init(esp_openclaw_setup_wifi_t *binding,
                                 esp_openclaw_setup_session_t *session);
/* Call after local confirmation. Acquires admission without changing the link.
 * The lease remains held between operations until the session closes. */
esp_err_t esp_openclaw_setup_wifi_activate(esp_openclaw_setup_wifi_t *binding,
                                         uint64_t generation);
/* Uses session authentication, confirmation and deduplication. Credentials are
 * copied directly into the Wi-Fi owner; the binding retains no credential copy.
 * A synchronous rejection returns an ACCEPTED but FAILED receipt. */
esp_openclaw_setup_result_t esp_openclaw_setup_wifi_submit(
    esp_openclaw_setup_wifi_t *binding, uint64_t generation, uint64_t peer,
    uint32_t operation_id, const char *ssid, const char *passphrase,
    esp_openclaw_setup_receipt_t *receipt);
/* Forward the session cancel callback here for WIFI_APPLY. This only records a
 * matching cancellation: authorization can invoke the callback during polling.
 * Do not reenter any other binding or Wi-Fi API from session callbacks. */
void esp_openclaw_setup_wifi_cancel(esp_openclaw_setup_wifi_t *binding,
                                   esp_openclaw_setup_token_t token);
/* Poll regularly, including after session close. Only drained terminal attempts
 * complete receipts; RECOVERY_REQUIRED remains pending. Returns driver/API or
 * release/reconnect errors to the owner without claiming completion. */
esp_err_t esp_openclaw_setup_wifi_poll(esp_openclaw_setup_wifi_t *binding);
/* Explicit cleanup retry, also available while the session is CLOSING. */
esp_err_t esp_openclaw_setup_wifi_retry_drain(esp_openclaw_setup_wifi_t *binding);
/* Local UI snapshot; no polling or peer authorization. Retains the final attempt
 * after close so credentials_saved and the drain outcome remain inspectable. */
esp_openclaw_setup_wifi_snapshot_t esp_openclaw_setup_wifi_snapshot(
    const esp_openclaw_setup_wifi_t *binding);

#ifdef __cplusplus
}
#endif
