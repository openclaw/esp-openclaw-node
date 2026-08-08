/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_openclaw_node.h"

/**
 * @brief Example-owned saved-session reconnect helper state.
 *
 * The helper retries only the persisted `{ gateway_uri, device_token }`
 * reconnect path. It does not retry explicit setup-code, token, password, or
 * no-auth requests.
 */
typedef struct {
    esp_openclaw_node_handle_t node; /**< Node to reconnect. */
    TaskHandle_t task; /**< Background task that waits for Wi-Fi and reissues reconnects. */
} esp_openclaw_node_example_saved_session_reconnect_t;

/**
 * @brief Start the example saved-session reconnect helper.
 *
 * The helper requests one saved-session connect after Wi-Fi first comes up, and
 * then retries the saved session after retryable connection-loss events.
 *
 * @param[in,out] state Helper state to initialize.
 * @param[in] node Node to reconnect.
 * @param[in] task_name Optional FreeRTOS task name. Pass `NULL` to use the
 *            built-in default.
 *
 * @return
 *      - `ESP_OK` on success
 *      - `ESP_ERR_INVALID_ARG` if @p state or @p node is `NULL`
 *      - `ESP_ERR_INVALID_STATE` if the helper is already running
 *      - `ESP_ERR_NO_MEM` if the helper task could not be created
 */
esp_err_t esp_openclaw_node_example_saved_session_reconnect_start(
    esp_openclaw_node_example_saved_session_reconnect_t *state,
    esp_openclaw_node_handle_t node,
    const char *task_name);

/**
 * @brief Feed node terminal events into the saved-session reconnect helper.
 *
 * Call this from the example's `esp_openclaw_node_event_cb_t`. The helper retries
 * only retryable transport-loss outcomes and does not retry auth rejections.
 *
 * @param[in,out] state Helper state returned from
 *            @ref esp_openclaw_node_example_saved_session_reconnect_start.
 * @param[in] event Event type from the component callback.
 * @param[in] event_data Event payload pointer from the component callback.
 */
void esp_openclaw_node_example_saved_session_reconnect_handle_event(
    esp_openclaw_node_example_saved_session_reconnect_t *state,
    esp_openclaw_node_event_t event,
    const void *event_data);
