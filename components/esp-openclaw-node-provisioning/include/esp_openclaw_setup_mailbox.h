#pragma once

#include "esp_openclaw_setup_dispatcher.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "protocomm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { ESP_OPENCLAW_MAILBOX_EMPTY, ESP_OPENCLAW_MAILBOX_QUEUED,
    ESP_OPENCLAW_MAILBOX_PROCESSING, ESP_OPENCLAW_MAILBOX_READY } esp_openclaw_setup_mailbox_phase_t;

/* One transient exchange, NOT an operation/receipt cache. Caller-owned storage,
 * init once before callbacks; fields private. Slot lock never covers mutable
 * session/transfer/dispatch calls, allocation or waits. No code holding the
 * security guard lock enters this lock. */
typedef struct {
    portMUX_TYPE lock;
    esp_openclaw_setup_transfer_t *transfer;
    esp_openclaw_setup_dispatcher_t *dispatcher;
    uint64_t (*now_ms)(void *context);
    void *context;
    uint64_t generation, peer, opened_ms;
    bool open, delivered, reply_pending;
    esp_openclaw_setup_mailbox_phase_t phase;
    size_t request_size, reply_size;
    uint8_t request[ESP_OPENCLAW_SETUP_FRAME_SIZE_MAX], reply[ESP_OPENCLAW_SETUP_FRAME_SIZE_MAX];
} esp_openclaw_setup_mailbox_t;

bool esp_openclaw_setup_mailbox_init(esp_openclaw_setup_mailbox_t *mailbox,
    esp_openclaw_setup_transfer_t *transfer, esp_openclaw_setup_dispatcher_t *dispatcher,
    uint64_t (*now_ms)(void *context), void *context);
/* Serialized owner: arm with peer0 BEFORE BLE and the canonical opened_ms fact.
 * First authenticated frames get PENDING with no retained input until owner
 * attach/transfer open and a one-time same-generation bind to current peer.
 * Bind must preserve opened_ms exactly; no replacing a bound peer. A fresh
 * open requires a newer generation. Clock callback is bounded/thread-safe,
 * shares the session clock and must not reenter setup APIs. */
bool esp_openclaw_setup_mailbox_open(esp_openclaw_setup_mailbox_t *mailbox,
    uint64_t generation, uint64_t peer, uint64_t opened_ms);
/* Any task: exact generation+peer only. Revoke synchronously; true means slot
 * drained. A PROCESSING slot wipes when its bounded transfer phase returns.
 * Server exit must revoke the SDK guard BEFORE awaiting stop, including while
 * armed/unbound. Stop/drain SDK callbacks and polling before destroying storage. */
bool esp_openclaw_setup_mailbox_close(esp_openclaw_setup_mailbox_t *mailbox,
    uint64_t generation, uint64_t peer);
/* Actual protocomm endpoint callback. Copies input before return. PENDING is
 * not an ACK: retry IDENTICAL plaintext frame, including for PULL. Successful
 * output belongs to SDK; its subsequent crypto checks live peer authority. */
esp_err_t esp_openclaw_setup_mailbox_endpoint(uint32_t session_id, const uint8_t *input,
    ssize_t size, uint8_t **output, ssize_t *output_size, void *context);
/* One serialized owner call per regular setup tick. Publish canonical frame
 * reply BEFORE dispatch, which may block for a scan. Keep dispatcher_poll()
 * running separately through BLE exit/native drain. Never call on BLE host. */
void esp_openclaw_setup_mailbox_poll(esp_openclaw_setup_mailbox_t *mailbox);

#ifdef __cplusplus
}
#endif
