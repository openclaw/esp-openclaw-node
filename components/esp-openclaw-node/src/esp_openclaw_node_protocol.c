/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_openclaw_node_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

static bool websocket_send_json(esp_openclaw_node_handle_t node, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    if (json == NULL) {
        return false;
    }
    int written = node->websocket_client_ops->send_text(
        node->ws,
        json,
        (int)strlen(json),
        pdMS_TO_TICKS(5000));
    free(json);
    return written >= 0;
}

static void invoke_gateway_callback(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_gateway_request_cb_t callback,
    void *user_ctx,
    bool ok,
    const char *payload_json,
    const char *error_code,
    const char *error_message)
{
    if (callback == NULL) {
        return;
    }
    const esp_openclaw_node_gateway_result_t result = {
        .ok = ok,
        .payload_json = payload_json,
        .error_code = error_code,
        .error_message = error_message,
    };
    callback(node, &result, user_ctx);
}

void esp_openclaw_node_send_gateway_request(
    esp_openclaw_node_handle_t node,
    const char *method,
    const char *params_json,
    esp_openclaw_node_gateway_request_cb_t callback,
    void *user_ctx)
{
    size_t slot_index = ESP_OPENCLAW_NODE_MAX_PENDING_REQUESTS;
    char request_id[40] = {0};

    esp_openclaw_node_lock_state(node);
    if (node->state == ESP_OPENCLAW_NODE_INTERNAL_READY && node->ws != NULL) {
        for (size_t i = 0; i < ESP_OPENCLAW_NODE_MAX_PENDING_REQUESTS; ++i) {
            if (!node->pending_requests[i].in_use) {
                slot_index = i;
                break;
            }
        }
        if (slot_index < ESP_OPENCLAW_NODE_MAX_PENDING_REQUESTS) {
            snprintf(
                request_id,
                sizeof(request_id),
                "rpc-%" PRIu64,
                ++node->next_request_id);
            esp_openclaw_node_pending_request_t *pending =
                &node->pending_requests[slot_index];
            pending->in_use = true;
            snprintf(pending->request_id, sizeof(pending->request_id), "%s", request_id);
            pending->callback = callback;
            pending->user_ctx = user_ctx;
        }
    }
    esp_openclaw_node_unlock_state(node);

    if (slot_index >= ESP_OPENCLAW_NODE_MAX_PENDING_REQUESTS) {
        invoke_gateway_callback(
            node,
            callback,
            user_ctx,
            false,
            NULL,
            "UNAVAILABLE",
            "Gateway session is not ready or request capacity is full");
        return;
    }

    cJSON *params = cJSON_Parse(params_json != NULL ? params_json : "{}");
    cJSON *root = cJSON_CreateObject();
    if (!cJSON_IsObject(params) || root == NULL) {
        cJSON_Delete(params);
        cJSON_Delete(root);
        esp_openclaw_node_lock_state(node);
        memset(&node->pending_requests[slot_index], 0, sizeof(node->pending_requests[slot_index]));
        esp_openclaw_node_unlock_state(node);
        invoke_gateway_callback(
            node,
            callback,
            user_ctx,
            false,
            NULL,
            "INVALID_REQUEST",
            "Gateway request parameters must be a JSON object");
        return;
    }

    cJSON_AddStringToObject(root, "type", "req");
    cJSON_AddStringToObject(root, "id", request_id);
    cJSON_AddStringToObject(root, "method", method);
    cJSON_AddItemToObject(root, "params", params);
    if (!websocket_send_json(node, root)) {
        esp_openclaw_node_lock_state(node);
        memset(&node->pending_requests[slot_index], 0, sizeof(node->pending_requests[slot_index]));
        esp_openclaw_node_unlock_state(node);
        invoke_gateway_callback(
            node,
            callback,
            user_ctx,
            false,
            NULL,
            "TRANSPORT_ERROR",
            "Failed to send Gateway request");
    }
    cJSON_Delete(root);
}

void esp_openclaw_node_fail_pending_requests(
    esp_openclaw_node_handle_t node,
    const char *code,
    const char *message)
{
    for (;;) {
        esp_openclaw_node_gateway_request_cb_t callback = NULL;
        void *user_ctx = NULL;

        esp_openclaw_node_lock_state(node);
        for (size_t i = 0; i < ESP_OPENCLAW_NODE_MAX_PENDING_REQUESTS; ++i) {
            if (node->pending_requests[i].in_use) {
                callback = node->pending_requests[i].callback;
                user_ctx = node->pending_requests[i].user_ctx;
                memset(&node->pending_requests[i], 0, sizeof(node->pending_requests[i]));
                break;
            }
        }
        esp_openclaw_node_unlock_state(node);

        if (callback == NULL) {
            return;
        }
        invoke_gateway_callback(node, callback, user_ctx, false, NULL, code, message);
    }
}

static bool send_connect_request(
    esp_openclaw_node_handle_t node,
    const char *nonce,
    int64_t signed_at_ms)
{
    esp_openclaw_node_connect_material_t material = {0};

    esp_openclaw_node_lock_state(node);
    esp_err_t selection_err =
        esp_openclaw_node_resolve_active_connect_material_locked(node, &material);
    esp_openclaw_node_unlock_state(node);
    if (selection_err != ESP_OK) {
        ESP_LOGE(
            ESP_OPENCLAW_NODE_TAG,
            "failed resolving connect material: %s",
            esp_err_to_name(selection_err));
        return false;
    }

    /*
     * The gateway rebuilds this payload from the DECLARED connect scopes, so
     * the signed CSV must match params.scopes exactly (same entries, same
     * registration order). An empty CSV only verifies for scope-less roles,
     * which is why node connects passed while operator connects failed.
     */
    char *scopes_csv = NULL;
    esp_openclaw_node_lock_state(node);
    size_t scopes_csv_len = 1;
    for (size_t i = 0; i < node->scope_count; ++i) {
        scopes_csv_len += strlen(node->scopes[i]) + 1;
    }
    scopes_csv = calloc(1, scopes_csv_len);
    if (scopes_csv != NULL) {
        for (size_t i = 0; i < node->scope_count; ++i) {
            if (i > 0) {
                strlcat(scopes_csv, ",", scopes_csv_len);
            }
            strlcat(scopes_csv, node->scopes[i], scopes_csv_len);
        }
    }
    esp_openclaw_node_unlock_state(node);
    if (scopes_csv == NULL) {
        esp_openclaw_node_free_connect_material(&material);
        ESP_LOGE(ESP_OPENCLAW_NODE_TAG, "failed building scopes payload");
        return false;
    }

    char *payload = NULL;
    esp_err_t err = esp_openclaw_node_identity_build_auth_payload_v3(
        &node->identity,
        node->config.client_id,
        node->config.client_mode,
        node->config.role,
        scopes_csv,
        signed_at_ms,
        material.signature_token,
        nonce,
        node->config.platform,
        node->config.device_family,
        &payload);
    free(scopes_csv);
    if (err != ESP_OK || payload == NULL) {
        esp_openclaw_node_free_connect_material(&material);
        ESP_LOGE(ESP_OPENCLAW_NODE_TAG, "failed building auth payload");
        return false;
    }

    char signature[ESP_OPENCLAW_NODE_SIGNATURE_B64_BUFFER_LEN] = {0};
    err = esp_openclaw_node_identity_sign_payload(
        &node->identity,
        payload,
        signature,
        sizeof(signature));
    free(payload);
    if (err != ESP_OK) {
        esp_openclaw_node_free_connect_material(&material);
        ESP_LOGE(ESP_OPENCLAW_NODE_TAG, "failed signing auth payload");
        return false;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "req");
    char request_id[32] = {0};
    snprintf(
        request_id,
        sizeof(request_id),
        "connect-%" PRIu64,
        (uint64_t)esp_timer_get_time());
    cJSON_AddStringToObject(root, "id", request_id);
    cJSON_AddStringToObject(root, "method", "connect");

    cJSON *params = cJSON_CreateObject();
    /*
     * maxProtocol 4 keeps the session on the current node protocol: legacy
     * (v3-only) sessions have plugin-owned caps like `canvas` stripped by the
     * gateway and never receive `pluginSurfaceUrls`. The node surface this
     * component uses (connect.challenge, hello-ok, node.invoke.*) is identical
     * across 3 and 4.
     */
    cJSON_AddNumberToObject(params, "minProtocol", 3);
    cJSON_AddNumberToObject(params, "maxProtocol", 4);

    cJSON *client = cJSON_CreateObject();
    cJSON_AddStringToObject(client, "id", node->config.client_id);
    cJSON_AddStringToObject(client, "displayName", node->config.display_name);
    cJSON_AddStringToObject(
        client,
        "version",
        esp_openclaw_node_firmware_version());
    cJSON_AddStringToObject(client, "platform", node->config.platform);
    cJSON_AddStringToObject(
        client,
        "deviceFamily",
        node->config.device_family);
    cJSON_AddStringToObject(
        client,
        "modelIdentifier",
        node->config.model_identifier);
    cJSON_AddStringToObject(client, "mode", node->config.client_mode);
    cJSON_AddItemToObject(params, "client", client);

    cJSON_AddStringToObject(params, "role", node->config.role);
    esp_openclaw_node_add_registered_string_array(
        params,
        "scopes",
        node->scopes,
        node->scope_count);
    esp_openclaw_node_add_registered_string_array(
        params,
        "caps",
        node->capabilities,
        node->capability_count);
    esp_openclaw_node_add_registered_command_array(params, "commands", node);

    if (material.auth_value != NULL) {
        cJSON *auth_json = cJSON_CreateObject();
        switch (material.kind) {
        case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_BOOTSTRAP_TOKEN:
            cJSON_AddStringToObject(
                auth_json,
                "bootstrapToken",
                material.auth_value);
            break;
        case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SHARED_TOKEN:
            cJSON_AddStringToObject(auth_json, "token", material.auth_value);
            break;
        case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_PASSWORD:
            cJSON_AddStringToObject(
                auth_json,
                "password",
                material.auth_value);
            break;
        case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_DEVICE_TOKEN:
        case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_SAVED_SESSION:
            cJSON_AddStringToObject(
                auth_json,
                "deviceToken",
                material.auth_value);
            break;
        case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_NO_AUTH:
        case ESP_OPENCLAW_NODE_CONNECT_SOURCE_KIND_NONE:
        default:
            break;
        }
        cJSON_AddItemToObject(params, "auth", auth_json);
    }

    char user_agent[64] = {0};
    snprintf(
        user_agent,
        sizeof(user_agent),
        "esp-openclaw-node/%s",
        esp_openclaw_node_firmware_version());
    cJSON_AddStringToObject(params, "userAgent", user_agent);
    cJSON_AddStringToObject(params, "locale", node->config.locale);

    cJSON *device = cJSON_CreateObject();
    cJSON_AddStringToObject(device, "id", node->identity.device_id);
    cJSON_AddStringToObject(
        device,
        "publicKey",
        node->identity.public_key_b64url);
    cJSON_AddStringToObject(device, "signature", signature);
    cJSON_AddNumberToObject(device, "signedAt", (double)signed_at_ms);
    cJSON_AddStringToObject(device, "nonce", nonce);
    cJSON_AddItemToObject(params, "device", device);
    cJSON_AddItemToObject(root, "params", params);

    bool ready_to_send = false;
    esp_openclaw_node_internal_state_t state = ESP_OPENCLAW_NODE_INTERNAL_IDLE;
    esp_openclaw_node_lock_state(node);
    state = node->state;
    ready_to_send = node->state == ESP_OPENCLAW_NODE_INTERNAL_CONNECTING &&
                    node->pending_connect_id[0] == '\0';
    if (ready_to_send) {
        snprintf(
            node->pending_connect_id,
            sizeof(node->pending_connect_id),
            "%s",
            request_id);
    }
    esp_openclaw_node_unlock_state(node);
    if (!ready_to_send) {
        cJSON_Delete(root);
        esp_openclaw_node_free_connect_material(&material);
        ESP_LOGW(
            ESP_OPENCLAW_NODE_TAG,
            "connect request ignored in state=%s",
            esp_openclaw_node_internal_state_name(state));
        return false;
    }

    bool ok = websocket_send_json(node, root);
    cJSON_Delete(root);
    if (!ok) {
        esp_openclaw_node_lock_state(node);
        node->pending_connect_id[0] = '\0';
        esp_openclaw_node_unlock_state(node);
        esp_openclaw_node_free_connect_material(&material);
        return false;
    }

    ESP_LOGI(
        ESP_OPENCLAW_NODE_TAG,
        "sent connect request using %s auth",
        esp_openclaw_node_connect_source_kind_name(material.kind));
    esp_openclaw_node_free_connect_material(&material);
    return true;
}

static void send_invoke_result(
    esp_openclaw_node_handle_t node,
    const char *request_id,
    const char *node_id,
    bool ok,
    const char *payload_json,
    const char *error_code,
    const char *error_message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "req");

    char ws_request_id[32] = {0};
    snprintf(
        ws_request_id,
        sizeof(ws_request_id),
        "esp32-%" PRIu64,
        (uint64_t)esp_timer_get_time());
    cJSON_AddStringToObject(root, "id", ws_request_id);
    cJSON_AddStringToObject(root, "method", "node.invoke.result");

    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "id", request_id);
    cJSON_AddStringToObject(params, "nodeId", node_id);
    cJSON_AddBoolToObject(params, "ok", ok);

    if (ok) {
        if (payload_json != NULL && payload_json[0] != '\0') {
            cJSON_AddStringToObject(params, "payloadJSON", payload_json);
        }
    } else {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(
            error,
            "code",
            error_code ? error_code : "INVALID_REQUEST");
        cJSON_AddStringToObject(
            error,
            "message",
            error_message ? error_message : "command failed");
        cJSON_AddItemToObject(params, "error", error);
    }

    cJSON_AddItemToObject(root, "params", params);
    if (!websocket_send_json(node, root)) {
        ESP_LOGW(ESP_OPENCLAW_NODE_TAG, "failed sending invoke result");
    }
    cJSON_Delete(root);
}

static esp_err_t build_connect_response_session_update(
    esp_openclaw_node_handle_t node,
    const char *device_token_text,
    esp_openclaw_node_persisted_session_t *update)
{
    if (node == NULL || device_token_text == NULL || update == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(update, 0, sizeof(*update));
    update->version = 1;
    update->gateway_uri = esp_openclaw_node_duplicate_string(
        esp_openclaw_node_trimmed_or_null(node->transport_gateway_uri));
    update->device_token =
        esp_openclaw_node_duplicate_string(device_token_text);
    if (update->gateway_uri == NULL || update->device_token == NULL) {
        esp_openclaw_node_persisted_session_free(update);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t persist_handoff_device_tokens(
    esp_openclaw_node_handle_t node,
    cJSON *auth)
{
    cJSON *device_tokens = cJSON_IsObject(auth)
        ? cJSON_GetObjectItemCaseSensitive(auth, "deviceTokens")
        : NULL;
    if (!cJSON_IsArray(device_tokens)) {
        return ESP_OK;
    }

    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, device_tokens) {
        cJSON *role = cJSON_GetObjectItemCaseSensitive(entry, "role");
        cJSON *device_token = cJSON_GetObjectItemCaseSensitive(entry, "deviceToken");
        const char *role_text = cJSON_IsString(role)
            ? esp_openclaw_node_trimmed_or_null(role->valuestring)
            : NULL;
        const char *token_text = cJSON_IsString(device_token)
            ? esp_openclaw_node_trimmed_or_null(device_token->valuestring)
            : NULL;
        if (role_text == NULL || token_text == NULL ||
            strcmp(role_text, node->config.role) == 0) {
            continue;
        }
        if (strcmp(role_text, "node") != 0 && strcmp(role_text, "operator") != 0) {
            continue;
        }

        esp_openclaw_node_persisted_session_t stored = {0};
        ESP_RETURN_ON_ERROR(
            esp_openclaw_node_persisted_session_load(role_text, &stored),
            ESP_OPENCLAW_NODE_TAG,
            "load handed-off role session");
        esp_openclaw_node_persisted_session_t update = {
            .version = 1,
            .gateway_uri = esp_openclaw_node_duplicate_string(
                esp_openclaw_node_trimmed_or_null(node->transport_gateway_uri)),
            .device_token = esp_openclaw_node_duplicate_string(token_text),
        };
        if (update.gateway_uri == NULL || update.device_token == NULL) {
            esp_openclaw_node_persisted_session_free(&stored);
            esp_openclaw_node_persisted_session_free(&update);
            return ESP_ERR_NO_MEM;
        }
        esp_err_t err = esp_openclaw_node_persisted_session_store(
            role_text,
            &stored,
            &update);
        esp_openclaw_node_persisted_session_free(&stored);
        esp_openclaw_node_persisted_session_free(&update);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static connect_response_finalize_result_t finalize_connect_response_success(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_persisted_session_t *update,
    char *plugin_surface_urls_json)
{
    connect_response_finalize_result_t result = {
        .outcome = CONNECT_RESPONSE_OUTCOME_IGNORE,
        .err = ESP_OK,
        .state = ESP_OPENCLAW_NODE_INTERNAL_IDLE,
    };

    esp_openclaw_node_lock_state(node);
    result.state = node->state;
    if (node->state != ESP_OPENCLAW_NODE_INTERNAL_CONNECTING ||
        node->pending_connect_id[0] == '\0') {
        esp_openclaw_node_unlock_state(node);
        return result;
    }

    result.err = esp_openclaw_node_persisted_session_store(
        node->config.role,
        &node->persisted_session,
        update);
    if (result.err != ESP_OK) {
        result.outcome = CONNECT_RESPONSE_OUTCOME_CONNECT_FAILED;
    } else {
        free(node->plugin_surface_urls_json);
        node->plugin_surface_urls_json = plugin_surface_urls_json;
        node->state = ESP_OPENCLAW_NODE_INTERNAL_READY;
        /* Do NOT clear pending_control here: this attempt's CONNECT
         * reservation was already consumed when the transport started, so
         * anything present now is a queued disconnect/preempt that must still
         * run against the ready session — clearing it loses that request. */
        esp_openclaw_node_clear_session_wait_state_locked(node);
        esp_openclaw_node_clear_connect_source_struct(&node->active_connect_source);
        result.outcome = CONNECT_RESPONSE_OUTCOME_CONNECTED;
    }
    esp_openclaw_node_unlock_state(node);
    return result;
}

static void complete_connect_response_outcome(
    esp_openclaw_node_handle_t node,
    const connect_response_finalize_result_t *result)
{
    switch (result->outcome) {
    case CONNECT_RESPONSE_OUTCOME_IGNORE:
        ESP_LOGW(
            ESP_OPENCLAW_NODE_TAG,
            "ignoring connect response in state=%s",
            esp_openclaw_node_internal_state_name(result->state));
        break;
    case CONNECT_RESPONSE_OUTCOME_CONNECT_FAILED:
        esp_openclaw_node_complete_connect_failed(
            node,
            ESP_OPENCLAW_NODE_CONNECT_FAILURE_SESSION_FINALIZATION_FAILED,
            result->err,
            NULL,
            true);
        break;
    case CONNECT_RESPONSE_OUTCOME_CONNECTED:
        esp_openclaw_node_emit_connected(node);
        ESP_LOGI(
            ESP_OPENCLAW_NODE_TAG,
            "OpenClaw gateway handshake complete");
        break;
    default:
        break;
    }
}

static void handle_connect_response(
    esp_openclaw_node_handle_t node,
    cJSON *root)
{
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (!cJSON_IsBool(ok)) {
        return;
    }

    if (cJSON_IsTrue(ok)) {
        cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
        cJSON *type =
            payload ? cJSON_GetObjectItemCaseSensitive(payload, "type") : NULL;
        if (!cJSON_IsString(type) ||
            strcmp(type->valuestring, "hello-ok") != 0) {
            return;
        }

        cJSON *auth = cJSON_GetObjectItemCaseSensitive(payload, "auth");
        cJSON *plugin_surface_urls =
            cJSON_GetObjectItemCaseSensitive(payload, "pluginSurfaceUrls");
        char *plugin_surface_urls_json = cJSON_IsObject(plugin_surface_urls)
            ? cJSON_PrintUnformatted(plugin_surface_urls)
            : NULL;
        if (cJSON_IsObject(plugin_surface_urls) && plugin_surface_urls_json == NULL) {
            esp_openclaw_node_complete_connect_failed(
                node,
                ESP_OPENCLAW_NODE_CONNECT_FAILURE_SESSION_FINALIZATION_FAILED,
                ESP_ERR_NO_MEM,
                NULL,
                true);
            return;
        }
        cJSON *device_token = auth
            ? cJSON_GetObjectItemCaseSensitive(auth, "deviceToken")
            : NULL;
        const char *device_token_text = cJSON_IsString(device_token)
            ? esp_openclaw_node_trimmed_or_null(device_token->valuestring)
            : NULL;
        if (device_token_text == NULL) {
            free(plugin_surface_urls_json);
            esp_openclaw_node_complete_connect_failed(
                node,
                ESP_OPENCLAW_NODE_CONNECT_FAILURE_SESSION_FINALIZATION_FAILED,
                ESP_FAIL,
                NULL,
                true);
            return;
        }

        esp_err_t handoff_err = persist_handoff_device_tokens(node, auth);
        if (handoff_err != ESP_OK) {
            free(plugin_surface_urls_json);
            esp_openclaw_node_complete_connect_failed(
                node,
                ESP_OPENCLAW_NODE_CONNECT_FAILURE_SESSION_FINALIZATION_FAILED,
                handoff_err,
                NULL,
                true);
            return;
        }

        esp_openclaw_node_persisted_session_t update = {0};
        esp_err_t err = build_connect_response_session_update(
            node,
            device_token_text,
            &update);
        if (err != ESP_OK) {
            free(plugin_surface_urls_json);
            esp_openclaw_node_complete_connect_failed(
                node,
                ESP_OPENCLAW_NODE_CONNECT_FAILURE_SESSION_FINALIZATION_FAILED,
                err,
                NULL,
                true);
            return;
        }

        connect_response_finalize_result_t result =
            finalize_connect_response_success(
                node,
                &update,
                plugin_surface_urls_json);
        if (result.outcome != CONNECT_RESPONSE_OUTCOME_CONNECTED) {
            free(plugin_surface_urls_json);
        }
        esp_openclaw_node_persisted_session_free(&update);
        complete_connect_response_outcome(node, &result);
        return;
    }

    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    cJSON *message =
        error ? cJSON_GetObjectItemCaseSensitive(error, "message") : NULL;
    cJSON *details =
        error ? cJSON_GetObjectItemCaseSensitive(error, "details") : NULL;
    cJSON *detail_code =
        details ? cJSON_GetObjectItemCaseSensitive(details, "code") : NULL;
    cJSON *request_id =
        details ? cJSON_GetObjectItemCaseSensitive(details, "requestId") : NULL;
    const char *message_text = cJSON_IsString(message) &&
                               message->valuestring != NULL
        ? message->valuestring
        : "connect failed";
    const char *detail_code_text = cJSON_IsString(detail_code) &&
                                   detail_code->valuestring != NULL
        ? detail_code->valuestring
        : NULL;
    const char *request_id_text = cJSON_IsString(request_id) &&
                                  request_id->valuestring != NULL
        ? request_id->valuestring
        : NULL;

    ESP_LOGW(
        ESP_OPENCLAW_NODE_TAG,
        "connect rejected: %s%s%s%s%s%s",
        message_text,
        detail_code_text != NULL ? " (" : "",
        detail_code_text != NULL ? detail_code_text : "",
        detail_code_text != NULL ? ")" : "",
        request_id_text != NULL ? ", requestId=" : "",
        request_id_text != NULL ? request_id_text : "");

    esp_openclaw_node_complete_connect_failed(
        node,
        ESP_OPENCLAW_NODE_CONNECT_FAILURE_AUTH_REJECTED,
        ESP_OK,
        detail_code_text,
        true);
}

static void handle_connect_challenge(
    esp_openclaw_node_handle_t node,
    cJSON *payload)
{
    esp_openclaw_node_lock_state(node);
    bool accept_challenge =
        node->state == ESP_OPENCLAW_NODE_INTERNAL_CONNECTING &&
        node->pending_connect_id[0] == '\0';
    esp_openclaw_node_unlock_state(node);
    if (!accept_challenge) {
        ESP_LOGW(ESP_OPENCLAW_NODE_TAG, "ignoring unexpected connect.challenge");
        return;
    }

    cJSON *nonce = cJSON_GetObjectItemCaseSensitive(payload, "nonce");
    cJSON *ts = cJSON_GetObjectItemCaseSensitive(payload, "ts");
    if (!cJSON_IsString(nonce) || nonce->valuestring == NULL ||
        nonce->valuestring[0] == '\0') {
        ESP_LOGW(ESP_OPENCLAW_NODE_TAG, "connect.challenge missing nonce");
        return;
    }
    int64_t signed_at_ms = cJSON_IsNumber(ts) ? (int64_t)ts->valuedouble : 0;
    if (signed_at_ms <= 0) {
        signed_at_ms = esp_timer_get_time() / 1000LL;
    }
    if (!send_connect_request(node, nonce->valuestring, signed_at_ms)) {
        esp_openclaw_node_complete_connect_failed(
            node,
            ESP_OPENCLAW_NODE_CONNECT_FAILURE_TRANSPORT_START_FAILED,
            ESP_FAIL,
            NULL,
            true);
    }
}

static void handle_invoke_request(
    esp_openclaw_node_handle_t node,
    cJSON *payload)
{
    cJSON *id = cJSON_GetObjectItemCaseSensitive(payload, "id");
    cJSON *node_id = cJSON_GetObjectItemCaseSensitive(payload, "nodeId");
    cJSON *command = cJSON_GetObjectItemCaseSensitive(payload, "command");
    cJSON *params_json =
        cJSON_GetObjectItemCaseSensitive(payload, "paramsJSON");
    cJSON *session_key =
        cJSON_GetObjectItemCaseSensitive(payload, "sessionKey");

    if (!cJSON_IsString(id) || !cJSON_IsString(node_id) ||
        !cJSON_IsString(command) || id->valuestring == NULL ||
        node_id->valuestring == NULL || command->valuestring == NULL) {
        ESP_LOGW(
            ESP_OPENCLAW_NODE_TAG,
            "dropping malformed node.invoke.request");
        return;
    }

    bool session_key_valid = session_key == NULL;
    if (cJSON_IsString(session_key) && session_key->valuestring != NULL) {
        size_t session_key_len = strlen(session_key->valuestring);
        session_key_valid = session_key_len > 0 &&
            session_key_len <= ESP_OPENCLAW_NODE_MAX_SESSION_KEY_LEN;
        for (size_t i = 0; session_key_valid && i < session_key_len; ++i) {
            unsigned char ch = (unsigned char)session_key->valuestring[i];
            session_key_valid = ch >= 0x21 && ch <= 0x7e;
        }
    }
    if (!session_key_valid) {
        send_invoke_result(
            node,
            id->valuestring,
            node_id->valuestring,
            false,
            NULL,
            "INVALID_REQUEST",
            "sessionKey must be 1..256 printable ASCII bytes");
        return;
    }

    const char *effective_params_json = "{}";
    size_t effective_params_len = 2;
    if (cJSON_IsString(params_json) && params_json->valuestring != NULL &&
        params_json->valuestring[0] != '\0') {
        effective_params_json = params_json->valuestring;
        effective_params_len = strlen(effective_params_json);
    }

    char *result_json = NULL;
    const char *error_code = NULL;
    const char *error_message = NULL;
    const esp_openclaw_node_command_invocation_t invocation = {
        .session_key = session_key != NULL ? session_key->valuestring : NULL,
    };
    esp_err_t err = esp_openclaw_node_dispatch_command(
        node,
        command->valuestring,
        effective_params_json,
        effective_params_len,
        &invocation,
        &result_json,
        &error_code,
        &error_message);
    if (err == ESP_OK) {
        send_invoke_result(
            node,
            id->valuestring,
            node_id->valuestring,
            true,
            result_json,
            NULL,
            NULL);
    } else {
        send_invoke_result(
            node,
            id->valuestring,
            node_id->valuestring,
            false,
            NULL,
            error_code,
            error_message);
    }

    free(result_json);
}

static bool handle_gateway_response(
    esp_openclaw_node_handle_t node,
    const char *request_id,
    cJSON *root)
{
    esp_openclaw_node_gateway_request_cb_t callback = NULL;
    void *user_ctx = NULL;

    esp_openclaw_node_lock_state(node);
    for (size_t i = 0; i < ESP_OPENCLAW_NODE_MAX_PENDING_REQUESTS; ++i) {
        esp_openclaw_node_pending_request_t *pending = &node->pending_requests[i];
        if (pending->in_use && strcmp(pending->request_id, request_id) == 0) {
            callback = pending->callback;
            user_ctx = pending->user_ctx;
            memset(pending, 0, sizeof(*pending));
            break;
        }
    }
    esp_openclaw_node_unlock_state(node);
    if (callback == NULL) {
        return false;
    }

    cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    cJSON *error_code = cJSON_IsObject(error)
        ? cJSON_GetObjectItemCaseSensitive(error, "code")
        : NULL;
    cJSON *error_message = cJSON_IsObject(error)
        ? cJSON_GetObjectItemCaseSensitive(error, "message")
        : NULL;
    char *payload_json = payload != NULL ? cJSON_PrintUnformatted(payload) : NULL;
    invoke_gateway_callback(
        node,
        callback,
        user_ctx,
        cJSON_IsTrue(ok),
        payload_json,
        cJSON_IsString(error_code) ? error_code->valuestring : NULL,
        cJSON_IsString(error_message) ? error_message->valuestring : NULL);
    free(payload_json);
    return true;
}

static void emit_gateway_event(
    esp_openclaw_node_handle_t node,
    const char *event,
    cJSON *payload)
{
    esp_openclaw_node_gateway_event_cb_t callback = NULL;
    void *user_ctx = NULL;
    esp_openclaw_node_lock_state(node);
    bool ready = node->state == ESP_OPENCLAW_NODE_INTERNAL_READY;
    callback = node->config.gateway_event_cb;
    user_ctx = node->config.gateway_event_user_ctx;
    esp_openclaw_node_unlock_state(node);
    if (!ready || callback == NULL) {
        return;
    }

    char *payload_json = payload != NULL ? cJSON_PrintUnformatted(payload) : strdup("null");
    if (payload_json == NULL) {
        return;
    }
    callback(node, event, payload_json, user_ctx);
    free(payload_json);
}

void esp_openclaw_node_process_gateway_message(
    esp_openclaw_node_handle_t node,
    const char *text)
{
    cJSON *root = cJSON_Parse(text);
    if (root == NULL) {
        ESP_LOGW(ESP_OPENCLAW_NODE_TAG, "invalid gateway JSON frame");
        return;
    }

    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || type->valuestring == NULL) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "event") == 0) {
        cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
        cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
        if (cJSON_IsString(event) && event->valuestring != NULL) {
            if (strcmp(event->valuestring, "connect.challenge") == 0 &&
                cJSON_IsObject(payload)) {
                ESP_LOGI(ESP_OPENCLAW_NODE_TAG, "received connect.challenge");
                handle_connect_challenge(node, payload);
            } else if (
                strcmp(event->valuestring, "node.invoke.request") == 0 &&
                cJSON_IsObject(payload)) {
                esp_openclaw_node_lock_state(node);
                bool ready = node->state == ESP_OPENCLAW_NODE_INTERNAL_READY;
                esp_openclaw_node_unlock_state(node);
                if (ready) {
                    handle_invoke_request(node, payload);
                } else {
                    ESP_LOGW(
                        ESP_OPENCLAW_NODE_TAG,
                        "ignoring node.invoke.request before session is ready");
                }
            } else {
                esp_openclaw_node_lock_state(node);
                bool connecting =
                    esp_openclaw_node_state_is_connecting(node->state);
                esp_openclaw_node_unlock_state(node);
                if (connecting) {
                    ESP_LOGD(
                        ESP_OPENCLAW_NODE_TAG,
                        "received gateway event during connect: %s",
                        event->valuestring);
                } else {
                    emit_gateway_event(node, event->valuestring, payload);
                }
            }
        }
    } else if (strcmp(type->valuestring, "res") == 0) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
        bool is_pending_connect_response = false;
        if (cJSON_IsString(id) && id->valuestring != NULL) {
            esp_openclaw_node_lock_state(node);
            is_pending_connect_response =
                node->transport_connected &&
                node->state == ESP_OPENCLAW_NODE_INTERNAL_CONNECTING &&
                node->pending_connect_id[0] != '\0' &&
                strcmp(id->valuestring, node->pending_connect_id) == 0;
            esp_openclaw_node_unlock_state(node);
        }
        if (is_pending_connect_response) {
            handle_connect_response(node, root);
        } else if (cJSON_IsString(id) && id->valuestring != NULL) {
            (void)handle_gateway_response(node, id->valuestring, root);
        }
    }

    cJSON_Delete(root);
}
