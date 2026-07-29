/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_openclaw_node_internal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"

static const char *DEFAULT_PLATFORM = "esp32";
static const char *DEFAULT_DEVICE_FAMILY = "ESP32";
static const char *DEFAULT_DISPLAY_NAME = "OpenClaw ESP32";
static const char *DEFAULT_CLIENT_ID = "node-host";
static const char *DEFAULT_CLIENT_MODE = "node";
static const char *DEFAULT_ROLE = "node";
static const char *DEFAULT_LOCALE = "en-US";

const esp_openclaw_node_websocket_client_ops_t esp_openclaw_node_default_websocket_client_ops = {
    .client_init = esp_websocket_client_init,
    .register_events = esp_websocket_register_events,
    .client_start = esp_websocket_client_start,
    .client_stop = esp_websocket_client_stop,
    .client_destroy = esp_websocket_client_destroy,
    .send_text = esp_websocket_client_send_text,
    .send_with_opcode = esp_websocket_client_send_with_opcode,
};

__attribute__((weak)) const esp_openclaw_node_websocket_client_ops_t *esp_openclaw_node_test_websocket_client_ops(void)
{
    return NULL;
}

const char *esp_openclaw_node_firmware_version(void)
{
    return esp_app_get_description()->version;
}

char *esp_openclaw_node_duplicate_string(const char *value)
{
    if (value == NULL) {
        return NULL;
    }
    return strdup(value);
}

const char *esp_openclaw_node_trimmed_or_null(const char *value)
{
    if (value == NULL) {
        return NULL;
    }
    while (*value == ' ' || *value == '\t' || *value == '\r' ||
           *value == '\n') {
        ++value;
    }
    return value[0] != '\0' ? value : NULL;
}

bool esp_openclaw_node_is_valid_gateway_uri(const char *gateway_uri)
{
    const char *trimmed = esp_openclaw_node_trimmed_or_null(gateway_uri);
    if (trimmed == NULL) {
        return false;
    }
    return strncmp(trimmed, "ws://", 5) == 0 ||
           strncmp(trimmed, "wss://", 6) == 0;
}

void esp_openclaw_node_lock_state(esp_openclaw_node_handle_t node)
{
    if (node != NULL && node->state_lock != NULL) {
        xSemaphoreTakeRecursive(node->state_lock, portMAX_DELAY);
    }
}

void esp_openclaw_node_unlock_state(esp_openclaw_node_handle_t node)
{
    if (node != NULL && node->state_lock != NULL) {
        xSemaphoreGiveRecursive(node->state_lock);
    }
}

static esp_err_t copy_string_field(char **dst, const char *src, bool required)
{
    if (src == NULL || src[0] == '\0') {
        return required ? ESP_ERR_INVALID_ARG : ESP_OK;
    }
    *dst = esp_openclaw_node_duplicate_string(src);
    return *dst != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t copy_tls_cert_field(
    esp_openclaw_node_config_t *dst,
    const esp_openclaw_node_config_t *src)
{
    if (src->tls_cert_pem == NULL) {
        dst->tls_cert_pem = NULL;
        dst->tls_cert_len = 0;
        return ESP_OK;
    }

    size_t cert_len = src->tls_cert_len;
    if (cert_len == 0) {
        cert_len = strlen(src->tls_cert_pem);
    }

    char *cert_copy = calloc(cert_len + 1U, sizeof(char));
    if (cert_copy == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (cert_len > 0) {
        memcpy(cert_copy, src->tls_cert_pem, cert_len);
    }
    cert_copy[cert_len] = '\0';

    dst->tls_cert_pem = cert_copy;
    dst->tls_cert_len = src->tls_cert_len;
    return ESP_OK;
}

void esp_openclaw_node_free_config_strings(esp_openclaw_node_config_t *config)
{
    free((char *)config->display_name);
    free((char *)config->platform);
    free((char *)config->device_family);
    free((char *)config->client_id);
    free((char *)config->client_mode);
    free((char *)config->role);
    free((char *)config->model_identifier);
    free((char *)config->locale);
    free((char *)config->tls_common_name);
    free((char *)config->tls_cert_pem);
    memset(config, 0, sizeof(*config));
}

esp_err_t esp_openclaw_node_copy_config(
    const esp_openclaw_node_config_t *src,
    esp_openclaw_node_config_t *dst)
{
    const char *model_identifier =
        (src->model_identifier != NULL && src->model_identifier[0] != '\0')
            ? src->model_identifier
            : CONFIG_IDF_TARGET;
    const char *locale =
        (src->locale != NULL && src->locale[0] != '\0')
            ? src->locale
            : DEFAULT_LOCALE;

    memset(dst, 0, sizeof(*dst));
    ESP_RETURN_ON_ERROR(
        copy_string_field((char **)&dst->display_name, src->display_name, true),
        ESP_OPENCLAW_NODE_TAG,
        "display_name");
    ESP_RETURN_ON_ERROR(
        copy_string_field((char **)&dst->platform, src->platform, true),
        ESP_OPENCLAW_NODE_TAG,
        "platform");
    ESP_RETURN_ON_ERROR(
        copy_string_field(
            (char **)&dst->device_family,
            src->device_family,
            true),
        ESP_OPENCLAW_NODE_TAG,
        "device_family");
    ESP_RETURN_ON_ERROR(
        copy_string_field((char **)&dst->client_id, src->client_id, true),
        ESP_OPENCLAW_NODE_TAG,
        "client_id");
    ESP_RETURN_ON_ERROR(
        copy_string_field(
            (char **)&dst->client_mode,
            src->client_mode,
            true),
        ESP_OPENCLAW_NODE_TAG,
        "client_mode");
    ESP_RETURN_ON_ERROR(
        copy_string_field((char **)&dst->role, src->role, true),
        ESP_OPENCLAW_NODE_TAG,
        "role");
    ESP_RETURN_ON_ERROR(
        copy_string_field(
            (char **)&dst->model_identifier,
            model_identifier,
            false),
        ESP_OPENCLAW_NODE_TAG,
        "model_identifier");
    ESP_RETURN_ON_ERROR(
        copy_string_field((char **)&dst->locale, locale, false),
        ESP_OPENCLAW_NODE_TAG,
        "locale");
    ESP_RETURN_ON_ERROR(
        copy_string_field(
            (char **)&dst->tls_common_name,
            src->tls_common_name,
            false),
        ESP_OPENCLAW_NODE_TAG,
        "tls_common_name");
    ESP_RETURN_ON_ERROR(
        copy_tls_cert_field(dst, src),
        ESP_OPENCLAW_NODE_TAG,
        "tls_cert_pem");
    dst->use_cert_bundle = src->use_cert_bundle;
    dst->skip_cert_common_name_check = src->skip_cert_common_name_check;
    dst->event_cb = src->event_cb;
    dst->event_user_ctx = src->event_user_ctx;
    dst->gateway_event_cb = src->gateway_event_cb;
    dst->gateway_event_user_ctx = src->gateway_event_user_ctx;
    return ESP_OK;
}

void esp_openclaw_node_clear_session_wait_state_locked(esp_openclaw_node_handle_t node)
{
    node->pending_connect_id[0] = '\0';
    node->connect_started_ms = 0;
}

void esp_openclaw_node_set_pending_control_locked(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_pending_control_request_t kind)
{
    node->pending_control = kind;
}

void esp_openclaw_node_clear_pending_control_locked(esp_openclaw_node_handle_t node)
{
    esp_openclaw_node_set_pending_control_locked(
        node,
        ESP_OPENCLAW_NODE_PENDING_CONTROL_REQUEST_NONE);
}

bool esp_openclaw_node_saved_session_is_present_locked(
    const esp_openclaw_node_handle_t node)
{
    return esp_openclaw_node_persisted_session_is_present(&node->persisted_session);
}

bool esp_openclaw_node_state_is_connecting(esp_openclaw_node_internal_state_t state)
{
    return state == ESP_OPENCLAW_NODE_INTERNAL_CONNECTING;
}

const char *esp_openclaw_node_internal_state_name(
    esp_openclaw_node_internal_state_t state)
{
    switch (state) {
    case ESP_OPENCLAW_NODE_INTERNAL_IDLE:
        return "idle";
    case ESP_OPENCLAW_NODE_INTERNAL_CONNECTING:
        return "connecting";
    case ESP_OPENCLAW_NODE_INTERNAL_READY:
        return "ready";
    case ESP_OPENCLAW_NODE_INTERNAL_DESTROYING:
        return "destroying";
    case ESP_OPENCLAW_NODE_INTERNAL_CLOSED:
        return "closed";
    default:
        return "unknown";
    }
}

void esp_openclaw_node_clear_data_buffer_locked(esp_openclaw_node_handle_t node)
{
    free(node->rx_buffer);
    node->rx_buffer = NULL;
    node->rx_buffer_len = 0;
}

void esp_openclaw_node_free_work_message_payload(esp_openclaw_node_work_message_t *message)
{
    if (message == NULL) {
        return;
    }
    free(message->text);
    message->text = NULL;
    free(message->method);
    message->method = NULL;
    free(message->params_json);
    message->params_json = NULL;
    esp_openclaw_node_clear_connect_source_struct(&message->connect_source);
}

void esp_openclaw_node_drain_work_queue(esp_openclaw_node_handle_t node)
{
    esp_openclaw_node_work_message_t message = {0};
    while (node->work_queue != NULL &&
           xQueueReceive(node->work_queue, &message, 0) == pdTRUE) {
        esp_openclaw_node_free_work_message_payload(&message);
    }
}

static void emit_event(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_event_t event,
    const void *event_data)
{
    esp_openclaw_node_event_cb_t event_cb = NULL;
    void *event_user_ctx = NULL;

    esp_openclaw_node_lock_state(node);
    event_cb = node->config.event_cb;
    event_user_ctx = node->config.event_user_ctx;
    esp_openclaw_node_unlock_state(node);

    if (event_cb != NULL) {
        event_cb(node, event, event_data, event_user_ctx);
    }
}

void esp_openclaw_node_emit_connected(esp_openclaw_node_handle_t node)
{
    emit_event(node, ESP_OPENCLAW_NODE_EVENT_CONNECTED, NULL);
}

void esp_openclaw_node_emit_connect_failed(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_connect_failure_reason_t reason,
    esp_err_t local_err,
    const char *gateway_detail_code)
{
    esp_openclaw_node_connect_failed_event_t event = {
        .reason = reason,
        .local_err = local_err,
        .gateway_detail_code = gateway_detail_code,
    };
    emit_event(node, ESP_OPENCLAW_NODE_EVENT_CONNECT_FAILED, &event);
}

void esp_openclaw_node_emit_disconnected(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_disconnected_reason_t reason,
    esp_err_t local_err)
{
    esp_openclaw_node_disconnected_event_t event = {
        .reason = reason,
        .local_err = local_err,
    };
    emit_event(node, ESP_OPENCLAW_NODE_EVENT_DISCONNECTED, &event);
}

bool esp_openclaw_node_is_node_task_context(esp_openclaw_node_handle_t node)
{
    return node != NULL && node->task_handle != NULL &&
           xTaskGetCurrentTaskHandle() == node->task_handle;
}

void esp_openclaw_node_config_init_default(esp_openclaw_node_config_t *config)
{
    if (config == NULL) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->display_name = DEFAULT_DISPLAY_NAME;
    config->platform = DEFAULT_PLATFORM;
    config->device_family = DEFAULT_DEVICE_FAMILY;
    config->client_id = DEFAULT_CLIENT_ID;
    config->client_mode = DEFAULT_CLIENT_MODE;
    config->role = DEFAULT_ROLE;
    config->model_identifier = CONFIG_IDF_TARGET;
    config->locale = DEFAULT_LOCALE;
    config->use_cert_bundle = true;
}

esp_err_t esp_openclaw_node_create(
    const esp_openclaw_node_config_t *config,
    esp_openclaw_node_handle_t *out_node)
{
    if (config == NULL || out_node == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_openclaw_node_handle_t node = calloc(1, sizeof(*node));
    if (node == NULL) {
        return ESP_ERR_NO_MEM;
    }

    node->state_lock = xSemaphoreCreateRecursiveMutex();
    if (node->state_lock == NULL) {
        free(node);
        return ESP_ERR_NO_MEM;
    }

    node->destroy_done = xSemaphoreCreateBinary();
    if (node->destroy_done == NULL) {
        vSemaphoreDelete(node->state_lock);
        free(node);
        return ESP_ERR_NO_MEM;
    }

    node->work_queue = xQueueCreate(
        ESP_OPENCLAW_NODE_WORK_QUEUE_LENGTH,
        sizeof(esp_openclaw_node_work_message_t));
    if (node->work_queue == NULL) {
        vSemaphoreDelete(node->destroy_done);
        vSemaphoreDelete(node->state_lock);
        free(node);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_openclaw_node_copy_config(config, &node->config);
    if (err != ESP_OK) {
        esp_openclaw_node_free_config_strings(&node->config);
        vQueueDelete(node->work_queue);
        vSemaphoreDelete(node->destroy_done);
        vSemaphoreDelete(node->state_lock);
        free(node);
        return err;
    }

    err = esp_openclaw_node_identity_load_or_create(&node->identity);
    if (err != ESP_OK) {
        esp_openclaw_node_free_config_strings(&node->config);
        vQueueDelete(node->work_queue);
        vSemaphoreDelete(node->destroy_done);
        vSemaphoreDelete(node->state_lock);
        free(node);
        return err;
    }

    err = esp_openclaw_node_persisted_session_load(
        node->config.role,
        &node->persisted_session);
    if (err != ESP_OK) {
        esp_openclaw_node_identity_free(&node->identity);
        esp_openclaw_node_free_config_strings(&node->config);
        vQueueDelete(node->work_queue);
        vSemaphoreDelete(node->destroy_done);
        vSemaphoreDelete(node->state_lock);
        free(node);
        return err;
    }

    const esp_openclaw_node_websocket_client_ops_t *test_websocket_client_ops =
        esp_openclaw_node_test_websocket_client_ops();
    node->websocket_client_ops = test_websocket_client_ops != NULL
        ? test_websocket_client_ops
        : &esp_openclaw_node_default_websocket_client_ops;
    node->state = ESP_OPENCLAW_NODE_INTERNAL_IDLE;

    BaseType_t task_ok = xTaskCreate(
        esp_openclaw_node_task,
        "esp_openclaw_node",
        ESP_OPENCLAW_NODE_TASK_STACK_SIZE,
        node,
        5,
        &node->task_handle);
    if (task_ok != pdPASS) {
        esp_openclaw_node_persisted_session_free(&node->persisted_session);
        esp_openclaw_node_identity_free(&node->identity);
        esp_openclaw_node_free_config_strings(&node->config);
        vQueueDelete(node->work_queue);
        vSemaphoreDelete(node->destroy_done);
        vSemaphoreDelete(node->state_lock);
        free(node);
        return ESP_ERR_NO_MEM;
    }

    *out_node = node;
    return ESP_OK;
}

esp_err_t esp_openclaw_node_destroy(esp_openclaw_node_handle_t node)
{
    if (node == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (esp_openclaw_node_is_node_task_context(node)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_openclaw_node_lock_state(node);
    if (node->state == ESP_OPENCLAW_NODE_INTERNAL_DESTROYING ||
        node->state == ESP_OPENCLAW_NODE_INTERNAL_CLOSED) {
        esp_openclaw_node_unlock_state(node);
        return ESP_ERR_INVALID_STATE;
    }
    SemaphoreHandle_t destroy_done = node->destroy_done;
    if (destroy_done == NULL) {
        esp_openclaw_node_unlock_state(node);
        return ESP_FAIL;
    }
    node->state = ESP_OPENCLAW_NODE_INTERNAL_DESTROYING;
    esp_openclaw_node_unlock_state(node);

    (void)xSemaphoreTake(destroy_done, 0);

    esp_openclaw_node_work_message_t message = {
        .type = ESP_OPENCLAW_NODE_WORK_MSG_SHUTDOWN,
    };
    if (node->work_queue == NULL ||
        xQueueSend(node->work_queue, &message, portMAX_DELAY) != pdTRUE) {
        esp_openclaw_node_lock_state(node);
        node->state = ESP_OPENCLAW_NODE_INTERNAL_IDLE;
        esp_openclaw_node_unlock_state(node);
        return ESP_FAIL;
    }

    if (xSemaphoreTake(destroy_done, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    if (node->work_queue != NULL) {
        vQueueDelete(node->work_queue);
        node->work_queue = NULL;
    }
    if (node->state_lock != NULL) {
        vSemaphoreDelete(node->state_lock);
        node->state_lock = NULL;
    }
    if (node->destroy_done != NULL) {
        vSemaphoreDelete(node->destroy_done);
        node->destroy_done = NULL;
    }

    esp_openclaw_node_cleanup_registry(node);
    esp_openclaw_node_clear_connect_source_struct(&node->active_connect_source);
    esp_openclaw_node_persisted_session_free(&node->persisted_session);
    esp_openclaw_node_identity_free(&node->identity);
    esp_openclaw_node_free_config_strings(&node->config);
    free(node);
    return ESP_OK;
}

esp_err_t esp_openclaw_node_register_capability(
    esp_openclaw_node_handle_t node,
    const char *capability)
{
    return esp_openclaw_node_register_capability_internal(node, capability);
}

esp_err_t esp_openclaw_node_register_scope(
    esp_openclaw_node_handle_t node,
    const char *scope)
{
    return esp_openclaw_node_register_scope_internal(node, scope);
}

esp_err_t esp_openclaw_node_register_command(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_command_t *command)
{
    return esp_openclaw_node_register_command_internal(node, command);
}

esp_err_t esp_openclaw_node_request_connect(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_connect_request_t *request)
{
    if (node == NULL || request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_openclaw_node_connect_request_source_t connect_source = {0};
    esp_err_t err =
        esp_openclaw_node_build_connect_source_from_request(
            request,
            &connect_source);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_openclaw_node_submit_connect_request(node, &connect_source);
    if (err != ESP_OK) {
        esp_openclaw_node_clear_connect_source_struct(&connect_source);
        return err;
    }
    return ESP_OK;
}

esp_err_t esp_openclaw_node_request_disconnect(esp_openclaw_node_handle_t node)
{
    if (node == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return esp_openclaw_node_submit_disconnect_request(node);
}

esp_err_t esp_openclaw_node_gateway_request(
    esp_openclaw_node_handle_t node,
    const char *method,
    const char *params_json,
    esp_openclaw_node_gateway_request_cb_t callback,
    void *user_ctx)
{
    if (node == NULL || method == NULL || method[0] == '\0' || callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *params = cJSON_Parse(params_json != NULL ? params_json : "{}");
    bool params_valid = cJSON_IsObject(params);
    cJSON_Delete(params);
    if (!params_valid) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_openclaw_node_submit_gateway_request(
        node,
        method,
        params_json,
        callback,
        user_ctx);
}

const char *esp_openclaw_node_get_device_id(esp_openclaw_node_handle_t node)
{
    return node != NULL ? node->identity.device_id : NULL;
}

bool esp_openclaw_node_has_saved_session(esp_openclaw_node_handle_t node)
{
    if (node == NULL) {
        return false;
    }
    esp_openclaw_node_lock_state(node);
    bool present = esp_openclaw_node_saved_session_is_present_locked(node);
    esp_openclaw_node_unlock_state(node);
    return present;
}
