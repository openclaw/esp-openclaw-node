/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_openclaw_node_persisted_session.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "esp_openclaw_node_session";
static const char *NVS_NAMESPACE = "openclaw";
static const char *NVS_KEY_VERSION = "session_v";
static const char *NVS_KEY_URI = "session_uri";
static const char *NVS_KEY_DEVICE_TOKEN = "session_dev_tok";
static const char *NVS_KEY_OPERATOR_VERSION = "op_v";
static const char *NVS_KEY_OPERATOR_URI = "op_uri";
static const char *NVS_KEY_OPERATOR_DEVICE_TOKEN = "op_dev_tok";
static const uint8_t PERSISTED_SESSION_VERSION = 1;

typedef struct {
    const char *version;
    const char *uri;
    const char *device_token;
} persisted_session_keys_t;

static esp_err_t resolve_session_keys(
    const char *role,
    persisted_session_keys_t *keys)
{
    if (role == NULL || keys == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(role, "node") == 0) {
        *keys = (persisted_session_keys_t){
            .version = NVS_KEY_VERSION,
            .uri = NVS_KEY_URI,
            .device_token = NVS_KEY_DEVICE_TOKEN,
        };
        return ESP_OK;
    }
    if (strcmp(role, "operator") == 0) {
        *keys = (persisted_session_keys_t){
            .version = NVS_KEY_OPERATOR_VERSION,
            .uri = NVS_KEY_OPERATOR_URI,
            .device_token = NVS_KEY_OPERATOR_DEVICE_TOKEN,
        };
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t write_persisted_session_to_storage(
    const persisted_session_keys_t *keys,
    const esp_openclaw_node_persisted_session_t *update);

static char *duplicate_string(const char *value)
{
    if (value == NULL) {
        return NULL;
    }
    char *copy = strdup(value);
    if (copy == NULL) {
        return NULL;
    }
    return copy;
}

static bool is_valid_gateway_uri(const char *gateway_uri)
{
    if (gateway_uri == NULL || gateway_uri[0] == '\0') {
        return false;
    }
    return strncmp(gateway_uri, "ws://", 5) == 0 || strncmp(gateway_uri, "wss://", 6) == 0;
}

static void clear_persisted_session_struct(esp_openclaw_node_persisted_session_t *session)
{
    free(session->gateway_uri);
    session->gateway_uri = NULL;
    free(session->device_token);
    session->device_token = NULL;
    session->version = 0;
}

static esp_err_t validate_persisted_session(const esp_openclaw_node_persisted_session_t *session)
{
    if (session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bool has_uri = session->gateway_uri != NULL && session->gateway_uri[0] != '\0';
    bool has_device_token = session->device_token != NULL && session->device_token[0] != '\0';
    if (has_uri != has_device_token) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!has_uri) {
        return ESP_OK;
    }
    if (!is_valid_gateway_uri(session->gateway_uri)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t load_optional_string(
    nvs_handle_t nvs,
    const char *key,
    char **out_value)
{
    size_t required = 0;
    esp_err_t err = nvs_get_str(nvs, key, NULL, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out_value = NULL;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "failed reading string size");

    char *value = malloc(required);
    if (value == NULL) {
        return ESP_ERR_NO_MEM;
    }
    err = nvs_get_str(nvs, key, value, &required);
    if (err != ESP_OK) {
        free(value);
        return err;
    }
    if (value[0] == '\0') {
        free(value);
        value = NULL;
    }
    *out_value = value;
    return ESP_OK;
}

static esp_err_t persist_optional_string(
    nvs_handle_t nvs,
    const char *key,
    const char *value)
{
    if (value == NULL || value[0] == '\0') {
        esp_err_t err = nvs_erase_key(nvs, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            return ESP_OK;
        }
        return err;
    }
    return nvs_set_str(nvs, key, value);
}

static esp_err_t clear_session_keys(
    nvs_handle_t nvs,
    const persisted_session_keys_t *keys)
{
    esp_err_t err = nvs_erase_key(nvs, keys->version);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }
    err = nvs_erase_key(nvs, keys->uri);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }
    err = nvs_erase_key(nvs, keys->device_token);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }
    return nvs_commit(nvs);
}

static esp_err_t clear_invalid_loaded_session(
    nvs_handle_t nvs,
    const persisted_session_keys_t *keys,
    esp_openclaw_node_persisted_session_t *session,
    const char *reason)
{
    ESP_LOGW(TAG, "discarding malformed persisted session: %s", reason);
    clear_persisted_session_struct(session);

    esp_err_t clear_err = clear_session_keys(nvs, keys);
    if (clear_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "failed clearing malformed persisted session: %s",
            esp_err_to_name(clear_err));
    }
    return ESP_OK;
}

static esp_err_t copy_persisted_session(
    esp_openclaw_node_persisted_session_t *dst,
    const esp_openclaw_node_persisted_session_t *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->version = src->gateway_uri != NULL ? PERSISTED_SESSION_VERSION : 0;
    dst->gateway_uri = duplicate_string(src->gateway_uri);
    dst->device_token = duplicate_string(src->device_token);
    if ((src->gateway_uri != NULL && dst->gateway_uri == NULL) ||
        (src->device_token != NULL && dst->device_token == NULL)) {
        clear_persisted_session_struct(dst);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t write_persisted_session_to_storage(
    const persisted_session_keys_t *keys,
    const esp_openclaw_node_persisted_session_t *update)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    if (!esp_openclaw_node_persisted_session_is_present(update)) {
        err = clear_session_keys(nvs, keys);
        nvs_close(nvs);
        return err;
    }

    err = nvs_set_u8(nvs, keys->version, PERSISTED_SESSION_VERSION);
    if (err == ESP_OK) {
        err = persist_optional_string(nvs, keys->uri, update->gateway_uri);
    }
    if (err == ESP_OK) {
        err = persist_optional_string(nvs, keys->device_token, update->device_token);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

bool esp_openclaw_node_persisted_session_is_present(const esp_openclaw_node_persisted_session_t *session)
{
    return session != NULL &&
           session->gateway_uri != NULL &&
           session->gateway_uri[0] != '\0' &&
           session->device_token != NULL &&
           session->device_token[0] != '\0';
}

esp_err_t esp_openclaw_node_persisted_session_load(
    const char *role,
    esp_openclaw_node_persisted_session_t *session)
{
    if (session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(session, 0, sizeof(*session));
    persisted_session_keys_t keys = {0};
    ESP_RETURN_ON_ERROR(resolve_session_keys(role, &keys), TAG, "invalid role");

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t version = 0;
    err = nvs_get_u8(nvs, keys.version, &version);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }
    if (version != PERSISTED_SESSION_VERSION) {
        ESP_LOGW(
            TAG,
            "ignoring unsupported persisted session version %u",
            (unsigned)version);
        esp_err_t clear_err = clear_session_keys(nvs, &keys);
        nvs_close(nvs);
        if (clear_err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "failed clearing unsupported persisted session: %s",
                esp_err_to_name(clear_err));
        }
        return ESP_OK;
    }

    session->version = version;
    err = load_optional_string(nvs, keys.uri, &session->gateway_uri);
    if (err == ESP_OK) {
        err = load_optional_string(nvs, keys.device_token, &session->device_token);
    }
    if (err == ESP_OK) {
        err = validate_persisted_session(session);
        if (err == ESP_ERR_INVALID_ARG) {
            err = clear_invalid_loaded_session(
                nvs,
                &keys,
                session,
                "incomplete or invalid fields");
        }
    }
    nvs_close(nvs);

    if (err != ESP_OK) {
        esp_openclaw_node_persisted_session_free(session);
    }
    return err;
}

void esp_openclaw_node_persisted_session_free(esp_openclaw_node_persisted_session_t *session)
{
    if (session == NULL) {
        return;
    }
    clear_persisted_session_struct(session);
}

esp_err_t esp_openclaw_node_persisted_session_store(
    const char *role,
    esp_openclaw_node_persisted_session_t *session,
    const esp_openclaw_node_persisted_session_t *update)
{
    if (role == NULL || session == NULL || update == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    persisted_session_keys_t keys = {0};
    ESP_RETURN_ON_ERROR(resolve_session_keys(role, &keys), TAG, "invalid role");
    ESP_RETURN_ON_ERROR(validate_persisted_session(update), TAG, "invalid persisted session");

    esp_openclaw_node_persisted_session_t copy = {0};
    if (esp_openclaw_node_persisted_session_is_present(update)) {
        ESP_RETURN_ON_ERROR(copy_persisted_session(&copy, update), TAG, "copy session");
    }

    esp_err_t err = write_persisted_session_to_storage(&keys, update);
    if (err != ESP_OK) {
        esp_openclaw_node_persisted_session_free(&copy);
        return err;
    }

    esp_openclaw_node_persisted_session_free(session);
    *session = copy;
    return ESP_OK;
}
