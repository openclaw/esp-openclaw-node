#pragma once

#include "esp_openclaw_setup_gateway.h"
#include "esp_openclaw_setup_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_openclaw_setup_runtime *esp_openclaw_setup_runtime_handle_t;
typedef enum { ESP_OPENCLAW_SETUP_RUNTIME_CLOSED, ESP_OPENCLAW_SETUP_RUNTIME_OPENING,
    ESP_OPENCLAW_SETUP_RUNTIME_ACTIVE, ESP_OPENCLAW_SETUP_RUNTIME_CLOSING,
    ESP_OPENCLAW_SETUP_RUNTIME_ERROR } esp_openclaw_setup_runtime_phase_t;
typedef enum { ESP_OPENCLAW_SETUP_LOCAL_NEUTRAL, ESP_OPENCLAW_SETUP_LOCAL_CONFIRM,
    ESP_OPENCLAW_SETUP_LOCAL_RETRY } esp_openclaw_setup_local_action_t;

typedef struct {
    esp_openclaw_node_handle_t node;
    const char *label, *model; /* 1..64 UTF-8 bytes, copied at creation. */
    void (*gateway_reconnect)(void *context); /* Bounded wake, never reenter setup. */
    void *context;
} esp_openclaw_setup_runtime_config_t;

/* Trusted LOCAL presentation only. QR/pop are ephemeral credentials: never log,
 * publish in screenshots or send through a remote display/menu command. */
typedef struct {
    uint64_t request_id, peer;
    esp_openclaw_setup_runtime_phase_t phase;
    esp_err_t error;
    bool can_open;
    esp_openclaw_setup_snapshot_t session;
    esp_openclaw_setup_wifi_snapshot_t wifi;
    esp_openclaw_setup_gateway_snapshot_t gateway;
    char service[30], pop[33], qr[192], comparison[10];
} esp_openclaw_setup_runtime_snapshot_t;

/* Boot owner only, once. Creates one boot-lifetime task and initializes the
 * canonical owners once; no advertising until explicit local open. Node and
 * callback context must outlive the task. Unsupported hardware returns
 * ESP_ERR_NOT_SUPPORTED without starting a task or changing *runtime. Controller
 * modem sleep must be disabled (the shipped Watch/Dial default) for handshake
 * entropy while another owner can stop Wi-Fi. */
esp_err_t esp_openclaw_setup_runtime_create(const esp_openclaw_setup_runtime_config_t *config,
    esp_openclaw_setup_runtime_handle_t *runtime);
/* Nonblocking local commands. Queue-full is ESP_ERR_TIMEOUT, never replayed.
 * OPEN reserves a boot-unique request ID before the owner can generate a
 * canonical session generation. Output unchanged on rejection. */
esp_err_t esp_openclaw_setup_runtime_open(esp_openclaw_setup_runtime_handle_t runtime, uint64_t *request_id);
/* Matching request only: synchronously revokes authority and clears discovery,
 * even while owner is generating SRP/blocked in a scan. Cleanup is asynchronous.
 * No queue capacity is needed to revoke; the owner already polls active work. */
esp_err_t esp_openclaw_setup_runtime_cancel(esp_openclaw_setup_runtime_handle_t runtime, uint64_t request_id);
/* NEUTRAL/CONFIRM are physical local input only, after the same comparison was
 * painted and all relevant input drivers provided NEW released samples. A
 * subsequent fresh physical press confirms, never a synthesized UI event.
 * RETRY retries partial activation while READY or failed cleanup while closing.
 * All commands carry the exact request/generation/peer shown by the local UI. */
esp_err_t esp_openclaw_setup_runtime_action(esp_openclaw_setup_runtime_handle_t runtime,
    uint64_t request_id, uint64_t generation, uint64_t peer, esp_openclaw_setup_local_action_t action);
bool esp_openclaw_setup_runtime_snapshot(esp_openclaw_setup_runtime_handle_t runtime,
    esp_openclaw_setup_runtime_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
