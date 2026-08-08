/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_openclaw_node_internal.h"

#include <inttypes.h>

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

void esp_openclaw_node_complete_connect_failed(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_connect_failure_reason_t reason,
    esp_err_t local_err,
    const char *gateway_detail_code,
    bool stop_client)
{
    esp_openclaw_node_cleanup_transport_instance(node, stop_client);
    esp_openclaw_node_lock_state(node);
    node->state = ESP_OPENCLAW_NODE_INTERNAL_IDLE;
    esp_openclaw_node_clear_pending_control_locked(node);
    esp_openclaw_node_unlock_state(node);
    esp_openclaw_node_emit_connect_failed(
        node,
        reason,
        local_err,
        gateway_detail_code);
}

void esp_openclaw_node_complete_disconnected(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_disconnected_reason_t reason,
    esp_err_t local_err,
    bool stop_client)
{
    esp_openclaw_node_cleanup_transport_instance(node, stop_client);
    esp_openclaw_node_lock_state(node);
    node->state = ESP_OPENCLAW_NODE_INTERNAL_IDLE;
    esp_openclaw_node_clear_pending_control_locked(node);
    esp_openclaw_node_unlock_state(node);
    esp_openclaw_node_fail_pending_requests(
        node,
        "DISCONNECTED",
        "Gateway connection closed");
    esp_openclaw_node_emit_disconnected(node, reason, local_err);
}

void esp_openclaw_node_fail_if_connect_timed_out(esp_openclaw_node_handle_t node)
{
    esp_openclaw_node_lock_state(node);
    bool connecting = node->state == ESP_OPENCLAW_NODE_INTERNAL_CONNECTING;
    int64_t connect_started_ms = node->connect_started_ms;
    esp_openclaw_node_unlock_state(node);

    if (!connecting || connect_started_ms <= 0) {
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000LL;
    int64_t waited_ms = now_ms - connect_started_ms;
    if (waited_ms < ESP_OPENCLAW_NODE_CONNECT_TIMEOUT_MS) {
        return;
    }

    ESP_LOGW(
        ESP_OPENCLAW_NODE_TAG,
        "timed out waiting for connect completion after %" PRId64 " ms",
        waited_ms);
    esp_openclaw_node_complete_connect_failed(
        node,
        ESP_OPENCLAW_NODE_CONNECT_FAILURE_TRANSPORT_START_FAILED,
        ESP_ERR_TIMEOUT,
        NULL,
        true);
}

esp_err_t esp_openclaw_node_enqueue_work_message(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_work_message_t *message)
{
    if (node->work_queue == NULL) {
        esp_openclaw_node_free_work_message_payload(message);
        return ESP_FAIL;
    }
    if (xQueueSend(node->work_queue, message, 0) != pdTRUE) {
        esp_openclaw_node_free_work_message_payload(message);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void esp_openclaw_node_enqueue_work_message_from_callback(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_work_message_t *message)
{
    bool accept = false;

    esp_openclaw_node_lock_state(node);
    accept = esp_openclaw_node_should_accept_callback_transport_id_locked(
        node,
        message->transport_id);
    esp_openclaw_node_unlock_state(node);
    if (!accept) {
        esp_openclaw_node_free_work_message_payload(message);
        return;
    }
    if (esp_openclaw_node_enqueue_work_message(node, message) != ESP_OK) {
        ESP_LOGW(
            ESP_OPENCLAW_NODE_TAG,
            "dropping websocket work item due to full queue");
    }
}

static void complete_transport_event_state_after_disconnect_locked(
    esp_openclaw_node_handle_t node)
{
    esp_openclaw_node_clear_session_wait_state_locked(node);
    node->transport_connected = false;
    node->client_started = false;
}

static void handle_request_connect(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_work_message_t *message)
{
    esp_openclaw_node_lock_state(node);
    esp_openclaw_node_internal_state_t state = node->state;
    if (state == ESP_OPENCLAW_NODE_INTERNAL_DESTROYING ||
        state == ESP_OPENCLAW_NODE_INTERNAL_CLOSED) {
        esp_openclaw_node_unlock_state(node);
        return;
    }
    if (state != ESP_OPENCLAW_NODE_INTERNAL_IDLE) {
        /* Saved-session requests are reconnect-loop traffic: reserved
         * idle-only, but a preempting explicit request can slip in before
         * this message is processed. Drop it rather than let auto-reconnect
         * cancel an operator-issued attempt; the loop retries on the next
         * terminal event. */
        if (message->connect_source.kind ==
            ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SAVED_SESSION) {
            esp_openclaw_node_unlock_state(node);
            return;
        }
        /* An explicit request reserved over a live attempt/session: tear the
         * old one down first so its terminal event fires before the new
         * attempt starts. */
        esp_openclaw_node_unlock_state(node);
        if (esp_openclaw_node_state_is_connecting(state)) {
            esp_openclaw_node_complete_connect_failed(
                node,
                ESP_OPENCLAW_NODE_CONNECT_FAILURE_CANCELED,
                ESP_OK,
                NULL,
                true);
        } else {
            esp_openclaw_node_complete_disconnected(
                node,
                ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED,
                ESP_OK,
                true);
        }
        esp_openclaw_node_lock_state(node);
        if (node->state != ESP_OPENCLAW_NODE_INTERNAL_IDLE) {
            esp_openclaw_node_unlock_state(node);
            return;
        }
    }
    esp_openclaw_node_clear_pending_control_locked(node);
    esp_openclaw_node_clear_connect_source_struct(&node->active_connect_source);
    node->active_connect_source = message->connect_source;
    memset(&message->connect_source, 0, sizeof(message->connect_source));
    node->state = ESP_OPENCLAW_NODE_INTERNAL_CONNECTING;
    node->connect_started_ms = esp_timer_get_time() / 1000LL;
    esp_openclaw_node_unlock_state(node);

    esp_err_t err = esp_openclaw_node_start_transport_for_active_source(node);
    if (err != ESP_OK) {
        esp_openclaw_node_complete_connect_failed(
            node,
            ESP_OPENCLAW_NODE_CONNECT_FAILURE_TRANSPORT_START_FAILED,
            err,
            NULL,
            false);
    }
}

static void handle_request_disconnect(esp_openclaw_node_handle_t node)
{
    esp_openclaw_node_lock_state(node);
    bool request_pending =
        node->pending_control == ESP_OPENCLAW_NODE_PENDING_CONTROL_REQUEST_DISCONNECT;
    bool was_connecting = esp_openclaw_node_state_is_connecting(node->state);
    bool has_transport =
        node->ws != NULL || node->active_transport_id != 0;
    esp_openclaw_node_unlock_state(node);

    if (!request_pending) {
        /* The session/attempt this targeted already terminated on its own;
         * complete_* cleared the reservation and emitted the terminal event. */
        return;
    }

    /* A canceled connect attempt is a CONNECT_FAILED terminal, not
     * DISCONNECTED: callers wait for exactly one outcome per accepted
     * connect request. */
    if (was_connecting) {
        esp_openclaw_node_complete_connect_failed(
            node,
            ESP_OPENCLAW_NODE_CONNECT_FAILURE_CANCELED,
            ESP_OK,
            NULL,
            has_transport);
        return;
    }

    esp_openclaw_node_complete_disconnected(
        node,
        ESP_OPENCLAW_NODE_DISCONNECTED_REASON_REQUESTED,
        ESP_OK,
        has_transport);
}

static void handle_request_gateway(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_work_message_t *message)
{
    esp_openclaw_node_send_gateway_request(
        node,
        message->method,
        message->params_json,
        message->request_cb,
        message->request_ctx);
}

static void handle_ws_connected(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_work_message_t *message)
{
    bool kick_challenge = false;

    esp_openclaw_node_lock_state(node);
    bool current_transport =
        node->active_transport_id == message->transport_id;
    esp_openclaw_node_internal_state_t state = node->state;
    if (current_transport) {
        node->transport_connected = true;
    }
    if (current_transport &&
        node->state == ESP_OPENCLAW_NODE_INTERNAL_CONNECTING) {
        kick_challenge = true;
    }
    esp_openclaw_node_unlock_state(node);

    if (!current_transport) {
        return;
    }

    if (kick_challenge) {
        esp_openclaw_node_send_challenge_kick_ping(node);
    }

    ESP_LOGI(
        ESP_OPENCLAW_NODE_TAG,
        "websocket connected: transport_id=%" PRIu32 " state=%s",
        message->transport_id,
        esp_openclaw_node_internal_state_name(state));
}

static void handle_ws_disconnected(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_work_message_t *message)
{
    esp_openclaw_node_lock_state(node);
    bool current_transport =
        node->active_transport_id == message->transport_id;
    esp_openclaw_node_internal_state_t state = node->state;
    if (current_transport) {
        complete_transport_event_state_after_disconnect_locked(node);
    }
    esp_openclaw_node_unlock_state(node);

    if (!current_transport) {
        return;
    }

    ESP_LOGW(
        ESP_OPENCLAW_NODE_TAG,
        "websocket disconnected: transport_id=%" PRIu32 " state=%s err=%s",
        message->transport_id,
        esp_openclaw_node_internal_state_name(state),
        esp_err_to_name(message->local_err));

    if (state == ESP_OPENCLAW_NODE_INTERNAL_READY) {
        esp_openclaw_node_complete_disconnected(
            node,
            ESP_OPENCLAW_NODE_DISCONNECTED_REASON_CONNECTION_LOST,
            message->local_err,
            false);
        return;
    }

    if (esp_openclaw_node_state_is_connecting(state)) {
        esp_openclaw_node_complete_connect_failed(
            node,
            ESP_OPENCLAW_NODE_CONNECT_FAILURE_CONNECTION_LOST,
            message->local_err,
            NULL,
            false);
    }
}

static void handle_ws_error(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_work_message_t *message)
{
    esp_openclaw_node_lock_state(node);
    bool current_transport =
        node->active_transport_id == message->transport_id;
    esp_openclaw_node_unlock_state(node);
    if (!current_transport) {
        return;
    }

    ESP_LOGW(
        ESP_OPENCLAW_NODE_TAG,
        "websocket error: transport_id=%" PRIu32 " state=%s err=%s",
        message->transport_id,
        esp_openclaw_node_internal_state_name(node->state),
        esp_err_to_name(message->local_err));
}

static void handle_ws_fatal(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_work_message_t *message)
{
    esp_openclaw_node_lock_state(node);
    bool current_transport = node->active_transport_id == message->transport_id;
    esp_openclaw_node_internal_state_t state = node->state;
    esp_openclaw_node_unlock_state(node);
    if (!current_transport) {
        return;
    }

    ESP_LOGE(
        ESP_OPENCLAW_NODE_TAG,
        "closing websocket after fatal inbound message error: %s",
        esp_err_to_name(message->local_err));
    if (esp_openclaw_node_state_is_connecting(state)) {
        esp_openclaw_node_complete_connect_failed(
            node,
            ESP_OPENCLAW_NODE_CONNECT_FAILURE_CONNECTION_LOST,
            message->local_err,
            NULL,
            true);
    } else if (state == ESP_OPENCLAW_NODE_INTERNAL_READY) {
        esp_openclaw_node_complete_disconnected(
            node,
            ESP_OPENCLAW_NODE_DISCONNECTED_REASON_CONNECTION_LOST,
            message->local_err,
            true);
    }
}

static void handle_shutdown_request(esp_openclaw_node_handle_t node)
{
    esp_openclaw_node_cleanup_transport_instance(node, true);
    esp_openclaw_node_drain_work_queue(node);

    esp_openclaw_node_lock_state(node);
    esp_openclaw_node_clear_data_buffer_locked(node);
    esp_openclaw_node_clear_pending_control_locked(node);
    node->state = ESP_OPENCLAW_NODE_INTERNAL_CLOSED;
    esp_openclaw_node_unlock_state(node);
    esp_openclaw_node_fail_pending_requests(
        node,
        "CANCELED",
        "OpenClaw node was destroyed");
}

static void exit_node_task(esp_openclaw_node_handle_t node)
{
    SemaphoreHandle_t destroy_done = NULL;

    esp_openclaw_node_lock_state(node);
    node->task_handle = NULL;
    destroy_done = node->destroy_done;
    esp_openclaw_node_unlock_state(node);

    if (destroy_done != NULL) {
        xSemaphoreGive(destroy_done);
    }
    vTaskDelete(NULL);
}

static bool process_work_message(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_work_message_t *message)
{
    switch (message->type) {
    case ESP_OPENCLAW_NODE_WORK_MSG_REQUEST_CONNECT:
        handle_request_connect(node, message);
        break;
    case ESP_OPENCLAW_NODE_WORK_MSG_REQUEST_DISCONNECT:
        handle_request_disconnect(node);
        break;
    case ESP_OPENCLAW_NODE_WORK_MSG_REQUEST_GATEWAY:
        handle_request_gateway(node, message);
        break;
    case ESP_OPENCLAW_NODE_WORK_MSG_WS_CONNECTED:
        handle_ws_connected(node, message);
        break;
    case ESP_OPENCLAW_NODE_WORK_MSG_WS_DISCONNECTED:
        handle_ws_disconnected(node, message);
        break;
    case ESP_OPENCLAW_NODE_WORK_MSG_WS_ERROR:
        handle_ws_error(node, message);
        break;
    case ESP_OPENCLAW_NODE_WORK_MSG_WS_FATAL:
        handle_ws_fatal(node, message);
        break;
    case ESP_OPENCLAW_NODE_WORK_MSG_DATA: {
        esp_openclaw_node_lock_state(node);
        bool current_transport =
            node->active_transport_id == message->transport_id &&
            node->state != ESP_OPENCLAW_NODE_INTERNAL_DESTROYING &&
            node->state != ESP_OPENCLAW_NODE_INTERNAL_CLOSED;
        esp_openclaw_node_unlock_state(node);
        if (current_transport && message->text != NULL) {
            esp_openclaw_node_process_gateway_message(node, message->text);
        }
        break;
    }
    case ESP_OPENCLAW_NODE_WORK_MSG_SHUTDOWN:
        handle_shutdown_request(node);
        return false;
    default:
        break;
    }
    return true;
}

void esp_openclaw_node_task(void *arg)
{
    esp_openclaw_node_handle_t node = (esp_openclaw_node_handle_t)arg;

    for (;;) {
        esp_openclaw_node_work_message_t fatal = {0};
        esp_openclaw_node_lock_state(node);
        fatal.transport_id = node->fatal_transport_id;
        fatal.local_err = node->fatal_transport_err;
        node->fatal_transport_id = 0;
        node->fatal_transport_err = ESP_OK;
        esp_openclaw_node_unlock_state(node);
        if (fatal.transport_id != 0) {
            fatal.type = ESP_OPENCLAW_NODE_WORK_MSG_WS_FATAL;
            (void)process_work_message(node, &fatal);
        }
        esp_openclaw_node_work_message_t message = {0};
        if (xQueueReceive(node->work_queue, &message, ESP_OPENCLAW_NODE_TASK_POLL_TICKS) != pdTRUE) {
            esp_openclaw_node_fail_if_connect_timed_out(node);
            continue;
        }

        do {
            bool keep_running = process_work_message(node, &message);
            esp_openclaw_node_free_work_message_payload(&message);
            if (!keep_running) {
                exit_node_task(node);
                return;
            }
            memset(&message, 0, sizeof(message));
        } while (xQueueReceive(node->work_queue, &message, 0) == pdTRUE);
    }
}

static void rollback_pending_control_locked(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_pending_control_request_t expected_request,
    esp_openclaw_node_pending_control_request_t rollback_op)
{
    if (node->pending_control == expected_request) {
        node->pending_control = rollback_op;
    }
}

static esp_err_t submit_pending_request(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_work_message_t *message,
    esp_openclaw_node_pending_control_request_t expected_request,
    esp_openclaw_node_pending_control_request_t rollback_op)
{
    esp_err_t err = esp_openclaw_node_enqueue_work_message(node, message);
    if (err != ESP_OK) {
        esp_openclaw_node_lock_state(node);
        rollback_pending_control_locked(
            node,
            expected_request,
            rollback_op);
        esp_openclaw_node_unlock_state(node);
    }
    return err;
}

esp_err_t esp_openclaw_node_submit_connect_request(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_connect_request_source_t *connect_source)
{
    esp_openclaw_node_lock_state(node);
    esp_err_t err =
        esp_openclaw_node_reserve_connect_request_locked(node, connect_source);
    esp_openclaw_node_unlock_state(node);
    if (err != ESP_OK) {
        return err;
    }

    esp_openclaw_node_work_message_t message = {
        .type = ESP_OPENCLAW_NODE_WORK_MSG_REQUEST_CONNECT,
        .connect_source = *connect_source,
    };
    memset(connect_source, 0, sizeof(*connect_source));

    err = submit_pending_request(
        node,
        &message,
        ESP_OPENCLAW_NODE_PENDING_CONTROL_REQUEST_CONNECT,
        ESP_OPENCLAW_NODE_PENDING_CONTROL_REQUEST_NONE);
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

static esp_err_t reserve_disconnect_request_locked(esp_openclaw_node_handle_t node)
{
    if (node->state == ESP_OPENCLAW_NODE_INTERNAL_DESTROYING ||
        node->state == ESP_OPENCLAW_NODE_INTERNAL_CLOSED) {
        return ESP_ERR_INVALID_STATE;
    }

    /* CONNECTING is accepted as a cancel: without it, a client retrying an
     * unreachable gateway is uncontrollable until its connect timeout. */
    if (node->state != ESP_OPENCLAW_NODE_INTERNAL_READY &&
        !esp_openclaw_node_state_is_connecting(node->state)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (node->pending_control != ESP_OPENCLAW_NODE_PENDING_CONTROL_REQUEST_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_openclaw_node_set_pending_control_locked(
        node,
        ESP_OPENCLAW_NODE_PENDING_CONTROL_REQUEST_DISCONNECT);
    return ESP_OK;
}

esp_err_t esp_openclaw_node_submit_disconnect_request(esp_openclaw_node_handle_t node)
{
    esp_openclaw_node_lock_state(node);
    esp_err_t err = reserve_disconnect_request_locked(node);
    esp_openclaw_node_unlock_state(node);
    if (err != ESP_OK) {
        return err;
    }

    esp_openclaw_node_work_message_t message = {
        .type = ESP_OPENCLAW_NODE_WORK_MSG_REQUEST_DISCONNECT,
    };
    err = submit_pending_request(
        node,
        &message,
        ESP_OPENCLAW_NODE_PENDING_CONTROL_REQUEST_DISCONNECT,
        ESP_OPENCLAW_NODE_PENDING_CONTROL_REQUEST_NONE);
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

esp_err_t esp_openclaw_node_submit_gateway_request(
    esp_openclaw_node_handle_t node,
    const char *method,
    const char *params_json,
    esp_openclaw_node_gateway_request_cb_t callback,
    void *user_ctx)
{
    esp_openclaw_node_lock_state(node);
    bool ready = node->state == ESP_OPENCLAW_NODE_INTERNAL_READY &&
                 node->pending_control == ESP_OPENCLAW_NODE_PENDING_CONTROL_REQUEST_NONE;
    esp_openclaw_node_unlock_state(node);
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_openclaw_node_work_message_t message = {
        .type = ESP_OPENCLAW_NODE_WORK_MSG_REQUEST_GATEWAY,
        .method = esp_openclaw_node_duplicate_string(method),
        .params_json = esp_openclaw_node_duplicate_string(
            params_json != NULL ? params_json : "{}"),
        .request_cb = callback,
        .request_ctx = user_ctx,
    };
    if (message.method == NULL || message.params_json == NULL) {
        esp_openclaw_node_free_work_message_payload(&message);
        return ESP_ERR_NO_MEM;
    }
    return esp_openclaw_node_enqueue_work_message(node, &message);
}
