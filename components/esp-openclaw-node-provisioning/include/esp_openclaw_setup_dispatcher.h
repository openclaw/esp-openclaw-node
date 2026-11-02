#pragma once

#include "esp_openclaw_setup_gateway.h"
#include "esp_openclaw_setup_response.h"
#include "esp_openclaw_setup_transfer.h"
#include "esp_openclaw_setup_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Supplies the SAME encrypted-peer comparison shown by the local UI. Borrows
 * label/model/challenge through handle(); never generates a second challenge.
 * The callback runs on the owner task and must not reenter setup APIs. */
typedef bool (*esp_openclaw_setup_hello_t)(uint64_t generation, uint64_t peer,
    esp_openclaw_setup_bytes_t *label, esp_openclaw_setup_bytes_t *model,
    const uint8_t **challenge, void *context);

/* Initialize once per boot with owners already initialized to the same session.
 * All calls belong to their single serialized owner task. Treat fields as
 * private. Keep this caller-owned scratch off small task stacks; it must not
 * overlap transfer storage. No receipt/effects/challenge cache lives here. */
typedef struct {
    esp_openclaw_setup_session_t *session;
    esp_openclaw_setup_transfer_t *transfer;
    esp_openclaw_setup_wifi_t *wifi;
    esp_openclaw_setup_gateway_t *gateway;
    esp_openclaw_setup_hello_t hello;
    void *context;
    esp_openclaw_setup_response_t response;
    uint8_t encoded[ESP_OPENCLAW_SETUP_RESPONSE_SIZE_MAX];
    union {
        char setup_code[ESP_OPENCLAW_SETUP_CODE_SIZE_MAX + 1];
        struct { char ssid[33], password[65]; } wifi;
        wifi_ap_record_t scan[ESP_OPENCLAW_SETUP_RESPONSE_SCAN_MAX];
    } scratch;
} esp_openclaw_setup_dispatcher_t;

bool esp_openclaw_setup_dispatcher_init(esp_openclaw_setup_dispatcher_t *dispatch,
    esp_openclaw_setup_session_t *session, esp_openclaw_setup_transfer_t *transfer,
    esp_openclaw_setup_wifi_t *wifi, esp_openclaw_setup_gateway_t *gateway,
    esp_openclaw_setup_hello_t hello, void *context);
/* Local owner: call after a new physical confirmation, never for every retry.
 * A partial activation retains its lease; retry while READY or close and poll. */
esp_err_t esp_openclaw_setup_dispatcher_activate(esp_openclaw_setup_dispatcher_t *dispatch, uint64_t generation);
/* Consumes only a current complete transfer handoff. Publishes admission/error
 * immediately; terminal outcomes require a NEW status transfer. False includes
 * intentional no-reply cancellation/revocation. Credential adaptations are
 * synchronously copied into native owners then wiped before return. */
bool esp_openclaw_setup_dispatcher_handle(esp_openclaw_setup_dispatcher_t *dispatch,
    const esp_openclaw_setup_transfer_request_t *request);
/* Forward the canonical session's cancel callback here, then poll through exit
 * until both bindings drain. Poll even with BLE closed; wake the application's
 * automatic Gateway reconnect owner when gateway_released is true. */
void esp_openclaw_setup_dispatcher_cancel(esp_openclaw_setup_dispatcher_t *dispatch, esp_openclaw_setup_token_t token);
esp_err_t esp_openclaw_setup_dispatcher_poll(esp_openclaw_setup_dispatcher_t *dispatch, bool *gateway_released);

#ifdef __cplusplus
}
#endif
