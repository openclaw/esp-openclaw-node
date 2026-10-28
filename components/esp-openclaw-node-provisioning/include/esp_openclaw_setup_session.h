#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_OPENCLAW_SETUP_RECEIPT_LIMIT 32
#define ESP_OPENCLAW_SETUP_TIMEOUT_MS UINT64_C(120000)

typedef enum {
    ESP_OPENCLAW_SETUP_CLOSED,
    ESP_OPENCLAW_SETUP_DISCOVERABLE,
    ESP_OPENCLAW_SETUP_COMPARISON,
    ESP_OPENCLAW_SETUP_READY,
    ESP_OPENCLAW_SETUP_BUSY,
    ESP_OPENCLAW_SETUP_CLOSING,
} esp_openclaw_setup_phase_t;

typedef enum {
    ESP_OPENCLAW_SETUP_WIFI_APPLY,
    ESP_OPENCLAW_SETUP_GATEWAY_APPLY,
} esp_openclaw_setup_operation_t;

typedef enum {
    ESP_OPENCLAW_SETUP_PENDING,
    ESP_OPENCLAW_SETUP_SUCCEEDED,
    ESP_OPENCLAW_SETUP_FAILED,
    ESP_OPENCLAW_SETUP_UNKNOWN,
} esp_openclaw_setup_outcome_t;

typedef enum {
    ESP_OPENCLAW_SETUP_OK,
    ESP_OPENCLAW_SETUP_ACCEPTED,
    ESP_OPENCLAW_SETUP_DUPLICATE,
    ESP_OPENCLAW_SETUP_INVALID,
    ESP_OPENCLAW_SETUP_STALE,
    ESP_OPENCLAW_SETUP_UNAUTHORIZED,
    ESP_OPENCLAW_SETUP_NEEDS_CONFIRMATION,
    ESP_OPENCLAW_SETUP_OCCUPIED,
    ESP_OPENCLAW_SETUP_FULL,
    ESP_OPENCLAW_SETUP_NOT_FOUND,
    ESP_OPENCLAW_SETUP_EXHAUSTED,
} esp_openclaw_setup_result_t;

typedef struct {
    uint64_t generation;
    uint32_t operation_id;
} esp_openclaw_setup_token_t;

typedef struct {
    esp_openclaw_setup_token_t token;
    esp_openclaw_setup_operation_t operation;
    esp_openclaw_setup_outcome_t outcome;
    int32_t detail;
} esp_openclaw_setup_receipt_t;

typedef struct {
    uint64_t (*now_ms)(void *context);
    bool (*peer_is_current_authenticated)(uint64_t peer, void *context);
    void (*cancel)(esp_openclaw_setup_token_t token, void *context);
    void *context;
} esp_openclaw_setup_callbacks_t;

/* Dispatch copies any payload it needs; the session never retains credentials.
 * It must report exactly one terminal completion, including queue rejection.
 * ACCEPTED means dispatched, not connected or persisted. */
typedef void (*esp_openclaw_setup_dispatch_t)(esp_openclaw_setup_token_t token,
                                             esp_openclaw_setup_operation_t operation,
                                             void *payload_context);

/* Caller-owned storage. Initialize once per boot, not once per setup attempt.
 * All calls and side effects run on one serialized owner task. Treat fields as
 * private. Clock/auth callbacks must not reenter. Dispatch/cancel may only
 * reenter through complete(), including synchronous rejection or draining. */
typedef struct {
    esp_openclaw_setup_callbacks_t callbacks;
    uint64_t generation;
    uint64_t opened_ms;
    uint64_t peer;
    esp_openclaw_setup_phase_t phase;
    bool input_released;
    uint8_t receipt_count;
    esp_openclaw_setup_receipt_t receipts[ESP_OPENCLAW_SETUP_RECEIPT_LIMIT];
} esp_openclaw_setup_session_t;

typedef struct {
    uint64_t generation;
    esp_openclaw_setup_phase_t phase;
    uint8_t receipt_count;
    bool has_operation;
    esp_openclaw_setup_receipt_t operation;
} esp_openclaw_setup_snapshot_t;

bool esp_openclaw_setup_init(esp_openclaw_setup_session_t *session,
                             const esp_openclaw_setup_callbacks_t *callbacks);
esp_openclaw_setup_result_t esp_openclaw_setup_open(esp_openclaw_setup_session_t *session,
                                                   uint64_t *generation);
/* peer identifies one live encrypted transport session, never a device address.
 * Attach only after authenticating and binding the displayed comparison to it. */
esp_openclaw_setup_result_t esp_openclaw_setup_attach(esp_openclaw_setup_session_t *session,
                                                     uint64_t generation, uint64_t peer);
/* Current encrypted peer, including comparison before local confirmation.
 * Permits transport exchange only; mutations and saved-state reads still need
 * confirmation. Polls live authority on the serialized setup owner. */
esp_openclaw_setup_result_t esp_openclaw_setup_authorize_peer(esp_openclaw_setup_session_t *session,
                                                             uint64_t generation, uint64_t peer);
/* Local UI only: observe neutral input AFTER displaying this peer's comparison,
 * then confirm on a new physical press. Never expose either call over transport. */
esp_openclaw_setup_result_t esp_openclaw_setup_input_released(esp_openclaw_setup_session_t *session,
                                                             uint64_t generation);
esp_openclaw_setup_result_t esp_openclaw_setup_confirm(esp_openclaw_setup_session_t *session,
                                                      uint64_t generation);
esp_openclaw_setup_result_t esp_openclaw_setup_submit(esp_openclaw_setup_session_t *session,
    uint64_t generation, uint64_t peer, uint32_t operation_id,
    esp_openclaw_setup_operation_t operation, esp_openclaw_setup_dispatch_t dispatch,
    void *payload_context, esp_openclaw_setup_receipt_t *receipt);
esp_openclaw_setup_result_t esp_openclaw_setup_get_receipt(esp_openclaw_setup_session_t *session,
    uint64_t generation, uint64_t peer, uint32_t operation_id,
    esp_openclaw_setup_receipt_t *receipt);
/* Required immediately before a queued/awaited operation applies a side effect,
 * on the owner task with no intervening yield. A token alone is not authority. */
bool esp_openclaw_setup_authorize_operation(esp_openclaw_setup_session_t *session,
                                           esp_openclaw_setup_token_t token);
/* Terminal means the worker has drained, including all possible late effects.
 * UNKNOWN records a drained operation whose external outcome is unresolved. */
bool esp_openclaw_setup_complete(esp_openclaw_setup_session_t *session,
    esp_openclaw_setup_token_t token, esp_openclaw_setup_outcome_t outcome, int32_t detail);
/* Local exit. Pending work keeps the session CLOSING until its
 * matching completion; cancel must request drain even if rollback is impossible. */
void esp_openclaw_setup_close(esp_openclaw_setup_session_t *session);
/* Transport disconnect event: ignore other peers and old generations. */
bool esp_openclaw_setup_detach(esp_openclaw_setup_session_t *session,
                              uint64_t generation, uint64_t peer);
void esp_openclaw_setup_poll(esp_openclaw_setup_session_t *session);
/* Local UI metadata only; not an unauthenticated wire response. Polls first. */
esp_openclaw_setup_snapshot_t esp_openclaw_setup_snapshot(esp_openclaw_setup_session_t *session);

#ifdef __cplusplus
}
#endif
