#pragma once

#include "esp_openclaw_setup_frame.h"
#include "esp_openclaw_setup_session.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t generation;
    uint64_t peer;
    uint32_t transfer_id;
} esp_openclaw_setup_transfer_token_t;

typedef struct {
    esp_openclaw_setup_transfer_token_t token;
    const uint8_t *data;
    size_t size;
} esp_openclaw_setup_transfer_request_t;

typedef enum {
    ESP_OPENCLAW_TRANSFER_EMPTY,
    ESP_OPENCLAW_TRANSFER_RECEIVING,
    ESP_OPENCLAW_TRANSFER_PENDING,
    ESP_OPENCLAW_TRANSFER_READY,
    ESP_OPENCLAW_TRANSFER_FAILED,
} esp_openclaw_setup_transfer_phase_t;

/* Caller-owned storage, initialized once per boot. All calls run on the same
 * serialized owner as session; fields are private. Keep this 16 KiB object off
 * small task stacks. No allocation, application parsing or side effects here. */
typedef struct {
    esp_openclaw_setup_session_t *session;
    esp_openclaw_setup_transfer_token_t token;
    esp_openclaw_setup_transfer_phase_t phase;
    esp_openclaw_setup_frame_error_t error;
    uint16_t received, total, response_size;
    bool active;
    uint8_t request[ESP_OPENCLAW_SETUP_MESSAGE_SIZE_MAX];
    uint8_t response[ESP_OPENCLAW_SETUP_MESSAGE_SIZE_MAX];
} esp_openclaw_setup_transfer_t;

bool esp_openclaw_setup_transfer_init(esp_openclaw_setup_transfer_t *transfer,
                                     esp_openclaw_setup_session_t *session);
/* Bind once per setup generation after attach. A closed generation cannot be
 * reopened to reset transfer IDs; close/reopen the canonical setup session. */
bool esp_openclaw_setup_transfer_open(esp_openclaw_setup_transfer_t *transfer,
                                     uint64_t generation, uint64_t peer);
/* Wipes both complete buffers. Does not cancel native operations; the canonical
 * session owns their teardown/drain. Poll even when no frames arrive. */
void esp_openclaw_setup_transfer_close(esp_openclaw_setup_transfer_t *transfer);
bool esp_openclaw_setup_transfer_poll(esp_openclaw_setup_transfer_t *transfer);

/* Input, output and transfer storage must not overlap. Reply capacity must be
 * at least FRAME_SIZE_MAX. False leaves output arguments/bytes unchanged.
 * True returns an encoded ACK, response or error. A complete request is handed
 * off exactly once via request; otherwise request is empty. Its data borrows
 * storage until the next admitted transfer or close: async consumers must copy.
 * An ACK accepts bytes only. The application must decode, authorize and admit
 * operations before acting. Revalidate authority before any awaited delivery. */
bool esp_openclaw_setup_transfer_receive(esp_openclaw_setup_transfer_t *transfer,
    uint64_t generation, uint64_t peer, const uint8_t *input, size_t input_size,
    uint8_t *output, size_t capacity, size_t *output_size,
    esp_openclaw_setup_transfer_request_t *request);
/* Only the exact pending token may publish once. Response data must not overlap
 * transfer storage. Zero/null responses fail with INTERNAL, oversize with
 * TOO_LARGE; both leave a stable error to pull and return false. */
bool esp_openclaw_setup_transfer_complete(esp_openclaw_setup_transfer_t *transfer,
    esp_openclaw_setup_transfer_token_t token, const uint8_t *response, size_t size);

#ifdef __cplusplus
}
#endif
