/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/** @brief Snapshot of the example Wi-Fi station state. */
typedef struct {
    bool has_saved_network; /**< Whether station credentials are currently stored in NVS. */
    bool connected; /**< Whether the station currently has a live connection and IP. */
    char ssid[33]; /**< Saved station SSID, when configured. */
    char ip[16]; /**< Current IPv4 address in dotted-quad form when connected. */
    char netmask[16]; /**< Current IPv4 netmask in dotted-quad form when connected. */
    char gateway[16]; /**< Current IPv4 gateway in dotted-quad form when connected. */
    int8_t rssi; /**< Current RSSI from `esp_wifi_sta_get_ap_info()` when connected. */
    uint8_t channel; /**< Current primary Wi-Fi channel when connected. */
    uint8_t authmode; /**< Current AP auth mode reported by ESP-IDF when connected. */
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

/** @brief Disconnect the Wi-Fi station if it is currently connected. */
esp_err_t esp_openclaw_node_wifi_disconnect(void);

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
