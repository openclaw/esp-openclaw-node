/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_openclaw_node_internal.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "esp_check.h"
#include "esp_log.h"

static void websocket_event_handler(
    void *handler_args,
    esp_event_base_t base,
    int32_t event_id,
    void *event_data);

static void enqueue_fatal_message_error(
    esp_openclaw_node_handle_t node,
    uint32_t transport_id,
    const char *reason)
{
    ESP_LOGE(ESP_OPENCLAW_NODE_TAG, "%s", reason);
    esp_openclaw_node_lock_state(node);
    if (esp_openclaw_node_should_accept_callback_transport_id_locked(
            node, transport_id)) {
        node->fatal_transport_id = transport_id;
        node->fatal_transport_err = ESP_ERR_INVALID_SIZE;
    }
    esp_openclaw_node_unlock_state(node);
    esp_openclaw_node_work_message_t message = {
        .type = ESP_OPENCLAW_NODE_WORK_MSG_WS_FATAL,
        .transport_id = transport_id,
        .local_err = ESP_ERR_INVALID_SIZE,
    };
    esp_openclaw_node_enqueue_work_message_from_callback(node, &message);
}

bool esp_openclaw_node_should_accept_callback_transport_id_locked(
    esp_openclaw_node_handle_t node,
    uint32_t transport_id)
{
    return node->state != ESP_OPENCLAW_NODE_INTERNAL_DESTROYING &&
           node->state != ESP_OPENCLAW_NODE_INTERNAL_CLOSED &&
           node->active_transport_id == transport_id &&
           transport_id != 0;
}

esp_err_t esp_openclaw_node_validate_tls_preflight(
    const esp_openclaw_node_config_t *config,
    const char *gateway_uri)
{
    if (gateway_uri != NULL && strncmp(gateway_uri, "wss://", 6) == 0) {
        if (config->tls_cert_pem == NULL && !config->use_cert_bundle) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_OK;
}

static void clear_active_transport_fields_locked(esp_openclaw_node_handle_t node)
{
    node->ws = NULL;
    node->transport_connected = false;
    node->client_started = false;
    node->active_transport_id = 0;
    esp_openclaw_node_clear_session_wait_state_locked(node);
    esp_openclaw_node_clear_connect_source_struct(&node->active_connect_source);
}

void esp_openclaw_node_cleanup_transport_instance(
    esp_openclaw_node_handle_t node,
    bool stop_client)
{
    esp_websocket_client_handle_t ws = NULL;
    esp_openclaw_node_transport_event_ctx_t *transport_ctx = NULL;
    char *gateway_uri = NULL;
    char *plugin_surface_urls_json = NULL;
    bool client_started = false;

    esp_openclaw_node_lock_state(node);
    ws = node->ws;
    transport_ctx = node->transport_ctx;
    gateway_uri = node->transport_gateway_uri;
    plugin_surface_urls_json = node->plugin_surface_urls_json;
    client_started = node->client_started;
    node->transport_ctx = NULL;
    node->transport_gateway_uri = NULL;
    node->plugin_surface_urls_json = NULL;
    clear_active_transport_fields_locked(node);
    esp_openclaw_node_unlock_state(node);

    if (ws != NULL) {
        if (stop_client && client_started) {
            node->websocket_client_ops->client_stop(ws);
        }
        node->websocket_client_ops->client_destroy(ws);
    }

    free(transport_ctx);
    free(gateway_uri);
    free(plugin_surface_urls_json);

    esp_openclaw_node_lock_state(node);
    esp_openclaw_node_clear_data_buffer_locked(node);
    esp_openclaw_node_unlock_state(node);
}

esp_err_t esp_openclaw_node_start_transport_for_active_source(
    esp_openclaw_node_handle_t node)
{
    const char *gateway_uri = NULL;
    switch (node->active_connect_source.kind) {
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SAVED_SESSION:
        gateway_uri =
            esp_openclaw_node_trimmed_or_null(node->persisted_session.gateway_uri);
        break;
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_BOOTSTRAP_TOKEN:
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_DEVICE_TOKEN:
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SHARED_TOKEN:
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_PASSWORD:
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_NO_AUTH:
        gateway_uri =
            esp_openclaw_node_trimmed_or_null(node->active_connect_source.gateway_uri);
        break;
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_NONE:
    default:
        return ESP_ERR_INVALID_STATE;
    }

    if (gateway_uri == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(
        esp_openclaw_node_validate_tls_preflight(&node->config, gateway_uri),
        ESP_OPENCLAW_NODE_TAG,
        "TLS preflight failed");

    char *gateway_uri_copy = esp_openclaw_node_duplicate_string(gateway_uri);
    if (gateway_uri_copy == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_openclaw_node_transport_event_ctx_t *transport_ctx =
        calloc(1, sizeof(*transport_ctx));
    if (transport_ctx == NULL) {
        free(gateway_uri_copy);
        return ESP_ERR_NO_MEM;
    }

    esp_websocket_client_config_t ws_config = {
        .uri = gateway_uri_copy,
        .disable_auto_reconnect = true,
        .network_timeout_ms = 10000,
        .ping_interval_sec = ESP_OPENCLAW_NODE_WS_PING_INTERVAL_SEC,
        .pingpong_timeout_sec = ESP_OPENCLAW_NODE_WS_PINGPONG_TIMEOUT_SEC,
        .keep_alive_enable = true,
        .keep_alive_idle = 5,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
        .task_prio = ESP_OPENCLAW_NODE_TRANSPORT_TASK_PRIORITY,
        .task_stack = ESP_OPENCLAW_NODE_TRANSPORT_TASK_STACK_SIZE,
        .buffer_size = ESP_OPENCLAW_NODE_TRANSPORT_BUFFER_SIZE,
        .user_context = transport_ctx,
    };
    if (strncmp(gateway_uri_copy, "wss://", 6) == 0) {
        if (node->config.tls_cert_pem != NULL) {
            ws_config.cert_pem = node->config.tls_cert_pem;
            ws_config.cert_len = node->config.tls_cert_len;
        } else if (node->config.use_cert_bundle) {
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
            ws_config.crt_bundle_attach = esp_crt_bundle_attach;
#else
            free(gateway_uri_copy);
            free(transport_ctx);
            return ESP_ERR_INVALID_STATE;
#endif
        }
        if (node->config.tls_common_name != NULL &&
            node->config.tls_common_name[0] != '\0') {
            ws_config.cert_common_name = node->config.tls_common_name;
        }
        ws_config.skip_cert_common_name_check =
            node->config.skip_cert_common_name_check;
    }

    esp_websocket_client_handle_t ws = node->websocket_client_ops->client_init(&ws_config);
    if (ws == NULL) {
        free(gateway_uri_copy);
        free(transport_ctx);
        return ESP_FAIL;
    }

    uint32_t transport_id = 0;
    esp_openclaw_node_lock_state(node);
    transport_id = ++node->next_transport_id;
    transport_ctx->node = node;
    transport_ctx->transport_id = transport_id;
    node->transport_ctx = transport_ctx;
    node->transport_gateway_uri = gateway_uri_copy;
    node->active_transport_id = transport_id;
    node->ws = ws;
    node->client_started = false;
    node->transport_connected = false;
    esp_openclaw_node_clear_session_wait_state_locked(node);
    esp_openclaw_node_unlock_state(node);

    esp_err_t err = node->websocket_client_ops->register_events(
        ws,
        WEBSOCKET_EVENT_ANY,
        websocket_event_handler,
        transport_ctx);
    if (err != ESP_OK) {
        esp_openclaw_node_cleanup_transport_instance(node, false);
        return err;
    }

    err = node->websocket_client_ops->client_start(ws);
    if (err != ESP_OK) {
        esp_openclaw_node_cleanup_transport_instance(node, false);
        return err;
    }

    esp_openclaw_node_lock_state(node);
    node->client_started = true;
    esp_openclaw_node_unlock_state(node);

    ESP_LOGI(
        ESP_OPENCLAW_NODE_TAG,
        "node connect attempt started for %s",
        gateway_uri_copy);
    return ESP_OK;
}

void esp_openclaw_node_send_challenge_kick_ping(esp_openclaw_node_handle_t node)
{
    esp_websocket_client_handle_t ws = NULL;

    /*
     * Some ESP-IDF websocket runtimes do not drain the first post-upgrade text
     * frame until the client transmits something. A zero-length ping forces the
     * read loop to wake and flush the already-buffered connect.challenge frame.
     */
    esp_openclaw_node_lock_state(node);
    ws = node->ws;
    esp_openclaw_node_unlock_state(node);

    if (ws == NULL) {
        return;
    }

    int written = node->websocket_client_ops->send_with_opcode(
        ws,
        WS_TRANSPORT_OPCODES_PING,
        NULL,
        0,
        pdMS_TO_TICKS(1000));
    if (written < 0) {
        ESP_LOGW(
            ESP_OPENCLAW_NODE_TAG,
            "failed sending websocket ping to prompt connect.challenge delivery");
    }
}

static esp_err_t local_err_from_ws_event(
    const esp_websocket_event_data_t *data)
{
    if (data == NULL) {
        return ESP_OK;
    }
    if (data->error_handle.esp_tls_last_esp_err != 0) {
        return data->error_handle.esp_tls_last_esp_err;
    }
    if (data->error_handle.esp_transport_sock_errno != 0) {
        return data->error_handle.esp_transport_sock_errno;
    }
    return ESP_OK;
}

static void websocket_event_handler(
    void *handler_args,
    esp_event_base_t base,
    int32_t event_id,
    void *event_data)
{
    (void)base;

    esp_openclaw_node_transport_event_ctx_t *transport_ctx =
        (esp_openclaw_node_transport_event_ctx_t *)handler_args;
    if (transport_ctx == NULL || transport_ctx->node == NULL) {
        return;
    }

    esp_openclaw_node_handle_t node = transport_ctx->node;
    esp_websocket_event_data_t *data =
        (esp_websocket_event_data_t *)event_data;

    esp_openclaw_node_lock_state(node);
    bool accept = esp_openclaw_node_should_accept_callback_transport_id_locked(
        node,
        transport_ctx->transport_id);
    esp_openclaw_node_internal_state_t state = node->state;
    esp_openclaw_node_unlock_state(node);
    if (!accept) {
        return;
    }

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        esp_openclaw_node_work_message_t message = {
            .type = ESP_OPENCLAW_NODE_WORK_MSG_WS_CONNECTED,
            .transport_id = transport_ctx->transport_id,
            .local_err = ESP_OK,
        };
        esp_openclaw_node_enqueue_work_message_from_callback(node, &message);
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED: {
        esp_openclaw_node_work_message_t message = {
            .type = ESP_OPENCLAW_NODE_WORK_MSG_WS_DISCONNECTED,
            .transport_id = transport_ctx->transport_id,
            .local_err = local_err_from_ws_event(data),
        };
        esp_openclaw_node_enqueue_work_message_from_callback(node, &message);
        break;
    }
    case WEBSOCKET_EVENT_ERROR: {
        esp_openclaw_node_work_message_t message = {
            .type = ESP_OPENCLAW_NODE_WORK_MSG_WS_ERROR,
            .transport_id = transport_ctx->transport_id,
            .local_err = local_err_from_ws_event(data),
        };
        esp_openclaw_node_enqueue_work_message_from_callback(node, &message);
        break;
    }
    case WEBSOCKET_EVENT_DATA:
        if (data == NULL) {
            enqueue_fatal_message_error(
                node,
                transport_ctx->transport_id,
                "websocket delivered a data event without metadata");
            break;
        }
        if (esp_openclaw_node_state_is_connecting(state)) {
            ESP_LOGD(
                ESP_OPENCLAW_NODE_TAG,
                "ws frame during connect: transport_id=%" PRIu32 " opcode=0x%x fin=%d data_len=%d payload_len=%d offset=%d state=%s",
                transport_ctx->transport_id,
                data != NULL ? data->op_code : 0,
                data != NULL ? data->fin : 0,
                data != NULL ? data->data_len : 0,
                data != NULL ? data->payload_len : 0,
                data != NULL ? data->payload_offset : 0,
                esp_openclaw_node_internal_state_name(state));
        }
        if (data->op_code == 0x08 || data->op_code == 0x09 ||
            data->op_code == 0x0a) {
            break;
        }
        if (data->op_code != 0x01 && data->op_code != 0x00) {
            esp_openclaw_node_lock_state(node);
            bool interrupts_text = node->rx_message_started;
            if (interrupts_text) esp_openclaw_node_clear_data_buffer_locked(node);
            esp_openclaw_node_unlock_state(node);
            if (interrupts_text) {
                enqueue_fatal_message_error(
                    node,
                    transport_ctx->transport_id,
                    "non-continuation data frame interrupted a fragmented text message");
                break;
            }
            ESP_LOGW(
                ESP_OPENCLAW_NODE_TAG,
                "ignoring unsupported websocket opcode=0x%x",
                data->op_code);
            break;
        }

        esp_openclaw_node_lock_state(node);
        if (!esp_openclaw_node_should_accept_callback_transport_id_locked(
                node,
                transport_ctx->transport_id)) {
            esp_openclaw_node_unlock_state(node);
            break;
        }

        bool malformed = false;
        bool oversized = false;
        if (data->payload_offset == 0) {
            /* payload_offset is per frame in esp_websocket_client. A TEXT
             * frame begins a message and each CONTINUATION frame extends it. */
            if (node->rx_frame_received != 0 || data->payload_len < 0 ||
                data->data_len < 0) {
                malformed = true;
            } else if (data->op_code == 0x01) {
                if (node->rx_message_started) {
                    malformed = true;
                } else {
                    esp_openclaw_node_clear_data_buffer_locked(node);
                    node->rx_message_started = true;
                }
            } else if (!node->rx_message_started) {
                malformed = true;
            }

            if (!malformed &&
                (size_t)data->payload_len >
                    ESP_OPENCLAW_NODE_MAX_INBOUND_MESSAGE_SIZE - node->rx_buffer_len) {
                oversized = true;
            }
            if (!malformed && !oversized) {
                size_t capacity = node->rx_buffer_len + (size_t)data->payload_len + 1U;
                char *grown = realloc(node->rx_buffer, capacity);
                if (grown == NULL) {
                    esp_openclaw_node_clear_data_buffer_locked(node);
                    esp_openclaw_node_unlock_state(node);
                    ESP_LOGE(ESP_OPENCLAW_NODE_TAG, "failed growing inbound message buffer");
                    break;
                }
                node->rx_buffer = grown;
                node->rx_buffer[node->rx_buffer_len] = '\0';
                node->rx_frame_len = (size_t)data->payload_len;
                node->rx_frame_received = 0;
                node->rx_frame_opcode = data->op_code;
                node->rx_frame_fin = data->fin;
            }
        } else if (data->payload_offset < 0 || data->payload_len < 0 ||
                   data->data_len < 0 || !node->rx_message_started ||
                   node->rx_frame_opcode != data->op_code ||
                   node->rx_frame_len != (size_t)data->payload_len ||
                   node->rx_frame_fin != data->fin ||
                   (size_t)data->payload_offset != node->rx_frame_received) {
            malformed = true;
        }

        if (!malformed && !oversized &&
            ((size_t)data->data_len > node->rx_frame_len - node->rx_frame_received ||
             (data->data_len > 0 && data->data_ptr == NULL))) {
            malformed = true;
        }

        if (malformed || oversized) {
            esp_openclaw_node_clear_data_buffer_locked(node);
            esp_openclaw_node_unlock_state(node);
            enqueue_fatal_message_error(
                node,
                transport_ctx->transport_id,
                oversized
                    ? "assembled inbound websocket message exceeds configured limit"
                    : "malformed websocket continuation ordering");
            break;
        }

        if (data->data_len > 0) {
            memcpy(
                node->rx_buffer + node->rx_buffer_len,
                data->data_ptr,
                (size_t)data->data_len);
        }
        node->rx_buffer_len += (size_t)data->data_len;
        node->rx_frame_received += (size_t)data->data_len;
        node->rx_buffer[node->rx_buffer_len] = '\0';

        if (node->rx_frame_received == node->rx_frame_len) {
            bool message_complete = node->rx_frame_fin;
            node->rx_frame_len = 0;
            node->rx_frame_received = 0;
            node->rx_frame_opcode = 0;
            node->rx_frame_fin = false;
            if (message_complete) {
                node->rx_message_started = false;
            esp_openclaw_node_work_message_t message = {
                .type = ESP_OPENCLAW_NODE_WORK_MSG_DATA,
                .transport_id = transport_ctx->transport_id,
                .text = node->rx_buffer,
            };
            node->rx_buffer = NULL;
            node->rx_buffer_len = 0;
            esp_openclaw_node_unlock_state(node);
            esp_openclaw_node_enqueue_work_message_from_callback(node, &message);
                break;
            }
        }
        esp_openclaw_node_unlock_state(node);
        break;
    default:
        break;
    }
}
