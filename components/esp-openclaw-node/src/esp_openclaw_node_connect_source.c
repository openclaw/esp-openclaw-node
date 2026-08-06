/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_openclaw_node_internal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "mbedtls/base64.h"

static bool connect_source_requires_secret(esp_openclaw_node_connect_source_kind_t kind)
{
    return kind == ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_BOOTSTRAP_TOKEN ||
           kind == ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_DEVICE_TOKEN ||
           kind == ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SHARED_TOKEN ||
           kind == ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_PASSWORD;
}

void esp_openclaw_node_clear_connect_source_struct(
    esp_openclaw_node_connect_request_source_t *source)
{
    if (source == NULL) {
        return;
    }
    free(source->gateway_uri);
    source->gateway_uri = NULL;
    free(source->secret);
    source->secret = NULL;
    source->kind = ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_NONE;
}

static esp_err_t validate_connect_source(
    const esp_openclaw_node_connect_request_source_t *source)
{
    if (source == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    switch (source->kind) {
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SAVED_SESSION:
        if (source->gateway_uri != NULL || source->secret != NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        return ESP_OK;
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_BOOTSTRAP_TOKEN:
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_DEVICE_TOKEN:
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SHARED_TOKEN:
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_PASSWORD:
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_NO_AUTH:
        if (!esp_openclaw_node_is_valid_gateway_uri(source->gateway_uri)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (connect_source_requires_secret(source->kind) &&
            esp_openclaw_node_trimmed_or_null(source->secret) == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        if (!connect_source_requires_secret(source->kind) &&
            esp_openclaw_node_trimmed_or_null(source->secret) != NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        return ESP_OK;
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_NONE:
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t decode_base64url_payload(
    const char *encoded,
    char **out_decoded)
{
    if (encoded == NULL || out_decoded == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *trimmed = esp_openclaw_node_trimmed_or_null(encoded);
    if (trimmed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t encoded_len = strlen(trimmed);
    size_t padded_len = ((encoded_len + 3U) / 4U) * 4U;
    char *padded = calloc(padded_len + 1U, sizeof(char));
    if (padded == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < encoded_len; ++i) {
        if (trimmed[i] == '-') {
            padded[i] = '+';
        } else if (trimmed[i] == '_') {
            padded[i] = '/';
        } else {
            padded[i] = trimmed[i];
        }
    }
    for (size_t i = encoded_len; i < padded_len; ++i) {
        padded[i] = '=';
    }

    size_t decoded_capacity = ((padded_len / 4U) * 3U) + 1U;
    unsigned char *decoded = calloc(decoded_capacity, sizeof(unsigned char));
    if (decoded == NULL) {
        free(padded);
        return ESP_ERR_NO_MEM;
    }

    size_t written = 0;
    int rc = mbedtls_base64_decode(
        decoded,
        decoded_capacity - 1U,
        &written,
        (const unsigned char *)padded,
        padded_len);
    free(padded);
    if (rc != 0) {
        free(decoded);
        return ESP_ERR_INVALID_ARG;
    }

    decoded[written] = '\0';
    *out_decoded = (char *)decoded;
    return ESP_OK;
}

static esp_err_t parse_setup_code(
    const char *setup_code,
    esp_openclaw_node_connect_request_source_t *out_source)
{
    if (setup_code == NULL || out_source == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_source, 0, sizeof(*out_source));

    char *decoded_json = NULL;
    ESP_RETURN_ON_ERROR(
        decode_base64url_payload(setup_code, &decoded_json),
        ESP_OPENCLAW_NODE_TAG,
        "invalid setup code encoding");

    cJSON *root = cJSON_Parse(decoded_json);
    free(decoded_json);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "url");
    cJSON *bootstrap_token =
        cJSON_GetObjectItemCaseSensitive(root, "bootstrapToken");
    cJSON *shared_token = cJSON_GetObjectItemCaseSensitive(root, "token");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");

    const char *bootstrap_text = cJSON_IsString(bootstrap_token)
        ? esp_openclaw_node_trimmed_or_null(bootstrap_token->valuestring)
        : NULL;
    const char *shared_text = cJSON_IsString(shared_token)
        ? esp_openclaw_node_trimmed_or_null(shared_token->valuestring)
        : NULL;
    const char *password_text = cJSON_IsString(password)
        ? esp_openclaw_node_trimmed_or_null(password->valuestring)
        : NULL;

    size_t credential_count = 0;
    credential_count += bootstrap_text != NULL ? 1U : 0U;
    credential_count += shared_text != NULL ? 1U : 0U;
    credential_count += password_text != NULL ? 1U : 0U;

    if (!cJSON_IsString(url) || url->valuestring == NULL ||
        !esp_openclaw_node_is_valid_gateway_uri(url->valuestring) ||
        credential_count != 1U) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    out_source->gateway_uri =
        esp_openclaw_node_duplicate_string(
            esp_openclaw_node_trimmed_or_null(url->valuestring));
    if (bootstrap_text != NULL) {
        out_source->kind = ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_BOOTSTRAP_TOKEN;
        out_source->secret = esp_openclaw_node_duplicate_string(bootstrap_text);
    } else if (shared_text != NULL) {
        out_source->kind = ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SHARED_TOKEN;
        out_source->secret = esp_openclaw_node_duplicate_string(shared_text);
    } else {
        out_source->kind = ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_PASSWORD;
        out_source->secret = esp_openclaw_node_duplicate_string(password_text);
    }

    cJSON_Delete(root);
    if (out_source->gateway_uri == NULL || out_source->secret == NULL) {
        esp_openclaw_node_clear_connect_source_struct(out_source);
        return ESP_ERR_NO_MEM;
    }

    return validate_connect_source(out_source);
}

static esp_err_t duplicate_explicit_connect_source(
    esp_openclaw_node_connect_source_kind_t kind,
    const char *gateway_uri,
    const char *secret,
    esp_openclaw_node_connect_request_source_t *out_source)
{
    if (out_source == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_source, 0, sizeof(*out_source));
    const char *trimmed_gateway_uri = esp_openclaw_node_trimmed_or_null(gateway_uri);
    const char *trimmed_secret = esp_openclaw_node_trimmed_or_null(secret);
    if (trimmed_gateway_uri == NULL ||
        (secret != NULL && trimmed_secret == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    out_source->kind = kind;
    out_source->gateway_uri = esp_openclaw_node_duplicate_string(trimmed_gateway_uri);
    if (secret != NULL) {
        out_source->secret = esp_openclaw_node_duplicate_string(trimmed_secret);
    }
    if (out_source->gateway_uri == NULL ||
        (secret != NULL && out_source->secret == NULL)) {
        esp_openclaw_node_clear_connect_source_struct(out_source);
        return ESP_ERR_NO_MEM;
    }

    return validate_connect_source(out_source);
}

esp_err_t esp_openclaw_node_build_connect_source_from_request(
    const esp_openclaw_node_connect_request_t *request,
    esp_openclaw_node_connect_request_source_t *out_source)
{
    if (request == NULL || out_source == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (request->source) {
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_SAVED_SESSION:
        if (request->gateway_uri != NULL || request->value != NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        memset(out_source, 0, sizeof(*out_source));
        out_source->kind = ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SAVED_SESSION;
        return ESP_OK;
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_SETUP_CODE:
        if (request->gateway_uri != NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        return parse_setup_code(request->value, out_source);
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_DEVICE_TOKEN:
        return duplicate_explicit_connect_source(
            ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_DEVICE_TOKEN,
            request->gateway_uri,
            request->value,
            out_source);
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_GATEWAY_TOKEN:
        return duplicate_explicit_connect_source(
            ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SHARED_TOKEN,
            request->gateway_uri,
            request->value,
            out_source);
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_GATEWAY_PASSWORD:
        return duplicate_explicit_connect_source(
            ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_PASSWORD,
            request->gateway_uri,
            request->value,
            out_source);
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_NO_AUTH:
        if (request->value != NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        return duplicate_explicit_connect_source(
            ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_NO_AUTH,
            request->gateway_uri,
            NULL,
            out_source);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

const char *esp_openclaw_node_connect_source_kind_name(
    esp_openclaw_node_connect_source_kind_t kind)
{
    switch (kind) {
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SHARED_TOKEN:
        return "shared-token";
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_PASSWORD:
        return "password";
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_BOOTSTRAP_TOKEN:
        return "bootstrap-token";
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_DEVICE_TOKEN:
        return "device-token";
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SAVED_SESSION:
        return "saved-device-token";
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_NO_AUTH:
        return "none";
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_NONE:
    default:
        return "none";
    }
}

void esp_openclaw_node_free_connect_material(esp_openclaw_node_connect_material_t *material)
{
    if (material == NULL) {
        return;
    }
    free(material->auth_value);
    memset(material, 0, sizeof(*material));
}

static esp_err_t validate_saved_session_connect_preflight_locked(
    esp_openclaw_node_handle_t node)
{
    /* NOT_FOUND (not INVALID_STATE) so callers can tell "no session material
     * exists, re-pair" apart from "client is busy, retry later". */
    if (!esp_openclaw_node_saved_session_is_present_locked(node)) {
        return ESP_ERR_NOT_FOUND;
    }
    return esp_openclaw_node_validate_tls_preflight(
        &node->config,
        node->persisted_session.gateway_uri);
}

static esp_err_t validate_explicit_connect_preflight_locked(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_connect_request_source_t *source)
{
    ESP_RETURN_ON_ERROR(
        validate_connect_source(source),
        ESP_OPENCLAW_NODE_TAG,
        "invalid connect source");
    if (source->kind == ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SAVED_SESSION) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_openclaw_node_validate_tls_preflight(
        &node->config,
        source->gateway_uri);
}

esp_err_t esp_openclaw_node_resolve_active_connect_material_locked(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_connect_material_t *material)
{
    memset(material, 0, sizeof(*material));
    material->kind = node->active_connect_source.kind;

    switch (node->active_connect_source.kind) {
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_BOOTSTRAP_TOKEN:
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_DEVICE_TOKEN:
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SHARED_TOKEN:
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_NO_AUTH:
        if (node->active_connect_source.secret != NULL) {
            material->auth_value =
                esp_openclaw_node_duplicate_string(
                    esp_openclaw_node_trimmed_or_null(
                        node->active_connect_source.secret));
            if (material->auth_value == NULL) {
                esp_openclaw_node_free_connect_material(material);
                return ESP_ERR_NO_MEM;
            }
            material->signature_token = material->auth_value;
        }
        return ESP_OK;
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_PASSWORD:
        if (node->active_connect_source.secret != NULL) {
            material->auth_value =
                esp_openclaw_node_duplicate_string(
                    esp_openclaw_node_trimmed_or_null(
                        node->active_connect_source.secret));
            if (material->auth_value == NULL) {
                esp_openclaw_node_free_connect_material(material);
                return ESP_ERR_NO_MEM;
            }
        }
        material->signature_token = NULL;
        return ESP_OK;
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SAVED_SESSION: {
        const char *device_token =
            esp_openclaw_node_trimmed_or_null(
                node->persisted_session.device_token);
        if (device_token == NULL) {
            esp_openclaw_node_free_connect_material(material);
            return ESP_ERR_INVALID_STATE;
        }
        material->auth_value = esp_openclaw_node_duplicate_string(device_token);
        if (material->auth_value == NULL) {
            esp_openclaw_node_free_connect_material(material);
            return ESP_ERR_NO_MEM;
        }
        material->signature_token = material->auth_value;
        return ESP_OK;
    }
    case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_NONE:
    default:
        esp_openclaw_node_free_connect_material(material);
        return ESP_ERR_INVALID_STATE;
    }
}

esp_err_t esp_openclaw_node_reserve_connect_request_locked(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_connect_request_source_t *source)
{
    if (node->state == ESP_OPENCLAW_NODE_INTERNAL_DESTROYING ||
        node->state == ESP_OPENCLAW_NODE_INTERNAL_CLOSED) {
        return ESP_ERR_INVALID_STATE;
    }

    /* The control slot is single-entry: a second request while one is queued
     * would race for it and drop silently. */
    if (node->pending_control != ESP_OPENCLAW_NODE_PENDING_CONTROL_REQUEST_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Saved-session requests come from automatic reconnect loops and must
     * never displace anything: idle-only. Explicit sources are operator
     * decisions and also reserve from CONNECTING/READY — the worker tears the
     * old attempt or session down (CANCELED/REQUESTED terminal event) before
     * starting the new one. Without this preemption a device retrying a dead
     * gateway can never be re-provisioned from the console. */
    if (source->kind == ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SAVED_SESSION &&
        node->state != ESP_OPENCLAW_NODE_INTERNAL_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = source->kind == ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SAVED_SESSION
        ? validate_saved_session_connect_preflight_locked(node)
        : validate_explicit_connect_preflight_locked(node, source);
    if (err != ESP_OK) {
        return err;
    }

    esp_openclaw_node_set_pending_control_locked(
        node,
        ESP_OPENCLAW_NODE_PENDING_CONTROL_REQUEST_CONNECT);
    return ESP_OK;
}
