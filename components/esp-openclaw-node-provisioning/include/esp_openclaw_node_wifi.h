/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"

#define ESP_OPENCLAW_NODE_WIFI_SCAN_LIMIT 16

/* Credential changes, connect/disconnect, and scans serialize their driver
 * calls. A competing explicit operation returns ESP_ERR_WIFI_STATE without
 * changing credentials or reconnect policy. Status reads remain available. */

/** @brief Snapshot of the example Wi-Fi station state. */
typedef struct {
    bool has_saved_network; /**< Whether station credentials are currently stored in NVS. */
    bool connected; /**< Whether the station currently has a live connection and IP. */
    bool ap_info_valid; /**< Whether RSSI, channel, and auth mode were read successfully. */
    char ssid[33]; /**< Saved station SSID, when configured. */
    char ip[16]; /**< Current IPv4 address in dotted-quad form when connected. */
    char netmask[16]; /**< Current IPv4 netmask in dotted-quad form when connected. */
    char gateway[16]; /**< Current IPv4 gateway in dotted-quad form when connected. */
    int8_t rssi; /**< Current RSSI when ap_info_valid is true. */
    uint8_t channel; /**< Current primary Wi-Fi channel when ap_info_valid is true. */
    uint8_t authmode; /**< Current ESP-IDF AP auth mode when ap_info_valid is true. */
    uint8_t last_disconnect_reason; /**< Last ESP-IDF disconnect reason code, or `0` when clear. */
} esp_openclaw_node_wifi_status_t;

/**
 * @brief Initialize the example Wi-Fi station support.
 *
 * The helper restores any previously stored station credentials from NVS and
 * starts the ESP-IDF station interface.
 */
esp_err_t esp_openclaw_node_wifi_start(void);

/**
 * @brief Persist Wi-Fi station credentials and start connecting immediately.
 *
 * Passing `NULL` for @p passphrase is treated the same as an empty string.
 * The helper rejects values that do not fit the ESP-IDF station config fields
 * exactly, instead of truncating them before storage.
 */
esp_err_t esp_openclaw_node_wifi_set_credentials(const char *ssid, const char *passphrase);

/**
 * @brief Remove any persisted Wi-Fi station credentials.
 *
 * This helper clears the example's station configuration from flash and
 * restarts the station in an unconfigured state.
 */
esp_err_t esp_openclaw_node_wifi_clear_credentials(void);

/** @brief Start or resume a Wi-Fi station connection attempt using saved credentials. */
esp_err_t esp_openclaw_node_wifi_connect(void);

/**
 * @brief Disconnect the Wi-Fi station if it is currently connected.
 *
 * Clears connection status and its waiter signal together. Queued IP events
 * cannot restore them until a saved-network connection is enabled again.
 * This does not promise asynchronous driver/event draining.
 */
esp_err_t esp_openclaw_node_wifi_disconnect(void);

/**
 * @brief Scan nearby networks while disconnected and not reconnecting.
 *
 * Run after startup without credentials, or after an explicit disconnect.
 * @p count is the capacity of @p records on entry (1 to SCAN_LIMIT), and the
 * number returned on success. @p total includes results beyond that capacity.
 * Returns ESP_ERR_WIFI_STATE while connecting, connected, or already scanning.
 * Credentials and reconnect policy are not changed.
 */
esp_err_t esp_openclaw_node_wifi_scan(wifi_ap_record_t *records, uint16_t *count, uint16_t *total);

/** Opaque, nonzero admission lease, unique within this boot. Not authentication. */
typedef uint64_t esp_openclaw_node_wifi_setup_lease_t;

/**
 * @brief Reserve Wi-Fi command admission for an authorized setup owner.
 *
 * Rejects competing explicit operations and automatic reconnect driver calls.
 * While held, ordinary mutations/scans return ESP_ERR_WIFI_STATE and automatic
 * reconnect requests are deferred. Status reads remain available. Acquisition
 * preserves credentials, reconnect policy, and the live link; it does not stop
 * or drain a previously accepted asynchronous join or queued driver events.
 * The caller must separately enforce live peer/local confirmation authority.
 * On failure, @p lease is unchanged.
 */
esp_err_t esp_openclaw_node_wifi_setup_acquire(esp_openclaw_node_wifi_setup_lease_t *lease);

/**
 * @brief Release matching ownership after its scan or setup attempt has finished.
 *
 * A stale lease returns ESP_ERR_INVALID_STATE; an active scan or nonterminal attempt returns
 * ESP_ERR_WIFI_STATE. Neither releases ownership nor changes @p reconnect_result.
 * Success releases ownership and replays at most one deferred reconnect when
 * still desired and disconnected. The return value describes lease release;
 * @p reconnect_result separately receives the immediate driver result (ESP_OK
 * if no retry was needed). It does not report asynchronous connection success.
 * Both the lease and @p reconnect_result must be nonzero/non-NULL respectively.
 */
esp_err_t esp_openclaw_node_wifi_setup_release(
    esp_openclaw_node_wifi_setup_lease_t lease, esp_err_t *reconnect_result);

/**
 * @brief Run the same bounded blocking scan under matching setup ownership.
 *
 * Uses the ordinary scan's buffers, limits, and connection restrictions. It
 * never disconnects a live link or cancels a join to make scanning possible.
 * Zero leases are invalid arguments; stale leases return ESP_ERR_INVALID_STATE.
 * Release is rejected until the blocking operation returns, including failure.
 */
esp_err_t esp_openclaw_node_wifi_setup_scan(
    esp_openclaw_node_wifi_setup_lease_t lease,
    wifi_ap_record_t *records, uint16_t *count, uint16_t *total);

#define ESP_OPENCLAW_NODE_WIFI_ATTEMPT_TIMEOUT_MS UINT64_C(30000)

typedef enum {
    ESP_OPENCLAW_NODE_WIFI_PREPARING,
    ESP_OPENCLAW_NODE_WIFI_JOINING,
    ESP_OPENCLAW_NODE_WIFI_DRAINING,
    ESP_OPENCLAW_NODE_WIFI_RECOVERY_REQUIRED,
    ESP_OPENCLAW_NODE_WIFI_SUCCEEDED,
    ESP_OPENCLAW_NODE_WIFI_FAILED,
    ESP_OPENCLAW_NODE_WIFI_CANCELLED,
} esp_openclaw_node_wifi_attempt_state_t;

typedef struct {
    uint64_t id;
    esp_openclaw_node_wifi_attempt_state_t state;
    esp_err_t error;
    esp_err_t recovery_error;
    uint8_t disconnect_reason;
    bool credentials_saved;
} esp_openclaw_node_wifi_attempt_t;

/** Start one native-radio join under the lease; does not yet change the radio.
 * Calls below belong to one serialized owner task, outside the default event
 * loop. Poll regularly to progress. A successful begin means queued, not joined.
 * Native stop/deinit plus two event-loop barriers fence older join/IP events
 * before applying credentials. Remote Wi-Fi backends return NOT_SUPPORTED.
 * IDs are boot-unique; old IDs cannot operate on a replacement attempt. */
esp_err_t esp_openclaw_node_wifi_setup_join_begin(
    esp_openclaw_node_wifi_setup_lease_t lease, const char *ssid,
    const char *passphrase, uint64_t *attempt_id);

/** Revalidate the live peer, setup generation and local confirmation. Called
 * only on the owner task; must not reenter any Wi-Fi attempt API. */
typedef bool (*esp_openclaw_node_wifi_authorize_t)(void *context);

/** Progress one step and copy the result. Authorization is checked before
 * teardown and immediately before credential/connection side effects.
 * Loss of authority cancels and drains. Only SUCCEEDED/FAILED/CANCELLED are
 * terminal; a timeout or driver/barrier failure is never proof of drain.
 * RECOVERY_REQUIRED retains the lease until explicit retry succeeds.
 * After a started failure/cancel, saved credentials remain but automatic
 * reconnect stays disabled. A successful link transfers to ordinary ownership.
 * Outputs are unchanged for an invalid lease/ID. */
esp_err_t esp_openclaw_node_wifi_setup_join_poll(
    esp_openclaw_node_wifi_setup_lease_t lease, uint64_t attempt_id,
    esp_openclaw_node_wifi_authorize_t authorize, void *context,
    esp_openclaw_node_wifi_attempt_t *result);

/** Request cancellation. Keep polling until terminal; does not release lease. */
esp_err_t esp_openclaw_node_wifi_setup_join_cancel(
    esp_openclaw_node_wifi_setup_lease_t lease, uint64_t attempt_id);

/** Retry a failed drain explicitly, with a fresh barrier generation. */
esp_err_t esp_openclaw_node_wifi_setup_join_retry_drain(
    esp_openclaw_node_wifi_setup_lease_t lease, uint64_t attempt_id);

/**
 * @brief Wait for the station to connect.
 *
 * @param[in] timeout_ticks Maximum time to wait, in FreeRTOS ticks.
 *
 * @return `true` if the station connected before the timeout, otherwise `false`.
 */
bool esp_openclaw_node_wifi_wait_for_connection(TickType_t timeout_ticks);

/** @brief Return whether the Wi-Fi station is currently connected and has an IP. */
bool esp_openclaw_node_wifi_is_connected(void);

/**
 * @brief Copy the current Wi-Fi station status into @p status.
 *
 * @param[out] status Destination status structure to populate.
 */
void esp_openclaw_node_wifi_get_status(esp_openclaw_node_wifi_status_t *status);
