/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_openclaw_node_identity.h"
#include "esp_openclaw_node_internal.h"
#include "esp_openclaw_node.h"
#include "esp_openclaw_node_persisted_session.h"
#include "unity.h"

typedef struct {
    volatile bool seen;
    volatile esp_openclaw_node_event_t event;
    volatile esp_err_t local_err;
    volatile int connected_count;
    volatile int connect_failed_count;
    volatile int disconnected_count;
    volatile esp_openclaw_node_connect_failure_reason_t connect_failed_reason;
    volatile esp_openclaw_node_disconnected_reason_t disconnected_reason;
} test_event_recorder_t;

typedef struct {
    esp_event_handler_t event_handler;
    void *event_handler_arg;
    esp_websocket_client_handle_t client;
    volatile int start_calls;
    volatile int stop_calls;
    volatile int destroy_calls;
    volatile int send_text_calls;
    volatile int send_with_opcode_calls;
    volatile ws_transport_opcodes_t last_opcode;
    char *last_sent_text;
    SemaphoreHandle_t start_entered;
    SemaphoreHandle_t start_release;
    esp_err_t start_result;
    SemaphoreHandle_t send_text_entered;
    SemaphoreHandle_t send_text_release;
} test_transport_state_t;

typedef struct {
    SemaphoreHandle_t entered;
    SemaphoreHandle_t release;
} blocking_command_ctx_t;

typedef struct {
    bool called;
    char session_key[ESP_OPENCLAW_NODE_MAX_SESSION_KEY_LEN + 1];
} invocation_command_ctx_t;

typedef struct {
    esp_openclaw_node_handle_t node;
    TaskHandle_t completion_waiter;
    volatile esp_err_t destroy_err;
    volatile uint32_t remaining_notify_count;
} destroy_task_ctx_t;

typedef struct {
    volatile bool event_seen;
    volatile bool request_seen;
    volatile bool request_ok;
    char event[64];
    char event_payload[128];
    char request_payload[128];
    char request_error[64];
} gateway_recorder_t;

static test_event_recorder_t s_event_recorder;
static test_transport_state_t s_transport_state;
static int s_fake_transport_client;
static gateway_recorder_t s_gateway_recorder;

static esp_websocket_client_handle_t test_transport_client_init(const esp_websocket_client_config_t *config)
{
    (void)config;
    s_transport_state.client = (esp_websocket_client_handle_t)&s_fake_transport_client;
    return s_transport_state.client;
}

static esp_err_t test_transport_register_events(
    esp_websocket_client_handle_t client,
    esp_websocket_event_id_t event,
    esp_event_handler_t event_handler,
    void *event_handler_arg)
{
    (void)client;
    (void)event;
    s_transport_state.event_handler = event_handler;
    s_transport_state.event_handler_arg = event_handler_arg;
    return ESP_OK;
}

static esp_err_t test_transport_client_start(esp_websocket_client_handle_t client)
{
    (void)client;
    s_transport_state.start_calls++;
    if (s_transport_state.start_entered != NULL) {
        TEST_ASSERT_TRUE(xSemaphoreGive(s_transport_state.start_entered) == pdTRUE);
    }
    if (s_transport_state.start_release != NULL) {
        TEST_ASSERT_TRUE(xSemaphoreTake(s_transport_state.start_release, portMAX_DELAY) == pdTRUE);
    }
    return s_transport_state.start_result;
}

static esp_err_t test_transport_client_stop(esp_websocket_client_handle_t client)
{
    (void)client;
    s_transport_state.stop_calls++;
    return ESP_OK;
}

static esp_err_t test_transport_client_destroy(esp_websocket_client_handle_t client)
{
    (void)client;
    s_transport_state.destroy_calls++;
    return ESP_OK;
}

static int test_transport_send_text(
    esp_websocket_client_handle_t client,
    const char *data,
    int len,
    TickType_t timeout)
{
    (void)client;
    (void)timeout;
    free(s_transport_state.last_sent_text);
    s_transport_state.last_sent_text = calloc((size_t)len + 1U, sizeof(char));
    TEST_ASSERT_NOT_NULL(s_transport_state.last_sent_text);
    memcpy(s_transport_state.last_sent_text, data, (size_t)len);
    s_transport_state.send_text_calls++;
    if (s_transport_state.send_text_entered != NULL) {
        TEST_ASSERT_TRUE(xSemaphoreGive(s_transport_state.send_text_entered) == pdTRUE);
    }
    if (s_transport_state.send_text_release != NULL) {
        TEST_ASSERT_TRUE(xSemaphoreTake(s_transport_state.send_text_release, portMAX_DELAY) == pdTRUE);
    }
    return len;
}

static int test_transport_send_with_opcode(
    esp_websocket_client_handle_t client,
    ws_transport_opcodes_t opcode,
    const uint8_t *data,
    int len,
    TickType_t timeout)
{
    (void)client;
    (void)data;
    (void)len;
    (void)timeout;
    s_transport_state.last_opcode = opcode;
    s_transport_state.send_with_opcode_calls++;
    return 0;
}

static const esp_openclaw_node_websocket_client_ops_t s_test_websocket_client_ops = {
    .client_init = test_transport_client_init,
    .register_events = test_transport_register_events,
    .client_start = test_transport_client_start,
    .client_stop = test_transport_client_stop,
    .client_destroy = test_transport_client_destroy,
    .send_text = test_transport_send_text,
    .send_with_opcode = test_transport_send_with_opcode,
};

const esp_openclaw_node_websocket_client_ops_t *esp_openclaw_node_test_websocket_client_ops(void)
{
    return &s_test_websocket_client_ops;
}

static void reset_openclaw_storage(void)
{
    nvs_flash_deinit();
    TEST_ASSERT_EQUAL(ESP_OK, nvs_flash_erase());
    TEST_ASSERT_EQUAL(ESP_OK, nvs_flash_init());
}

static char *encode_base64url_string(const char *input)
{
    size_t input_len = strlen(input);
    size_t encoded_capacity = (((input_len + 2U) / 3U) * 4U) + 1U;
    unsigned char *encoded = calloc(encoded_capacity, sizeof(unsigned char));
    TEST_ASSERT_NOT_NULL(encoded);

    size_t written = 0;
    TEST_ASSERT_EQUAL_INT(
        0,
        mbedtls_base64_encode(encoded, encoded_capacity, &written, (const unsigned char *)input, input_len));

    for (size_t i = 0; i < written; ++i) {
        if (encoded[i] == '+') {
            encoded[i] = '-';
        } else if (encoded[i] == '/') {
            encoded[i] = '_';
        }
    }
    while (written > 0 && encoded[written - 1] == '=') {
        --written;
    }
    encoded[written] = '\0';
    return (char *)encoded;
}

static esp_err_t request_connect_saved_session(esp_openclaw_node_handle_t node)
{
    const esp_openclaw_node_connect_request_t request = {
        .source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_SAVED_SESSION,
        .gateway_uri = NULL,
        .value = NULL,
    };
    return esp_openclaw_node_request_connect(node, &request);
}

static esp_err_t request_connect_setup_code(esp_openclaw_node_handle_t node, const char *setup_code)
{
    const esp_openclaw_node_connect_request_t request = {
        .source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_SETUP_CODE,
        .gateway_uri = NULL,
        .value = setup_code,
    };
    return esp_openclaw_node_request_connect(node, &request);
}

static esp_err_t request_connect_gateway_token(
    esp_openclaw_node_handle_t node,
    const char *gateway_uri,
    const char *token)
{
    const esp_openclaw_node_connect_request_t request = {
        .source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_GATEWAY_TOKEN,
        .gateway_uri = gateway_uri,
        .value = token,
    };
    return esp_openclaw_node_request_connect(node, &request);
}

static esp_err_t request_connect_gateway_password(
    esp_openclaw_node_handle_t node,
    const char *gateway_uri,
    const char *password)
{
    const esp_openclaw_node_connect_request_t request = {
        .source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_GATEWAY_PASSWORD,
        .gateway_uri = gateway_uri,
        .value = password,
    };
    return esp_openclaw_node_request_connect(node, &request);
}

static esp_err_t request_connect_no_auth(esp_openclaw_node_handle_t node, const char *gateway_uri)
{
    const esp_openclaw_node_connect_request_t request = {
        .source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_NO_AUTH,
        .gateway_uri = gateway_uri,
        .value = NULL,
    };
    return esp_openclaw_node_request_connect(node, &request);
}

static void assert_persisted_session_empty(const esp_openclaw_node_persisted_session_t *session)
{
    TEST_ASSERT_NOT_NULL(session);
    TEST_ASSERT_EQUAL_UINT8(0, session->version);
    TEST_ASSERT_NULL(session->gateway_uri);
    TEST_ASSERT_NULL(session->device_token);
    TEST_ASSERT_FALSE(esp_openclaw_node_persisted_session_is_present(session));
}

static bool contains_only_base64url_chars(const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return false;
    }
    for (const char *p = value; *p != '\0'; ++p) {
        bool is_alpha_num = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                            (*p >= '0' && *p <= '9');
        if (!is_alpha_num && *p != '-' && *p != '_') {
            return false;
        }
    }
    return true;
}

static void reset_event_recorder(void)
{
    memset((void *)&s_event_recorder, 0, sizeof(s_event_recorder));
}

static void reset_transport_state(void)
{
    free(s_transport_state.last_sent_text);
    memset(&s_transport_state, 0, sizeof(s_transport_state));
}

static void test_node_event_cb(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_event_t event,
    const void *event_data,
    void *user_ctx)
{
    (void)node;
    (void)user_ctx;

    s_event_recorder.event = event;
    s_event_recorder.local_err = ESP_OK;
    if (event == ESP_OPENCLAW_NODE_EVENT_CONNECTED) {
        s_event_recorder.connected_count++;
    } else if (event == ESP_OPENCLAW_NODE_EVENT_CONNECT_FAILED) {
        s_event_recorder.connect_failed_count++;
        const esp_openclaw_node_connect_failed_event_t *failed = event_data;
        if (failed != NULL) {
            s_event_recorder.local_err = failed->local_err;
            s_event_recorder.connect_failed_reason = failed->reason;
        }
    } else if (event == ESP_OPENCLAW_NODE_EVENT_DISCONNECTED) {
        s_event_recorder.disconnected_count++;
        const esp_openclaw_node_disconnected_event_t *disconnected = event_data;
        if (disconnected != NULL) {
            s_event_recorder.local_err = disconnected->local_err;
            s_event_recorder.disconnected_reason = disconnected->reason;
        }
    }
    s_event_recorder.seen = true;
}

static bool wait_for_event(esp_openclaw_node_event_t expected, TickType_t timeout_ticks)
{
    TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < timeout_ticks) {
        if (s_event_recorder.seen && s_event_recorder.event == expected) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

static bool wait_for_int_value(volatile int *value, int expected_minimum, TickType_t timeout_ticks)
{
    TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < timeout_ticks) {
        if (*value >= expected_minimum) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

static bool wait_for_bool(volatile bool *value, TickType_t timeout_ticks)
{
    TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < timeout_ticks) {
        if (*value) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

static void test_gateway_event_cb(
    esp_openclaw_node_handle_t node,
    const char *event,
    const char *payload_json,
    void *user_ctx)
{
    (void)node;
    gateway_recorder_t *recorder = user_ctx;
    snprintf(recorder->event, sizeof(recorder->event), "%s", event);
    snprintf(recorder->event_payload, sizeof(recorder->event_payload), "%s", payload_json);
    recorder->event_seen = true;
}

static void test_gateway_request_cb(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_gateway_result_t *result,
    void *user_ctx)
{
    (void)node;
    gateway_recorder_t *recorder = user_ctx;
    recorder->request_ok = result->ok;
    snprintf(
        recorder->request_payload,
        sizeof(recorder->request_payload),
        "%s",
        result->payload_json != NULL ? result->payload_json : "");
    snprintf(
        recorder->request_error,
        sizeof(recorder->request_error),
        "%s",
        result->error_code != NULL ? result->error_code : "");
    recorder->request_seen = true;
}

static void emit_ws_event(int32_t event_id, const char *text, esp_err_t local_err)
{
    TEST_ASSERT_NOT_NULL(s_transport_state.event_handler);

    esp_websocket_event_data_t event_data = {0};
    if (text != NULL) {
        event_data.op_code = 0x01;
        event_data.fin = true;
        event_data.data_ptr = (char *)text;
        event_data.data_len = (int)strlen(text);
        event_data.payload_len = (int)strlen(text);
        event_data.payload_offset = 0;
    }
    event_data.error_handle.esp_tls_last_esp_err = local_err;
    s_transport_state.event_handler(
        s_transport_state.event_handler_arg,
        WEBSOCKET_EVENTS,
        event_id,
        &event_data);
}

static void emit_ws_frame_chunk(
    uint8_t opcode,
    char *data,
    int data_len,
    int payload_len,
    int payload_offset,
    bool fin)
{
    TEST_ASSERT_NOT_NULL(s_transport_state.event_handler);
    esp_websocket_event_data_t event_data = {
        .op_code = opcode,
        .fin = fin,
        .data_ptr = data,
        .data_len = data_len,
        .payload_len = payload_len,
        .payload_offset = payload_offset,
    };
    s_transport_state.event_handler(
        s_transport_state.event_handler_arg,
        WEBSOCKET_EVENTS,
        WEBSOCKET_EVENT_DATA,
        &event_data);
}

static esp_err_t invocation_command_handler(
    esp_openclaw_node_handle_t node,
    void *context,
    const esp_openclaw_node_command_invocation_t *invocation,
    const char *params_json,
    size_t params_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node;
    (void)params_json;
    (void)params_len;
    (void)out_payload_json;
    (void)out_error;
    invocation_command_ctx_t *ctx = context;
    ctx->called = true;
    snprintf(
        ctx->session_key,
        sizeof(ctx->session_key),
        "%s",
        invocation->session_key != NULL ? invocation->session_key : "");
    return ESP_OK;
}

static char *extract_first_json_id(const char *json)
{
    const char *marker = "\"id\":\"";
    const char *start = strstr(json, marker);
    TEST_ASSERT_NOT_NULL(start);
    start += strlen(marker);
    const char *end = strchr(start, '"');
    TEST_ASSERT_NOT_NULL(end);
    size_t len = (size_t)(end - start);
    char *id = calloc(len + 1U, sizeof(char));
    TEST_ASSERT_NOT_NULL(id);
    memcpy(id, start, len);
    return id;
}

static esp_err_t blocking_command_handler(
    esp_openclaw_node_handle_t node,
    void *context,
    const char *params_json,
    size_t params_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node;
    (void)params_json;
    (void)params_len;
    (void)out_error;

    blocking_command_ctx_t *ctx = (blocking_command_ctx_t *)context;
    TEST_ASSERT_NOT_NULL(ctx);
    TEST_ASSERT_TRUE(xSemaphoreGive(ctx->entered) == pdTRUE);
    TEST_ASSERT_TRUE(xSemaphoreTake(ctx->release, portMAX_DELAY) == pdTRUE);

    *out_payload_json = calloc(3, sizeof(char));
    TEST_ASSERT_NOT_NULL(*out_payload_json);
    memcpy(*out_payload_json, "{}", 3);
    return ESP_OK;
}

static void destroy_with_pending_notification_task(void *arg)
{
    destroy_task_ctx_t *ctx = (destroy_task_ctx_t *)arg;
    TEST_ASSERT_NOT_NULL(ctx);

    xTaskNotifyGive(xTaskGetCurrentTaskHandle());
    ctx->destroy_err = esp_openclaw_node_destroy(ctx->node);
    ctx->remaining_notify_count = ulTaskNotifyTake(pdTRUE, 0);
    xTaskNotifyGive(ctx->completion_waiter);
    vTaskDelete(NULL);
}

TEST_CASE("persisted session stores loads and can be cleared by storing empty state", "[esp_openclaw_node][session]")
{
    reset_openclaw_storage();

    char gateway_uri[] = "wss://gateway.example/ws";
    char device_token[] = "device-token-123";
    esp_openclaw_node_persisted_session_t session = {0};
    esp_openclaw_node_persisted_session_t update = {
        .version = 1,
        .gateway_uri = gateway_uri,
        .device_token = device_token,
    };

    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_persisted_session_store("node", &session, &update));
    TEST_ASSERT_TRUE(esp_openclaw_node_persisted_session_is_present(&session));
    TEST_ASSERT_EQUAL_STRING("wss://gateway.example/ws", session.gateway_uri);
    TEST_ASSERT_EQUAL_STRING("device-token-123", session.device_token);

    esp_openclaw_node_persisted_session_t loaded = {0};
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_persisted_session_load("node", &loaded));
    TEST_ASSERT_TRUE(esp_openclaw_node_persisted_session_is_present(&loaded));
    TEST_ASSERT_EQUAL_STRING("wss://gateway.example/ws", loaded.gateway_uri);
    TEST_ASSERT_EQUAL_STRING("device-token-123", loaded.device_token);

    esp_openclaw_node_persisted_session_t cleared = {0};
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_persisted_session_store("node", &session, &cleared));
    assert_persisted_session_empty(&session);

    esp_openclaw_node_persisted_session_free(&loaded);
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_persisted_session_load("node", &loaded));
    assert_persisted_session_empty(&loaded);

    esp_openclaw_node_persisted_session_free(&loaded);
    esp_openclaw_node_persisted_session_free(&session);
}

TEST_CASE("persisted session load ignores unsupported stored versions", "[esp_openclaw_node][session]")
{
    reset_openclaw_storage();

    nvs_handle_t nvs = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open("openclaw", NVS_READWRITE, &nvs));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u8(nvs, "session_v", 99));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_str(nvs, "session_uri", "wss://gateway.example/ws"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_str(nvs, "session_dev_tok", "device-token-123"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(nvs));
    nvs_close(nvs);

    esp_openclaw_node_persisted_session_t loaded = {0};
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_persisted_session_load("node", &loaded));
    assert_persisted_session_empty(&loaded);

    uint8_t version = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open("openclaw", NVS_READONLY, &nvs));
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, nvs_get_u8(nvs, "session_v", &version));
    nvs_close(nvs);

    esp_openclaw_node_config_t config = {0};
    esp_openclaw_node_config_init_default(&config);

    esp_openclaw_node_handle_t node = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_create(&config, &node));
    TEST_ASSERT_NOT_NULL(node);
    TEST_ASSERT_FALSE(esp_openclaw_node_has_saved_session(node));
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_destroy(node));
}

TEST_CASE("persisted session load clears malformed stored state and node still boots", "[esp_openclaw_node][session]")
{
    reset_openclaw_storage();

    nvs_handle_t nvs = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open("openclaw", NVS_READWRITE, &nvs));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u8(nvs, "session_v", 1));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_str(nvs, "session_uri", "wss://gateway.example/ws"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(nvs));
    nvs_close(nvs);

    esp_openclaw_node_persisted_session_t loaded = {0};
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_persisted_session_load("node", &loaded));
    assert_persisted_session_empty(&loaded);

    uint8_t version = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open("openclaw", NVS_READONLY, &nvs));
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, nvs_get_u8(nvs, "session_v", &version));
    nvs_close(nvs);

    esp_openclaw_node_config_t config = {0};
    esp_openclaw_node_config_init_default(&config);

    esp_openclaw_node_handle_t node = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_create(&config, &node));
    TEST_ASSERT_NOT_NULL(node);
    TEST_ASSERT_FALSE(esp_openclaw_node_has_saved_session(node));
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_destroy(node));
}

TEST_CASE("node create deep-copies tls cert pem", "[esp_openclaw_node][config]")
{
    reset_openclaw_storage();

    char tls_cert_pem[] =
        "-----BEGIN CERTIFICATE-----\n"
        "example-cert\n"
        "-----END CERTIFICATE-----\n";

    esp_openclaw_node_config_t config = {0};
    esp_openclaw_node_config_init_default(&config);
    config.tls_cert_pem = tls_cert_pem;
    config.tls_cert_len = 0;
    config.use_cert_bundle = false;

    esp_openclaw_node_handle_t node = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_create(&config, &node));
    TEST_ASSERT_NOT_NULL(node);
    TEST_ASSERT_NOT_NULL(node->config.tls_cert_pem);
    TEST_ASSERT_TRUE(node->config.tls_cert_pem != config.tls_cert_pem);
    TEST_ASSERT_EQUAL_STRING(tls_cert_pem, node->config.tls_cert_pem);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)node->config.tls_cert_len);

    tls_cert_pem[0] = 'X';
    TEST_ASSERT_EQUAL_STRING(
        "-----BEGIN CERTIFICATE-----\n"
        "example-cert\n"
        "-----END CERTIFICATE-----\n",
        node->config.tls_cert_pem);

    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_destroy(node));
}

TEST_CASE("persisted session rejects partial state", "[esp_openclaw_node][session]")
{
    reset_openclaw_storage();

    char gateway_uri[] = "wss://gateway.example/ws";
    char device_token[] = "device-token-123";
    esp_openclaw_node_persisted_session_t session = {0};
    esp_openclaw_node_persisted_session_t invalid = {
        .version = 1,
        .gateway_uri = gateway_uri,
        .device_token = NULL,
    };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esp_openclaw_node_persisted_session_store("node", &session, &invalid));
    assert_persisted_session_empty(&session);

    invalid.gateway_uri = NULL;
    invalid.device_token = device_token;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esp_openclaw_node_persisted_session_store("node", &session, &invalid));
    assert_persisted_session_empty(&session);

    esp_openclaw_node_persisted_session_t loaded = {0};
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_persisted_session_load("node", &loaded));
    assert_persisted_session_empty(&loaded);
    esp_openclaw_node_persisted_session_free(&loaded);
}

TEST_CASE("persisted session store validates inputs before mutating state", "[esp_openclaw_node][session]")
{
    reset_openclaw_storage();

    char initial_gateway_uri[] = "wss://gateway.example/ws";
    char initial_device_token[] = "device-token-123";
    esp_openclaw_node_persisted_session_t session = {0};
    esp_openclaw_node_persisted_session_t initial = {
        .version = 1,
        .gateway_uri = initial_gateway_uri,
        .device_token = initial_device_token,
    };
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_persisted_session_store("node", &session, &initial));

    char replacement_gateway_uri[] = "wss://replacement.example/ws";
    char replacement_device_token[] = "device-token-456";
    esp_openclaw_node_persisted_session_t replacement = {
        .version = 1,
        .gateway_uri = replacement_gateway_uri,
        .device_token = replacement_device_token,
    };

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esp_openclaw_node_persisted_session_store("node", NULL, &replacement));
    TEST_ASSERT_EQUAL_STRING("wss://gateway.example/ws", session.gateway_uri);
    TEST_ASSERT_EQUAL_STRING("device-token-123", session.device_token);

    esp_openclaw_node_persisted_session_t loaded = {0};
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_persisted_session_load("node", &loaded));
    TEST_ASSERT_EQUAL_STRING("wss://gateway.example/ws", loaded.gateway_uri);
    TEST_ASSERT_EQUAL_STRING("device-token-123", loaded.device_token);
    esp_openclaw_node_persisted_session_free(&loaded);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esp_openclaw_node_persisted_session_store("node", &session, NULL));
    TEST_ASSERT_EQUAL_STRING("wss://gateway.example/ws", session.gateway_uri);
    TEST_ASSERT_EQUAL_STRING("device-token-123", session.device_token);

    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_persisted_session_load("node", &loaded));
    TEST_ASSERT_EQUAL_STRING("wss://gateway.example/ws", loaded.gateway_uri);
    TEST_ASSERT_EQUAL_STRING("device-token-123", loaded.device_token);

    esp_openclaw_node_persisted_session_free(&loaded);
    esp_openclaw_node_persisted_session_free(&session);
}

TEST_CASE("identity persists seed and auth payload signing", "[esp_openclaw_node][identity]")
{
    reset_openclaw_storage();

    uint8_t seed[ESP_OPENCLAW_NODE_ED25519_SEED_LEN] = {0};
    for (size_t i = 0; i < sizeof(seed); ++i) {
        seed[i] = (uint8_t)(i + 1U);
    }

    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_identity_store_seed_if_absent(seed, sizeof(seed)));

    esp_openclaw_node_identity_t identity = {0};
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_identity_load_or_create(&identity));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(seed, identity.seed, sizeof(seed));
    TEST_ASSERT_NOT_EQUAL('\0', identity.device_id[0]);
    TEST_ASSERT_NOT_EQUAL('\0', identity.public_key_b64url[0]);

    char *payload = NULL;
    TEST_ASSERT_EQUAL(
        ESP_OK,
        esp_openclaw_node_identity_build_auth_payload_v3(
            &identity,
            "node-host",
            "node",
            "node",
            "device,wifi",
            123456,
            "device-token-123",
            "nonce-xyz",
            "  ESP32-S3  ",
            "  Sensor Hub  ",
            &payload));
    TEST_ASSERT_NOT_NULL(payload);
    TEST_ASSERT_NOT_NULL(strstr(payload, "|esp32-s3|sensor hub"));

    char signature[ESP_OPENCLAW_NODE_SIGNATURE_B64_BUFFER_LEN] = {0};
    TEST_ASSERT_EQUAL(
        ESP_OK,
        esp_openclaw_node_identity_sign_payload(&identity, payload, signature, sizeof(signature)));
    TEST_ASSERT_TRUE(contains_only_base64url_chars(signature));

    esp_openclaw_node_identity_t reloaded = {0};
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_identity_load_or_create(&reloaded));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(seed, reloaded.seed, sizeof(seed));
    TEST_ASSERT_EQUAL_STRING(identity.device_id, reloaded.device_id);

    free(payload);
    esp_openclaw_node_identity_free(&reloaded);
    esp_openclaw_node_identity_free(&identity);
}

TEST_CASE("identity load recovers from malformed seed and clears saved session", "[esp_openclaw_node][identity][session]")
{
    reset_openclaw_storage();

    uint8_t malformed_seed[] = {1, 2, 3, 4, 5, 6, 7};

    nvs_handle_t nvs = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open("openclaw", NVS_READWRITE, &nvs));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_blob(nvs, "device_seed", malformed_seed, sizeof(malformed_seed)));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u8(nvs, "session_v", 1));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_str(nvs, "session_uri", "wss://gateway.example/ws"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_str(nvs, "session_dev_tok", "device-token-123"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(nvs));
    nvs_close(nvs);

    esp_openclaw_node_identity_t identity = {0};
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_identity_load_or_create(&identity));
    TEST_ASSERT_NOT_EQUAL('\0', identity.device_id[0]);
    TEST_ASSERT_NOT_EQUAL('\0', identity.public_key_b64url[0]);

    esp_openclaw_node_persisted_session_t loaded_session = {0};
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_persisted_session_load("node", &loaded_session));
    assert_persisted_session_empty(&loaded_session);
    esp_openclaw_node_persisted_session_free(&loaded_session);

    uint8_t stored_seed[ESP_OPENCLAW_NODE_ED25519_SEED_LEN] = {0};
    size_t stored_seed_len = sizeof(stored_seed);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_open("openclaw", NVS_READONLY, &nvs));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_blob(nvs, "device_seed", stored_seed, &stored_seed_len));
    TEST_ASSERT_EQUAL_UINT32(ESP_OPENCLAW_NODE_ED25519_SEED_LEN, (uint32_t)stored_seed_len);
    nvs_close(nvs);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(identity.seed, stored_seed, sizeof(stored_seed));

    esp_openclaw_node_identity_t reloaded = {0};
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_identity_load_or_create(&reloaded));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(identity.seed, reloaded.seed, sizeof(identity.seed));
    TEST_ASSERT_EQUAL_STRING(identity.device_id, reloaded.device_id);

    esp_openclaw_node_identity_free(&reloaded);
    esp_openclaw_node_identity_free(&identity);
}

TEST_CASE("connect saved session without persisted session fails", "[esp_openclaw_node][api]")
{
    reset_openclaw_storage();
    reset_transport_state();

    esp_openclaw_node_config_t config = {0};
    esp_openclaw_node_config_init_default(&config);

    esp_openclaw_node_handle_t node = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_create(&config, &node));
    TEST_ASSERT_NOT_NULL(node);
    TEST_ASSERT_FALSE(esp_openclaw_node_has_saved_session(node));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, request_connect_saved_session(node));

    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_destroy(node));
}

TEST_CASE("setup code rejects invalid or ambiguous auth shapes", "[esp_openclaw_node][api]")
{
    reset_openclaw_storage();
    reset_transport_state();

    esp_openclaw_node_config_t config = {0};
    esp_openclaw_node_config_init_default(&config);

    esp_openclaw_node_handle_t node = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_create(&config, &node));

    const char *invalid_json[] = {
        "{\"url\":\"ws://gateway.example\"}",
        "{\"url\":\"ws://gateway.example\",\"bootstrapToken\":\"boot\",\"token\":\"shared\"}",
        "{\"url\":\"ws://gateway.example\",\"bootstrapToken\":\"boot\",\"password\":\"secret\"}",
        "{\"url\":\"ws://gateway.example\",\"authMode\":\"none\"}",
    };

    for (size_t i = 0; i < sizeof(invalid_json) / sizeof(invalid_json[0]); ++i) {
        char *setup_code = encode_base64url_string(invalid_json[i]);
        TEST_ASSERT_NOT_NULL(setup_code);
        TEST_ASSERT_EQUAL(
            ESP_ERR_INVALID_ARG,
            request_connect_setup_code(node, setup_code));
        free(setup_code);
    }

    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_destroy(node));
}

TEST_CASE("explicit connect request validates psk and no-auth arguments", "[esp_openclaw_node][api]")
{
    reset_openclaw_storage();
    reset_transport_state();

    esp_openclaw_node_config_t config = {0};
    esp_openclaw_node_config_init_default(&config);

    esp_openclaw_node_handle_t node = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_create(&config, &node));

    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG,
        request_connect_gateway_token(
            node,
            "not-a-uri",
            "secret-password"));
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG,
        request_connect_gateway_token(
            node,
            "wss://gateway.example/ws",
            ""));
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG,
        request_connect_gateway_password(
            node,
            "not-a-uri",
            "secret-password"));
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG,
        request_connect_gateway_password(
            node,
            "wss://gateway.example/ws",
            ""));
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG,
        request_connect_gateway_token(
            node,
            "wss://gateway.example/ws",
            "   "));
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG,
        request_connect_gateway_token(
            node,
            "",
            "secret-password"));
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG,
        esp_openclaw_node_request_connect(
            node,
            &(esp_openclaw_node_connect_request_t){
                .source = (esp_openclaw_node_connect_source_t)99,
                .gateway_uri = "wss://gateway.example/ws",
                .value = "secret-password",
            }));
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG,
        request_connect_no_auth(
            node,
            "not-a-uri"));
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG,
        request_connect_no_auth(
            node,
            ""));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, esp_openclaw_node_request_disconnect(node));

    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_destroy(node));
}

TEST_CASE("password auth is excluded from the signed device payload", "[esp_openclaw_node][auth]")
{
    reset_openclaw_storage();
    reset_transport_state();

    esp_openclaw_node_config_t config = {0};
    esp_openclaw_node_config_init_default(&config);

    esp_openclaw_node_handle_t node = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_create(&config, &node));

    const esp_openclaw_node_connect_request_t password_request = {
        .source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_GATEWAY_PASSWORD,
        .gateway_uri = "ws://gateway.example/ws",
        .value = "secret-password",
    };
    const esp_openclaw_node_connect_request_t token_request = {
        .source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_GATEWAY_TOKEN,
        .gateway_uri = "ws://gateway.example/ws",
        .value = "shared-token",
    };

    esp_openclaw_node_connect_request_source_t password_source = {0};
    esp_openclaw_node_connect_request_source_t token_source = {0};
    TEST_ASSERT_EQUAL(
        ESP_OK,
        esp_openclaw_node_build_connect_source_from_request(
            &password_request,
            &password_source));
    TEST_ASSERT_EQUAL(
        ESP_OK,
        esp_openclaw_node_build_connect_source_from_request(
            &token_request,
            &token_source));

    esp_openclaw_node_connect_material_t password_material = {0};
    esp_openclaw_node_connect_material_t token_material = {0};

    esp_openclaw_node_lock_state(node);
    node->active_connect_source = password_source;
    memset(&password_source, 0, sizeof(password_source));
    TEST_ASSERT_EQUAL(
        ESP_OK,
        esp_openclaw_node_resolve_active_connect_material_locked(
            node,
            &password_material));

    esp_openclaw_node_clear_connect_source_struct(&node->active_connect_source);
    node->active_connect_source = token_source;
    memset(&token_source, 0, sizeof(token_source));
    TEST_ASSERT_EQUAL(
        ESP_OK,
        esp_openclaw_node_resolve_active_connect_material_locked(
            node,
            &token_material));
    esp_openclaw_node_unlock_state(node);

    TEST_ASSERT_EQUAL(
        ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_PASSWORD,
        password_material.kind);
    TEST_ASSERT_NOT_NULL(password_material.auth_value);
    TEST_ASSERT_NULL(password_material.signature_token);

    TEST_ASSERT_EQUAL(
        ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SHARED_TOKEN,
        token_material.kind);
    TEST_ASSERT_NOT_NULL(token_material.auth_value);
    TEST_ASSERT_EQUAL_PTR(token_material.auth_value, token_material.signature_token);

    esp_openclaw_node_free_connect_material(&password_material);
    esp_openclaw_node_free_connect_material(&token_material);
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_destroy(node));
}

TEST_CASE("destroy preserves the caller task notification count", "[esp_openclaw_node][destroy]")
{
    reset_openclaw_storage();
    reset_transport_state();

    esp_openclaw_node_config_t config = {0};
    esp_openclaw_node_config_init_default(&config);

    esp_openclaw_node_handle_t node = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_create(&config, &node));

    destroy_task_ctx_t ctx = {
        .node = node,
        .completion_waiter = xTaskGetCurrentTaskHandle(),
        .destroy_err = ESP_FAIL,
        .remaining_notify_count = 0,
    };

    TaskHandle_t destroy_task = NULL;
    TEST_ASSERT_EQUAL(
        pdPASS,
        xTaskCreate(
            destroy_with_pending_notification_task,
            "node_destroy_test",
            4096,
            &ctx,
            5,
            &destroy_task));
    TEST_ASSERT_TRUE(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) == 1);
    TEST_ASSERT_EQUAL(ESP_OK, ctx.destroy_err);
    TEST_ASSERT_EQUAL_UINT32(1, ctx.remaining_notify_count);
}

TEST_CASE("connect kicks challenge ping and disconnect is not dropped while busy", "[esp_openclaw_node][transport]")
{
    reset_openclaw_storage();
    reset_transport_state();

    esp_openclaw_node_config_t config = {0};
    esp_openclaw_node_config_init_default(&config);
    config.event_cb = test_node_event_cb;

    esp_openclaw_node_handle_t node = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_create(&config, &node));

    blocking_command_ctx_t command_ctx = {
        .entered = xSemaphoreCreateBinary(),
        .release = xSemaphoreCreateBinary(),
    };
    TEST_ASSERT_NOT_NULL(command_ctx.entered);
    TEST_ASSERT_NOT_NULL(command_ctx.release);

    esp_openclaw_node_command_t command = {
        .name = "block",
        .handler = blocking_command_handler,
        .context = &command_ctx,
    };
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_register_command(node, &command));

    reset_event_recorder();
    TEST_ASSERT_EQUAL(
        ESP_OK,
        request_connect_no_auth(node, "ws://gateway.example/ws"));
    TEST_ASSERT_TRUE(wait_for_int_value(&s_transport_state.start_calls, 1, pdMS_TO_TICKS(1000)));

    emit_ws_event(WEBSOCKET_EVENT_CONNECTED, NULL, ESP_OK);
    TEST_ASSERT_TRUE(
        wait_for_int_value(&s_transport_state.send_with_opcode_calls, 1, pdMS_TO_TICKS(1000)));
    TEST_ASSERT_EQUAL(WS_TRANSPORT_OPCODES_PING, s_transport_state.last_opcode);

    emit_ws_event(
        WEBSOCKET_EVENT_DATA,
        "{\"type\":\"event\",\"event\":\"connect.challenge\",\"payload\":{\"nonce\":\"nonce-1\",\"ts\":123}}",
        ESP_OK);
    TEST_ASSERT_TRUE(wait_for_int_value(&s_transport_state.send_text_calls, 1, pdMS_TO_TICKS(1000)));

    char *connect_id = extract_first_json_id(s_transport_state.last_sent_text);
    TEST_ASSERT_NOT_NULL(connect_id);

    char connect_response[512] = {0};
    snprintf(
        connect_response,
        sizeof(connect_response),
        "{\"type\":\"res\",\"id\":\"%s\",\"ok\":true,\"payload\":{\"type\":\"hello-ok\",\"auth\":{\"deviceToken\":\"device-token-123\"}}}",
        connect_id);
    free(connect_id);

    emit_ws_event(WEBSOCKET_EVENT_DATA, connect_response, ESP_OK);
    TEST_ASSERT_TRUE(wait_for_event(ESP_OPENCLAW_NODE_EVENT_CONNECTED, pdMS_TO_TICKS(1000)));

    reset_event_recorder();
    emit_ws_event(
        WEBSOCKET_EVENT_DATA,
        "{\"type\":\"event\",\"event\":\"node.invoke.request\",\"payload\":{\"id\":\"invoke-1\",\"nodeId\":\"node-1\",\"command\":\"block\",\"paramsJSON\":\"{}\"}}",
        ESP_OK);
    TEST_ASSERT_TRUE(xSemaphoreTake(command_ctx.entered, pdMS_TO_TICKS(1000)) == pdTRUE);

    for (int i = 0; i < 16; ++i) {
        emit_ws_event(WEBSOCKET_EVENT_ERROR, NULL, ESP_FAIL);
    }
    emit_ws_event(WEBSOCKET_EVENT_DISCONNECTED, NULL, ESP_ERR_INVALID_STATE);

    TEST_ASSERT_TRUE(xSemaphoreGive(command_ctx.release) == pdTRUE);
    TEST_ASSERT_TRUE(wait_for_event(ESP_OPENCLAW_NODE_EVENT_DISCONNECTED, pdMS_TO_TICKS(2000)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, s_event_recorder.local_err);

    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_destroy(node));
    vSemaphoreDelete(command_ctx.entered);
    vSemaphoreDelete(command_ctx.release);
}

TEST_CASE("node.invoke.request is ignored until handshake reaches ready", "[esp_openclaw_node][protocol]")
{
    reset_openclaw_storage();
    reset_transport_state();

    esp_openclaw_node_config_t config = {0};
    esp_openclaw_node_config_init_default(&config);
    config.event_cb = test_node_event_cb;

    esp_openclaw_node_handle_t node = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_create(&config, &node));

    blocking_command_ctx_t command_ctx = {
        .entered = xSemaphoreCreateBinary(),
        .release = xSemaphoreCreateBinary(),
    };
    TEST_ASSERT_NOT_NULL(command_ctx.entered);
    TEST_ASSERT_NOT_NULL(command_ctx.release);

    esp_openclaw_node_command_t command = {
        .name = "block",
        .handler = blocking_command_handler,
        .context = &command_ctx,
    };
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_register_command(node, &command));

    reset_event_recorder();
    TEST_ASSERT_EQUAL(
        ESP_OK,
        request_connect_no_auth(node, "ws://gateway.example/ws"));
    TEST_ASSERT_TRUE(wait_for_int_value(&s_transport_state.start_calls, 1, pdMS_TO_TICKS(1000)));

    emit_ws_event(WEBSOCKET_EVENT_CONNECTED, NULL, ESP_OK);
    TEST_ASSERT_TRUE(
        wait_for_int_value(&s_transport_state.send_with_opcode_calls, 1, pdMS_TO_TICKS(1000)));

    emit_ws_event(
        WEBSOCKET_EVENT_DATA,
        "{\"type\":\"event\",\"event\":\"connect.challenge\",\"payload\":{\"nonce\":\"nonce-1\",\"ts\":123}}",
        ESP_OK);
    TEST_ASSERT_TRUE(wait_for_int_value(&s_transport_state.send_text_calls, 1, pdMS_TO_TICKS(1000)));

    char *connect_id = extract_first_json_id(s_transport_state.last_sent_text);
    TEST_ASSERT_NOT_NULL(connect_id);

    emit_ws_event(
        WEBSOCKET_EVENT_DATA,
        "{\"type\":\"event\",\"event\":\"node.invoke.request\",\"payload\":{\"id\":\"invoke-early\",\"nodeId\":\"node-1\",\"command\":\"block\",\"paramsJSON\":\"{}\"}}",
        ESP_OK);
    TEST_ASSERT_TRUE(xSemaphoreTake(command_ctx.entered, pdMS_TO_TICKS(100)) == pdFALSE);
    TEST_ASSERT_EQUAL(1, s_transport_state.send_text_calls);
    TEST_ASSERT_EQUAL(0, s_event_recorder.connected_count);

    char connect_response[512] = {0};
    snprintf(
        connect_response,
        sizeof(connect_response),
        "{\"type\":\"res\",\"id\":\"%s\",\"ok\":true,\"payload\":{\"type\":\"hello-ok\",\"auth\":{\"deviceToken\":\"device-token-123\"}}}",
        connect_id);
    free(connect_id);

    emit_ws_event(WEBSOCKET_EVENT_DATA, connect_response, ESP_OK);
    TEST_ASSERT_TRUE(wait_for_event(ESP_OPENCLAW_NODE_EVENT_CONNECTED, pdMS_TO_TICKS(1000)));

    emit_ws_event(
        WEBSOCKET_EVENT_DATA,
        "{\"type\":\"event\",\"event\":\"node.invoke.request\",\"payload\":{\"id\":\"invoke-ready\",\"nodeId\":\"node-1\",\"command\":\"block\",\"paramsJSON\":\"{}\"}}",
        ESP_OK);
    TEST_ASSERT_TRUE(xSemaphoreTake(command_ctx.entered, pdMS_TO_TICKS(1000)) == pdTRUE);
    TEST_ASSERT_TRUE(xSemaphoreGive(command_ctx.release) == pdTRUE);
    TEST_ASSERT_TRUE(wait_for_int_value(&s_transport_state.send_text_calls, 2, pdMS_TO_TICKS(1000)));

    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_destroy(node));
    vSemaphoreDelete(command_ctx.entered);
    vSemaphoreDelete(command_ctx.release);
}

TEST_CASE("disconnect while transport start is in progress cancels the attempt", "[esp_openclaw_node][transport]")
{
    reset_openclaw_storage();
    reset_transport_state();

    esp_openclaw_node_config_t config = {0};
    esp_openclaw_node_config_init_default(&config);
    config.event_cb = test_node_event_cb;

    esp_openclaw_node_handle_t node = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_create(&config, &node));

    s_transport_state.start_entered = xSemaphoreCreateBinary();
    s_transport_state.start_release = xSemaphoreCreateBinary();
    s_transport_state.start_result = ESP_FAIL;
    TEST_ASSERT_NOT_NULL(s_transport_state.start_entered);
    TEST_ASSERT_NOT_NULL(s_transport_state.start_release);

    reset_event_recorder();
    TEST_ASSERT_EQUAL(
        ESP_OK,
        request_connect_no_auth(node, "ws://gateway.example/ws"));
    TEST_ASSERT_TRUE(xSemaphoreTake(s_transport_state.start_entered, pdMS_TO_TICKS(1000)) == pdTRUE);
    /* The cancel is accepted while the attempt is connecting; it is processed
     * after the blocked transport start returns. */
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_request_disconnect(node));
    TEST_ASSERT_TRUE(xSemaphoreGive(s_transport_state.start_release) == pdTRUE);

    TEST_ASSERT_TRUE(wait_for_event(ESP_OPENCLAW_NODE_EVENT_CONNECT_FAILED, pdMS_TO_TICKS(1000)));
    TEST_ASSERT_EQUAL(0, s_event_recorder.connected_count);
    TEST_ASSERT_EQUAL(0, s_event_recorder.disconnected_count);
    TEST_ASSERT_FALSE(esp_openclaw_node_has_saved_session(node));

    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_destroy(node));
    vSemaphoreDelete(s_transport_state.start_entered);
    vSemaphoreDelete(s_transport_state.start_release);
    s_transport_state.start_entered = NULL;
    s_transport_state.start_release = NULL;
}

TEST_CASE("disconnect after connect request send cancels before hello-ok lands", "[esp_openclaw_node][transport]")
{
    reset_openclaw_storage();
    reset_transport_state();

    esp_openclaw_node_config_t config = {0};
    esp_openclaw_node_config_init_default(&config);
    config.event_cb = test_node_event_cb;

    esp_openclaw_node_handle_t node = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_create(&config, &node));

    s_transport_state.send_text_entered = xSemaphoreCreateBinary();
    s_transport_state.send_text_release = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL(s_transport_state.send_text_entered);
    TEST_ASSERT_NOT_NULL(s_transport_state.send_text_release);

    reset_event_recorder();
    TEST_ASSERT_EQUAL(
        ESP_OK,
        request_connect_no_auth(node, "ws://gateway.example/ws"));
    TEST_ASSERT_TRUE(wait_for_int_value(&s_transport_state.start_calls, 1, pdMS_TO_TICKS(1000)));

    emit_ws_event(WEBSOCKET_EVENT_CONNECTED, NULL, ESP_OK);
    TEST_ASSERT_TRUE(
        wait_for_int_value(&s_transport_state.send_with_opcode_calls, 1, pdMS_TO_TICKS(1000)));
    emit_ws_event(
        WEBSOCKET_EVENT_DATA,
        "{\"type\":\"event\",\"event\":\"connect.challenge\",\"payload\":{\"nonce\":\"nonce-1\",\"ts\":123}}",
        ESP_OK);
    TEST_ASSERT_TRUE(xSemaphoreTake(s_transport_state.send_text_entered, pdMS_TO_TICKS(1000)) == pdTRUE);

    char *connect_id = extract_first_json_id(s_transport_state.last_sent_text);
    TEST_ASSERT_NOT_NULL(connect_id);
    /* The cancel is accepted while connecting and is queued ahead of the
     * hello-ok data event, so the attempt ends CONNECT_FAILED(CANCELED) and
     * the late hello-ok is dropped against the torn-down transport. */
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_request_disconnect(node));

    char connect_response[512] = {0};
    snprintf(
        connect_response,
        sizeof(connect_response),
        "{\"type\":\"res\",\"id\":\"%s\",\"ok\":true,\"payload\":{\"type\":\"hello-ok\",\"auth\":{\"deviceToken\":\"device-token-123\"}}}",
        connect_id);
    free(connect_id);
    TEST_ASSERT_TRUE(xSemaphoreGive(s_transport_state.send_text_release) == pdTRUE);
    emit_ws_event(WEBSOCKET_EVENT_DATA, connect_response, ESP_OK);

    TEST_ASSERT_TRUE(wait_for_event(ESP_OPENCLAW_NODE_EVENT_CONNECT_FAILED, pdMS_TO_TICKS(2000)));
    TEST_ASSERT_EQUAL(0, s_event_recorder.connected_count);
    TEST_ASSERT_EQUAL(1, s_event_recorder.connect_failed_count);
    TEST_ASSERT_EQUAL(0, s_event_recorder.disconnected_count);
    TEST_ASSERT_EQUAL(
        ESP_OPENCLAW_NODE_CONNECT_FAILURE_CANCELED,
        s_event_recorder.connect_failed_reason);
    TEST_ASSERT_FALSE(esp_openclaw_node_has_saved_session(node));

    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_destroy(node));
    vSemaphoreDelete(s_transport_state.send_text_entered);
    vSemaphoreDelete(s_transport_state.send_text_release);
    s_transport_state.send_text_entered = NULL;
    s_transport_state.send_text_release = NULL;
}

TEST_CASE("clean websocket close keeps local err clear", "[esp_openclaw_node][transport]")
{
    reset_openclaw_storage();
    reset_transport_state();

    esp_openclaw_node_config_t config = {0};
    esp_openclaw_node_config_init_default(&config);
    config.event_cb = test_node_event_cb;

    esp_openclaw_node_handle_t node = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_create(&config, &node));

    reset_event_recorder();
    TEST_ASSERT_EQUAL(
        ESP_OK,
        request_connect_no_auth(node, "ws://gateway.example/ws"));
    TEST_ASSERT_TRUE(wait_for_int_value(&s_transport_state.start_calls, 1, pdMS_TO_TICKS(1000)));

    emit_ws_event(WEBSOCKET_EVENT_CONNECTED, NULL, ESP_OK);
    TEST_ASSERT_TRUE(
        wait_for_int_value(&s_transport_state.send_with_opcode_calls, 1, pdMS_TO_TICKS(1000)));
    emit_ws_event(
        WEBSOCKET_EVENT_DATA,
        "{\"type\":\"event\",\"event\":\"connect.challenge\",\"payload\":{\"nonce\":\"nonce-1\",\"ts\":123}}",
        ESP_OK);
    TEST_ASSERT_TRUE(wait_for_int_value(&s_transport_state.send_text_calls, 1, pdMS_TO_TICKS(1000)));

    char *connect_id = extract_first_json_id(s_transport_state.last_sent_text);
    TEST_ASSERT_NOT_NULL(connect_id);

    char connect_response[512] = {0};
    snprintf(
        connect_response,
        sizeof(connect_response),
        "{\"type\":\"res\",\"id\":\"%s\",\"ok\":true,\"payload\":{\"type\":\"hello-ok\",\"auth\":{\"deviceToken\":\"device-token-123\"}}}",
        connect_id);
    free(connect_id);
    emit_ws_event(WEBSOCKET_EVENT_DATA, connect_response, ESP_OK);
    TEST_ASSERT_TRUE(wait_for_event(ESP_OPENCLAW_NODE_EVENT_CONNECTED, pdMS_TO_TICKS(1000)));

    reset_event_recorder();
    emit_ws_event(WEBSOCKET_EVENT_DISCONNECTED, NULL, ESP_OK);
    TEST_ASSERT_TRUE(wait_for_event(ESP_OPENCLAW_NODE_EVENT_DISCONNECTED, pdMS_TO_TICKS(1000)));
    TEST_ASSERT_EQUAL(ESP_OK, s_event_recorder.local_err);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_NODE_DISCONNECTED_REASON_CONNECTION_LOST, s_event_recorder.disconnected_reason);

    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_destroy(node));
}

TEST_CASE("gateway URI accessor prefers active transport and falls back to saved session", "[esp_openclaw_node][session]")
{
    reset_openclaw_storage();
    reset_transport_state();
    char saved_uri[] = "wss://saved.example/ws";
    char saved_token[] = "saved-token";
    esp_openclaw_node_persisted_session_t session = {0};
    const esp_openclaw_node_persisted_session_t update = {
        .version = 1,
        .gateway_uri = saved_uri,
        .device_token = saved_token,
    };
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_persisted_session_store("node", &session, &update));
    esp_openclaw_node_persisted_session_free(&session);

    esp_openclaw_node_config_t config = {0};
    esp_openclaw_node_config_init_default(&config);
    esp_openclaw_node_handle_t node = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_create(&config, &node));
    char *uri = esp_openclaw_node_dup_gateway_uri(node);
    TEST_ASSERT_EQUAL_STRING("wss://saved.example/ws", uri);
    free(uri);

    TEST_ASSERT_EQUAL(ESP_OK, request_connect_no_auth(node, "ws://active.example/ws"));
    TEST_ASSERT_TRUE(wait_for_int_value(&s_transport_state.start_calls, 1, pdMS_TO_TICKS(1000)));
    uri = esp_openclaw_node_dup_gateway_uri(node);
    TEST_ASSERT_EQUAL_STRING("ws://active.example/ws", uri);
    free(uri);
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_destroy(node));
    TEST_ASSERT_NULL(esp_openclaw_node_dup_gateway_uri(NULL));
}

TEST_CASE("inbound message ceiling accepts below and fragmented exact limit", "[esp_openclaw_node][transport]")
{
    reset_openclaw_storage();
    reset_transport_state();
    esp_openclaw_node_config_t config = {0};
    esp_openclaw_node_config_init_default(&config);
    esp_openclaw_node_handle_t node = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_create(&config, &node));
    TEST_ASSERT_EQUAL(ESP_OK, request_connect_no_auth(node, "ws://gateway.example/ws"));
    TEST_ASSERT_TRUE(wait_for_int_value(&s_transport_state.start_calls, 1, pdMS_TO_TICKS(1000)));

    char small[] = "{}";
    emit_ws_frame_chunk(0x01, small, 2, 2, 0, true);

    const int half = ESP_OPENCLAW_NODE_MAX_INBOUND_MESSAGE_SIZE / 2;
    char *chunk = calloc((size_t)half, 1);
    TEST_ASSERT_NOT_NULL(chunk);
    memset(chunk, ' ', (size_t)half);
    emit_ws_frame_chunk(0x01, chunk, half, half, 0, false);
    TEST_ASSERT_NOT_NULL(node->rx_buffer);
    TEST_ASSERT_EQUAL_UINT32(
        half,
        (uint32_t)node->rx_buffer_len);
    emit_ws_frame_chunk(0x00, chunk, half, half, 0, true);
    free(chunk);
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_destroy(node));
}

TEST_CASE("inbound message ceiling rejects above limit before allocation", "[esp_openclaw_node][transport]")
{
    reset_openclaw_storage();
    reset_transport_state();
    reset_event_recorder();
    esp_openclaw_node_config_t config = {0};
    esp_openclaw_node_config_init_default(&config);
    config.event_cb = test_node_event_cb;
    esp_openclaw_node_handle_t node = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_create(&config, &node));
    TEST_ASSERT_EQUAL(ESP_OK, request_connect_no_auth(node, "ws://gateway.example/ws"));
    TEST_ASSERT_TRUE(wait_for_int_value(&s_transport_state.start_calls, 1, pdMS_TO_TICKS(1000)));

    char *exact = calloc(ESP_OPENCLAW_NODE_MAX_INBOUND_MESSAGE_SIZE, 1);
    TEST_ASSERT_NOT_NULL(exact);
    memset(exact, ' ', ESP_OPENCLAW_NODE_MAX_INBOUND_MESSAGE_SIZE);
    emit_ws_frame_chunk(
        0x01,
        exact,
        ESP_OPENCLAW_NODE_MAX_INBOUND_MESSAGE_SIZE,
        ESP_OPENCLAW_NODE_MAX_INBOUND_MESSAGE_SIZE,
        0,
        false);
    char byte = ' ';
    emit_ws_frame_chunk(0x00, &byte, 1, 1, 0, true);
    free(exact);
    TEST_ASSERT_NULL(node->rx_buffer);
    TEST_ASSERT_TRUE(wait_for_event(ESP_OPENCLAW_NODE_EVENT_CONNECT_FAILED, pdMS_TO_TICKS(1000)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, s_event_recorder.local_err);
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_destroy(node));
}

TEST_CASE("malformed websocket continuation is terminal", "[esp_openclaw_node][transport]")
{
    reset_openclaw_storage();
    reset_transport_state();
    reset_event_recorder();
    esp_openclaw_node_config_t config = {0};
    esp_openclaw_node_config_init_default(&config);
    config.event_cb = test_node_event_cb;
    esp_openclaw_node_handle_t node = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_create(&config, &node));
    TEST_ASSERT_EQUAL(ESP_OK, request_connect_no_auth(node, "ws://gateway.example/ws"));
    TEST_ASSERT_TRUE(wait_for_int_value(&s_transport_state.start_calls, 1, pdMS_TO_TICKS(1000)));

    char byte = 'x';
    emit_ws_frame_chunk(0x00, &byte, 1, 1, 0, true);
    TEST_ASSERT_TRUE(wait_for_event(ESP_OPENCLAW_NODE_EVENT_CONNECT_FAILED, pdMS_TO_TICKS(1000)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, s_event_recorder.local_err);
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_destroy(node));
}

TEST_CASE("v2 command dispatch receives bounded invocation session metadata", "[esp_openclaw_node][protocol]")
{
    reset_openclaw_storage();
    reset_transport_state();
    esp_openclaw_node_config_t config = {0};
    esp_openclaw_node_config_init_default(&config);
    esp_openclaw_node_handle_t node = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_create(&config, &node));

    invocation_command_ctx_t ctx = {0};
    const esp_openclaw_node_command_v2_t command = {
        .name = "metadata.test",
        .handler = invocation_command_handler,
        .context = &ctx,
    };
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_register_command_v2(node, &command));
    esp_openclaw_node_lock_state(node);
    node->state = ESP_OPENCLAW_NODE_INTERNAL_READY;
    esp_openclaw_node_unlock_state(node);

    esp_openclaw_node_process_gateway_message(
        node,
        "{\"type\":\"event\",\"event\":\"node.invoke.request\",\"payload\":{\"id\":\"invoke-1\",\"nodeId\":\"node-1\",\"command\":\"metadata.test\",\"paramsJSON\":\"{}\",\"sessionKey\":\"agent:main:room\"}}");
    TEST_ASSERT_TRUE(ctx.called);
    TEST_ASSERT_EQUAL_STRING("agent:main:room", ctx.session_key);

    ctx.called = false;
    esp_openclaw_node_process_gateway_message(
        node,
        "{\"type\":\"event\",\"event\":\"node.invoke.request\",\"payload\":{\"id\":\"invoke-2\",\"nodeId\":\"node-1\",\"command\":\"metadata.test\",\"paramsJSON\":\"{}\",\"sessionKey\":null}}");
    TEST_ASSERT_FALSE(ctx.called);

    esp_openclaw_node_lock_state(node);
    node->state = ESP_OPENCLAW_NODE_INTERNAL_IDLE;
    esp_openclaw_node_unlock_state(node);
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_destroy(node));
}

TEST_CASE("device token source signs auth material", "[esp_openclaw_node][auth]")
{
    reset_openclaw_storage();
    reset_transport_state();

    esp_openclaw_node_config_t config = {0};
    esp_openclaw_node_config_init_default(&config);
    esp_openclaw_node_handle_t node = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_create(&config, &node));

    const esp_openclaw_node_connect_request_t request = {
        .source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_DEVICE_TOKEN,
        .gateway_uri = "wss://gateway.example/ws",
        .value = "role-token",
    };
    esp_openclaw_node_connect_request_source_t source = {0};
    TEST_ASSERT_EQUAL(
        ESP_OK,
        esp_openclaw_node_build_connect_source_from_request(&request, &source));
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_DEVICE_TOKEN, source.kind);

    esp_openclaw_node_connect_material_t material = {0};
    esp_openclaw_node_lock_state(node);
    node->active_connect_source = source;
    memset(&source, 0, sizeof(source));
    TEST_ASSERT_EQUAL(
        ESP_OK,
        esp_openclaw_node_resolve_active_connect_material_locked(node, &material));
    esp_openclaw_node_unlock_state(node);
    TEST_ASSERT_EQUAL_STRING("role-token", material.auth_value);
    TEST_ASSERT_EQUAL_PTR(material.auth_value, material.signature_token);

    esp_openclaw_node_free_connect_material(&material);
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_destroy(node));
}

TEST_CASE("scope registration stays idempotent at capacity", "[esp_openclaw_node][registry]")
{
    reset_openclaw_storage();
    reset_transport_state();

    esp_openclaw_node_config_t config = {0};
    esp_openclaw_node_config_init_default(&config);
    esp_openclaw_node_handle_t node = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_create(&config, &node));

    char scope[32] = {0};
    for (size_t i = 0; i < ESP_OPENCLAW_NODE_MAX_SCOPES; ++i) {
        snprintf(scope, sizeof(scope), "operator.test.%u", (unsigned)i);
        TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_register_scope(node, scope));
    }
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_register_scope(node, "operator.test.0"));
    TEST_ASSERT_EQUAL(
        ESP_ERR_NO_MEM,
        esp_openclaw_node_register_scope(node, "operator.test.overflow"));

    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_destroy(node));
}

TEST_CASE("voice bootstrap persists roles and correlates gateway traffic", "[esp_openclaw_node][voice]")
{
    reset_openclaw_storage();
    reset_transport_state();
    memset(&s_gateway_recorder, 0, sizeof(s_gateway_recorder));

    esp_openclaw_node_config_t config = {0};
    esp_openclaw_node_config_init_default(&config);
    config.event_cb = test_node_event_cb;
    config.gateway_event_cb = test_gateway_event_cb;
    config.gateway_event_user_ctx = &s_gateway_recorder;
    esp_openclaw_node_handle_t node = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_create(&config, &node));
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_register_scope(node, "operator.read"));
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_register_scope(node, "operator.talk"));

    reset_event_recorder();
    TEST_ASSERT_EQUAL(ESP_OK, request_connect_no_auth(node, "ws://gateway.example/ws"));
    TEST_ASSERT_TRUE(wait_for_int_value(&s_transport_state.start_calls, 1, pdMS_TO_TICKS(1000)));
    emit_ws_event(WEBSOCKET_EVENT_CONNECTED, NULL, ESP_OK);
    emit_ws_event(
        WEBSOCKET_EVENT_DATA,
        "{\"type\":\"event\",\"event\":\"connect.challenge\",\"payload\":{\"nonce\":\"voice-nonce\",\"ts\":123}}",
        ESP_OK);
    TEST_ASSERT_TRUE(wait_for_int_value(&s_transport_state.send_text_calls, 1, pdMS_TO_TICKS(1000)));
    TEST_ASSERT_NOT_NULL(strstr(s_transport_state.last_sent_text, "\"operator.read\""));
    TEST_ASSERT_NOT_NULL(strstr(s_transport_state.last_sent_text, "\"operator.talk\""));

    char *connect_id = extract_first_json_id(s_transport_state.last_sent_text);
    char connect_response[768] = {0};
    snprintf(
        connect_response,
        sizeof(connect_response),
        "{\"type\":\"res\",\"id\":\"%s\",\"ok\":true,\"payload\":{\"type\":\"hello-ok\",\"auth\":{\"deviceToken\":\"node-token\",\"deviceTokens\":[{\"role\":\"operator\",\"deviceToken\":\"operator-token\",\"scopes\":[\"operator.read\",\"operator.talk\"]}]}}}",
        connect_id);
    free(connect_id);
    emit_ws_event(WEBSOCKET_EVENT_DATA, connect_response, ESP_OK);
    TEST_ASSERT_TRUE(wait_for_event(ESP_OPENCLAW_NODE_EVENT_CONNECTED, pdMS_TO_TICKS(1000)));

    esp_openclaw_node_persisted_session_t operator_session = {0};
    TEST_ASSERT_EQUAL(
        ESP_OK,
        esp_openclaw_node_persisted_session_load("operator", &operator_session));
    TEST_ASSERT_EQUAL_STRING("ws://gateway.example/ws", operator_session.gateway_uri);
    TEST_ASSERT_EQUAL_STRING("operator-token", operator_session.device_token);
    esp_openclaw_node_persisted_session_free(&operator_session);

    TEST_ASSERT_EQUAL(
        ESP_OK,
        esp_openclaw_node_gateway_request(
            node,
            "talk.catalog",
            "{}",
            test_gateway_request_cb,
            &s_gateway_recorder));
    TEST_ASSERT_TRUE(wait_for_int_value(&s_transport_state.send_text_calls, 2, pdMS_TO_TICKS(1000)));
    char *request_id = extract_first_json_id(s_transport_state.last_sent_text);
    char response[256] = {0};
    snprintf(
        response,
        sizeof(response),
        "{\"type\":\"res\",\"id\":\"%s\",\"ok\":true,\"payload\":{\"providers\":[\"openai\"]}}",
        request_id);
    free(request_id);
    emit_ws_event(WEBSOCKET_EVENT_DATA, response, ESP_OK);
    TEST_ASSERT_TRUE(wait_for_bool(&s_gateway_recorder.request_seen, pdMS_TO_TICKS(1000)));
    TEST_ASSERT_TRUE(s_gateway_recorder.request_ok);
    TEST_ASSERT_NOT_NULL(strstr(s_gateway_recorder.request_payload, "openai"));

    emit_ws_event(
        WEBSOCKET_EVENT_DATA,
        "{\"type\":\"event\",\"event\":\"voicewake.changed\",\"payload\":{\"triggers\":[\"Hi ESP\"]}}",
        ESP_OK);
    TEST_ASSERT_TRUE(wait_for_bool(&s_gateway_recorder.event_seen, pdMS_TO_TICKS(1000)));
    TEST_ASSERT_EQUAL_STRING("voicewake.changed", s_gateway_recorder.event);

    s_gateway_recorder.request_seen = false;
    TEST_ASSERT_EQUAL(
        ESP_OK,
        esp_openclaw_node_gateway_request(
            node,
            "talk.catalog",
            "{}",
            test_gateway_request_cb,
            &s_gateway_recorder));
    TEST_ASSERT_TRUE(wait_for_int_value(&s_transport_state.send_text_calls, 3, pdMS_TO_TICKS(1000)));
    emit_ws_event(WEBSOCKET_EVENT_DISCONNECTED, NULL, ESP_FAIL);
    TEST_ASSERT_TRUE(wait_for_bool(&s_gateway_recorder.request_seen, pdMS_TO_TICKS(1000)));
    TEST_ASSERT_FALSE(s_gateway_recorder.request_ok);
    TEST_ASSERT_EQUAL_STRING("DISCONNECTED", s_gateway_recorder.request_error);

    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_node_destroy(node));
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
}
