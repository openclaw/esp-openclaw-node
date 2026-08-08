/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_openclaw_node_example_saved_session_reconnect.h"

#include "esp_openclaw_node_wifi.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "oc_reconnect";
static const char *DEFAULT_TASK_NAME = "esp_openclaw_node_reconnect";

static void esp_openclaw_node_example_saved_session_reconnect_task(void *arg)
{
    esp_openclaw_node_example_saved_session_reconnect_t *state = arg;
    const esp_openclaw_node_connect_request_t request = {
        .source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_SAVED_SESSION,
        .gateway_uri = NULL,
        .value = NULL,
    };

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (!esp_openclaw_node_wifi_wait_for_connection(portMAX_DELAY)) {
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));

        /* No has_saved_session precheck: it reads a possibly stale in-memory
         * snapshot, while the component reloads NVS on every saved-session
         * request and answers NOT_FOUND itself. */
        esp_err_t err = esp_openclaw_node_request_connect(state->node, &request);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "requested saved-session reconnect");
        } else if (err == ESP_ERR_NOT_FOUND) {
            ESP_LOGI(TAG, "saved-session reconnect skipped because no saved session is available");
        } else if (err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "saved-session reconnect request failed: %s", esp_err_to_name(err));
        }
    }
}

esp_err_t esp_openclaw_node_example_saved_session_reconnect_start(
    esp_openclaw_node_example_saved_session_reconnect_t *state,
    esp_openclaw_node_handle_t node,
    const char *task_name)
{
    if (state == NULL || node == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (state->task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    state->node = node;
    BaseType_t task_ok = xTaskCreate(
        esp_openclaw_node_example_saved_session_reconnect_task,
        task_name != NULL ? task_name : DEFAULT_TASK_NAME,
        4096,
        state,
        5,
        &state->task);
    if (task_ok != pdPASS) {
        state->node = NULL;
        state->task = NULL;
        return ESP_ERR_NO_MEM;
    }

    xTaskNotifyGive(state->task);
    return ESP_OK;
}

void esp_openclaw_node_example_saved_session_reconnect_handle_event(
    esp_openclaw_node_example_saved_session_reconnect_t *state,
    esp_openclaw_node_event_t event,
    const void *event_data)
{
    if (state == NULL || state->task == NULL) {
        return;
    }

    bool should_retry = false;
    if (event == ESP_OPENCLAW_NODE_EVENT_CONNECT_FAILED) {
        const esp_openclaw_node_connect_failed_event_t *failed = event_data;
        should_retry =
            failed != NULL &&
            (failed->reason == ESP_OPENCLAW_NODE_CONNECT_FAILURE_TRANSPORT_START_FAILED ||
             failed->reason == ESP_OPENCLAW_NODE_CONNECT_FAILURE_CONNECTION_LOST);
    } else if (event == ESP_OPENCLAW_NODE_EVENT_DISCONNECTED) {
        const esp_openclaw_node_disconnected_event_t *disconnected = event_data;
        should_retry =
            disconnected != NULL &&
            disconnected->reason == ESP_OPENCLAW_NODE_DISCONNECTED_REASON_CONNECTION_LOST;
    }

    if (should_retry) {
        xTaskNotifyGive(state->task);
    }
}
