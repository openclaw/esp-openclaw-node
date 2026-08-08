/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_openclaw_node_identity.h"
#include "esp_openclaw_node.h"
#include "esp_openclaw_node_persisted_session.h"

#define ESP_OPENCLAW_NODE_TAG                         "esp_openclaw_node"
#define ESP_OPENCLAW_NODE_CONNECT_TIMEOUT_MS          12000LL
#define ESP_OPENCLAW_NODE_WS_PING_INTERVAL_SEC        5
#define ESP_OPENCLAW_NODE_WS_PINGPONG_TIMEOUT_SEC     10
#define ESP_OPENCLAW_NODE_TASK_POLL_TICKS             pdMS_TO_TICKS(250)
#define ESP_OPENCLAW_NODE_WORK_QUEUE_LENGTH           CONFIG_ESP_OPENCLAW_NODE_WORK_QUEUE_LENGTH
#define ESP_OPENCLAW_NODE_TASK_STACK_SIZE             CONFIG_ESP_OPENCLAW_NODE_TASK_STACK_SIZE
#define ESP_OPENCLAW_NODE_TASK_PRIORITY               CONFIG_ESP_OPENCLAW_NODE_TASK_PRIORITY
#define ESP_OPENCLAW_NODE_TRANSPORT_TASK_STACK_SIZE   \
    CONFIG_ESP_OPENCLAW_NODE_TRANSPORT_TASK_STACK_SIZE
#define ESP_OPENCLAW_NODE_TRANSPORT_TASK_PRIORITY     \
    CONFIG_ESP_OPENCLAW_NODE_TRANSPORT_TASK_PRIORITY
#define ESP_OPENCLAW_NODE_TRANSPORT_BUFFER_SIZE       \
    CONFIG_ESP_OPENCLAW_NODE_TRANSPORT_BUFFER_SIZE
#define ESP_OPENCLAW_NODE_MAX_INBOUND_MESSAGE_SIZE    \
    CONFIG_ESP_OPENCLAW_NODE_MAX_INBOUND_MESSAGE_SIZE
#define ESP_OPENCLAW_NODE_MAX_PENDING_REQUESTS        \
    CONFIG_ESP_OPENCLAW_NODE_MAX_PENDING_REQUESTS

typedef enum {
    ESP_OPENCLAW_NODE_INTERNAL_IDLE = 0,
    ESP_OPENCLAW_NODE_INTERNAL_CONNECTING,
    ESP_OPENCLAW_NODE_INTERNAL_READY,
    ESP_OPENCLAW_NODE_INTERNAL_DESTROYING,
    ESP_OPENCLAW_NODE_INTERNAL_CLOSED,
} esp_openclaw_node_internal_state_t;

typedef enum {
    ESP_OPENCLAW_NODE_PENDING_CONTROL_REQUEST_NONE = 0,
    ESP_OPENCLAW_NODE_PENDING_CONTROL_REQUEST_CONNECT,
    ESP_OPENCLAW_NODE_PENDING_CONTROL_REQUEST_DISCONNECT,
} esp_openclaw_node_pending_control_request_t;

typedef enum {
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_NONE = 0,
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SAVED_SESSION,
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_BOOTSTRAP_TOKEN,
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_DEVICE_TOKEN,
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SHARED_TOKEN,
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_PASSWORD,
    ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_NO_AUTH,
} esp_openclaw_node_connect_source_kind_t;

typedef struct {
    esp_openclaw_node_connect_source_kind_t kind;
    char *gateway_uri;
    char *secret;
} esp_openclaw_node_connect_request_source_t;

typedef struct {
    esp_openclaw_node_connect_source_kind_t kind;
    char *auth_value;
    const char *signature_token;
} esp_openclaw_node_connect_material_t;

typedef enum {
    ESP_OPENCLAW_NODE_WORK_MSG_REQUEST_CONNECT = 0,
    ESP_OPENCLAW_NODE_WORK_MSG_REQUEST_DISCONNECT,
    ESP_OPENCLAW_NODE_WORK_MSG_REQUEST_GATEWAY,
    ESP_OPENCLAW_NODE_WORK_MSG_WS_CONNECTED,
    ESP_OPENCLAW_NODE_WORK_MSG_WS_DISCONNECTED,
    ESP_OPENCLAW_NODE_WORK_MSG_WS_ERROR,
    ESP_OPENCLAW_NODE_WORK_MSG_WS_FATAL,
    ESP_OPENCLAW_NODE_WORK_MSG_DATA,
    ESP_OPENCLAW_NODE_WORK_MSG_SHUTDOWN,
} esp_openclaw_node_work_message_type_t;

typedef struct {
    esp_openclaw_node_work_message_type_t type;
    uint32_t transport_id; /* Tags the websocket instance that produced this message. */
    esp_err_t local_err;
    char *text;
    char *method;
    char *params_json;
    esp_openclaw_node_gateway_request_cb_t request_cb;
    void *request_ctx;
    esp_openclaw_node_connect_request_source_t connect_source;
} esp_openclaw_node_work_message_t;

typedef struct {
    bool in_use;
    char request_id[40];
    esp_openclaw_node_gateway_request_cb_t callback;
    void *user_ctx;
} esp_openclaw_node_pending_request_t;

typedef struct {
    char *name;
    esp_openclaw_node_command_handler_t handler;
    esp_openclaw_node_command_handler_v2_t handler_v2;
    void *context;
} esp_openclaw_node_registered_command_t;

typedef struct esp_openclaw_node_transport_event_ctx {
    struct esp_openclaw_node *node;
    uint32_t transport_id; /* Stable ID for one websocket transport lifetime. */
} esp_openclaw_node_transport_event_ctx_t;

typedef struct {
    esp_websocket_client_handle_t (*client_init)(const esp_websocket_client_config_t *config);
    esp_err_t (*register_events)(
        esp_websocket_client_handle_t client,
        esp_websocket_event_id_t event,
        esp_event_handler_t event_handler,
        void *event_handler_arg);
    esp_err_t (*client_start)(esp_websocket_client_handle_t client);
    esp_err_t (*client_stop)(esp_websocket_client_handle_t client);
    esp_err_t (*client_destroy)(esp_websocket_client_handle_t client);
    int (*send_text)(
        esp_websocket_client_handle_t client,
        const char *data,
        int len,
        TickType_t timeout);
    int (*send_with_opcode)(
        esp_websocket_client_handle_t client,
        ws_transport_opcodes_t opcode,
        const uint8_t *data,
        int len,
        TickType_t timeout);
} esp_openclaw_node_websocket_client_ops_t;

typedef enum {
    CONNECT_RESPONSE_OUTCOME_IGNORE = 0,
    CONNECT_RESPONSE_OUTCOME_CONNECTED,
    CONNECT_RESPONSE_OUTCOME_CONNECT_FAILED,
} connect_response_outcome_t;

typedef struct {
    connect_response_outcome_t outcome;
    esp_err_t err;
    esp_openclaw_node_internal_state_t state;
} connect_response_finalize_result_t;

struct esp_openclaw_node {
    QueueHandle_t work_queue;
    TaskHandle_t task_handle;
    SemaphoreHandle_t destroy_done;
    SemaphoreHandle_t state_lock;
    const esp_openclaw_node_websocket_client_ops_t *websocket_client_ops;
    esp_openclaw_node_identity_t identity;
    esp_openclaw_node_persisted_session_t persisted_session;
    esp_openclaw_node_config_t config;
    esp_openclaw_node_internal_state_t state;
    esp_openclaw_node_pending_control_request_t pending_control;
    esp_websocket_client_handle_t ws;
    esp_openclaw_node_transport_event_ctx_t *transport_ctx;
    uint32_t next_transport_id;
    uint32_t active_transport_id;
    uint32_t fatal_transport_id;
    esp_err_t fatal_transport_err;
    bool transport_connected; /* True after the active websocket transport reports CONNECTED. */
    bool client_started; /* True after client_start() succeeds; cleanup should stop before destroy. */
    char pending_connect_id[32];
    int64_t connect_started_ms;
    char *transport_gateway_uri;
    char *plugin_surface_urls_json;
    char *rx_buffer;
    size_t rx_buffer_len; /* Complete message bytes, including the current partial frame. */
    size_t rx_frame_len;
    size_t rx_frame_received;
    uint8_t rx_frame_opcode;
    bool rx_message_started;
    bool rx_frame_fin;
    esp_openclaw_node_connect_request_source_t active_connect_source;
    size_t capability_count;
    char *capabilities[ESP_OPENCLAW_NODE_MAX_CAPABILITIES];
    size_t scope_count;
    char *scopes[ESP_OPENCLAW_NODE_MAX_SCOPES];
    size_t command_count;
    esp_openclaw_node_registered_command_t commands[ESP_OPENCLAW_NODE_MAX_COMMANDS];
    uint64_t next_request_id;
    esp_openclaw_node_pending_request_t pending_requests[ESP_OPENCLAW_NODE_MAX_PENDING_REQUESTS];
};

extern const esp_openclaw_node_websocket_client_ops_t esp_openclaw_node_default_websocket_client_ops;

__attribute__((weak)) const esp_openclaw_node_websocket_client_ops_t *esp_openclaw_node_test_websocket_client_ops(void);

const char *esp_openclaw_node_firmware_version(void);
char *esp_openclaw_node_duplicate_string(const char *value);
const char *esp_openclaw_node_trimmed_or_null(const char *value);
bool esp_openclaw_node_is_valid_gateway_uri(const char *gateway_uri);

void esp_openclaw_node_lock_state(esp_openclaw_node_handle_t node);
void esp_openclaw_node_unlock_state(esp_openclaw_node_handle_t node);

esp_err_t esp_openclaw_node_copy_config(
    const esp_openclaw_node_config_t *src,
    esp_openclaw_node_config_t *dst);
void esp_openclaw_node_free_config_strings(esp_openclaw_node_config_t *config);

void esp_openclaw_node_clear_session_wait_state_locked(esp_openclaw_node_handle_t node);
void esp_openclaw_node_set_pending_control_locked(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_pending_control_request_t kind);
void esp_openclaw_node_clear_pending_control_locked(esp_openclaw_node_handle_t node);
bool esp_openclaw_node_saved_session_is_present_locked(
    const esp_openclaw_node_handle_t node);
bool esp_openclaw_node_state_is_connecting(esp_openclaw_node_internal_state_t state);
const char *esp_openclaw_node_internal_state_name(
    esp_openclaw_node_internal_state_t state);
void esp_openclaw_node_clear_data_buffer_locked(esp_openclaw_node_handle_t node);

void esp_openclaw_node_free_work_message_payload(esp_openclaw_node_work_message_t *message);
void esp_openclaw_node_drain_work_queue(esp_openclaw_node_handle_t node);

void esp_openclaw_node_emit_connected(esp_openclaw_node_handle_t node);
void esp_openclaw_node_emit_connect_failed(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_connect_failure_reason_t reason,
    esp_err_t local_err,
    const char *gateway_detail_code);
void esp_openclaw_node_emit_disconnected(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_disconnected_reason_t reason,
    esp_err_t local_err);

bool esp_openclaw_node_is_node_task_context(esp_openclaw_node_handle_t node);

void esp_openclaw_node_clear_connect_source_struct(
    esp_openclaw_node_connect_request_source_t *source);
esp_err_t esp_openclaw_node_build_connect_source_from_request(
    const esp_openclaw_node_connect_request_t *request,
    esp_openclaw_node_connect_request_source_t *out_source);
const char *esp_openclaw_node_connect_source_kind_name(
    esp_openclaw_node_connect_source_kind_t kind);
void esp_openclaw_node_free_connect_material(esp_openclaw_node_connect_material_t *material);
esp_err_t esp_openclaw_node_resolve_active_connect_material_locked(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_connect_material_t *material);
esp_err_t esp_openclaw_node_reserve_connect_request_locked(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_connect_request_source_t *source);

void esp_openclaw_node_cleanup_registry(esp_openclaw_node_handle_t node);
esp_openclaw_node_registered_command_t *esp_openclaw_node_find_command(
    esp_openclaw_node_handle_t node,
    const char *name);
esp_err_t esp_openclaw_node_dispatch_command(
    esp_openclaw_node_handle_t node,
    const char *command,
    const char *params_json,
    size_t params_len,
    const esp_openclaw_node_command_invocation_t *invocation,
    char **out_payload_json,
    const char **out_error_code,
    const char **out_error_message);
void esp_openclaw_node_add_registered_string_array(
    cJSON *parent,
    const char *name,
    char *const *items,
    size_t count);
void esp_openclaw_node_add_registered_command_array(
    cJSON *parent,
    const char *name,
    esp_openclaw_node_handle_t node);
esp_err_t esp_openclaw_node_register_capability_internal(
    esp_openclaw_node_handle_t node,
    const char *capability);
esp_err_t esp_openclaw_node_register_scope_internal(
    esp_openclaw_node_handle_t node,
    const char *scope);
esp_err_t esp_openclaw_node_register_command_internal(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_command_t *command);
esp_err_t esp_openclaw_node_register_command_v2_internal(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_command_v2_t *command);

esp_err_t esp_openclaw_node_validate_tls_preflight(
    const esp_openclaw_node_config_t *config,
    const char *gateway_uri);
esp_err_t esp_openclaw_node_start_transport_for_active_source(
    esp_openclaw_node_handle_t node);
void esp_openclaw_node_cleanup_transport_instance(
    esp_openclaw_node_handle_t node,
    bool stop_client);
bool esp_openclaw_node_should_accept_callback_transport_id_locked(
    esp_openclaw_node_handle_t node,
    uint32_t transport_id);
void esp_openclaw_node_send_challenge_kick_ping(esp_openclaw_node_handle_t node);

void esp_openclaw_node_process_gateway_message(
    esp_openclaw_node_handle_t node,
    const char *text);
void esp_openclaw_node_send_gateway_request(
    esp_openclaw_node_handle_t node,
    const char *method,
    const char *params_json,
    esp_openclaw_node_gateway_request_cb_t callback,
    void *user_ctx);
void esp_openclaw_node_fail_pending_requests(
    esp_openclaw_node_handle_t node,
    const char *code,
    const char *message);

void esp_openclaw_node_complete_connect_failed(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_connect_failure_reason_t reason,
    esp_err_t local_err,
    const char *gateway_detail_code,
    bool stop_client);
void esp_openclaw_node_complete_disconnected(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_disconnected_reason_t reason,
    esp_err_t local_err,
    bool stop_client);
void esp_openclaw_node_fail_if_connect_timed_out(esp_openclaw_node_handle_t node);
esp_err_t esp_openclaw_node_enqueue_work_message(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_work_message_t *message);
void esp_openclaw_node_enqueue_work_message_from_callback(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_work_message_t *message);
void esp_openclaw_node_task(void *arg);
esp_err_t esp_openclaw_node_submit_connect_request(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_connect_request_source_t *connect_source);
esp_err_t esp_openclaw_node_submit_disconnect_request(
    esp_openclaw_node_handle_t node);
esp_err_t esp_openclaw_node_submit_gateway_request(
    esp_openclaw_node_handle_t node,
    const char *method,
    const char *params_json,
    esp_openclaw_node_gateway_request_cb_t callback,
    void *user_ctx);
