/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_openclaw_talk.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#define TAG "esp_openclaw_talk"
#define MAX_SDP_RESPONSE_BYTES (256U * 1024U)
#define MAX_VOICE_SESSION_ID_BYTES 128U
#define GATEWAY_CONTROL_CAPABILITY "gateway-control-v1"

typedef struct {
    char *name;
    char *value;
} offer_header_t;

typedef struct {
    esp_peer_signaling_cfg_t signaling;
    esp_openclaw_node_handle_t operator_node;
    char *gateway_http_base_url;
    char *session_key;
    char *provider;
    char *model;
    char *voice;
    uint16_t silence_duration_ms;
    esp_openclaw_talk_setup_failed_cb_t setup_failed_cb;
    void *setup_failed_ctx;
    char *offer_url;
    char *client_secret;
    char *voice_session_id;
    offer_header_t *offer_headers;
    size_t offer_header_count;
    portMUX_TYPE state_lock;
    size_t refs;
    bool stopped;
    bool close_notified;
    bool gateway_owned;
} talk_signaling_t;

typedef struct {
    char *data;
    size_t len;
    bool failed;
} http_response_t;

static char *duplicate_optional(const char *value)
{
    return value != NULL && value[0] != '\0' ? strdup(value) : NULL;
}

/* Match the Gateway's String.trim whitespace without changing UTF-8 key bytes. */
static size_t routing_space_bytes(const unsigned char *value, size_t length)
{
    if (value[0] == ' ' || (value[0] >= '\t' && value[0] <= '\r')) return 1;
    if (length >= 2 && value[0] == 0xc2 && value[1] == 0xa0) return 2;
    if (length < 3) return 0;
    bool space = (value[0] == 0xe1 && value[1] == 0x9a && value[2] == 0x80) ||
        (value[0] == 0xe2 && value[1] == 0x80 &&
         ((value[2] >= 0x80 && value[2] <= 0x8a) ||
          value[2] == 0xa8 || value[2] == 0xa9 || value[2] == 0xaf)) ||
        (value[0] == 0xe2 && value[1] == 0x81 && value[2] == 0x9f) ||
        (value[0] == 0xe3 && value[1] == 0x80 && value[2] == 0x80) ||
        (value[0] == 0xef && value[1] == 0xbb && value[2] == 0xbf);
    return space ? 3 : 0;
}

static bool normalize_routing_value(cJSON *field)
{
    unsigned char *value = (unsigned char *)field->valuestring;
    size_t length = strnlen((const char *)value, ESP_OPENCLAW_NODE_MAX_SESSION_KEY_LEN + 1U);
    if (length > ESP_OPENCLAW_NODE_MAX_SESSION_KEY_LEN) return false;
    size_t begin = 0;
    size_t width;
    while (begin < length && (width = routing_space_bytes(value + begin, length - begin)) != 0) {
        begin += width;
    }
    size_t end = begin;
    for (size_t i = begin; i < length;) {
        width = routing_space_bytes(value + i, length - i);
        if (width > 0) i += width;
        else end = ++i;
    }
    length = end - begin;
    memmove(value, value + begin, length);
    for (size_t i = 0; i < length; ++i) {
        if (value[i] >= 'A' && value[i] <= 'Z') value[i] += 'a' - 'A';
    }
    value[length] = '\0';
    return true;
}

static char *resolve_default_session_key(const char *payload_json)
{
    cJSON *payload = payload_json != NULL ? cJSON_Parse(payload_json) : NULL;
    cJSON *config = cJSON_GetObjectItemCaseSensitive(payload, "config");
    cJSON *settings = cJSON_GetObjectItemCaseSensitive(config, "talk");
    cJSON *session = cJSON_GetObjectItemCaseSensitive(config, "session");
    cJSON *owner = cJSON_GetObjectItemCaseSensitive(settings, "agentId");
    cJSON *main = cJSON_GetObjectItemCaseSensitive(session, "mainKey");
    char *key = NULL;
    bool valid = cJSON_IsObject(config) &&
        (settings == NULL || cJSON_IsObject(settings)) &&
        (session == NULL || cJSON_IsObject(session)) &&
        (owner == NULL || cJSON_IsString(owner)) &&
        (main == NULL || cJSON_IsString(main));
    /* This decoded snapshot is ours; validate both fields before selecting any
     * default, and retain only the final key after freeing the snapshot. */
    if (!valid || (owner != NULL && !normalize_routing_value(owner)) ||
        (main != NULL && !normalize_routing_value(main))) goto done;
    const char *agent_id = owner != NULL ? owner->valuestring : NULL;
    const char *main_key = main != NULL ? main->valuestring : NULL;
    if (agent_id == NULL || agent_id[0] == '\0') {
        /* Preserve the public default: the Gateway resolves a unique owner or
         * rejects ambiguity. Never select a roster entry or omit the target. */
        key = strdup("main");
        goto done;
    }
    /* A malformed reference must not be sanitized into a different owner. */
    size_t agent_length = strlen(agent_id);
    if (agent_length > 64U) goto done;
    for (size_t i = 0; i < agent_length; ++i) {
        char byte = agent_id[i];
        bool slug = (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
            (i > 0 && (byte == '_' || byte == '-'));
        if (!slug) goto done;
    }
    const char *selected_main = main_key != NULL && main_key[0] != '\0' ? main_key : "main";
    size_t key_length = strlen("agent:") + agent_length + 1U + strlen(selected_main);
    if (key_length > ESP_OPENCLAW_NODE_MAX_SESSION_KEY_LEN) goto done;
    key = malloc(key_length + 1U);
    if (key != NULL) snprintf(key, key_length + 1U, "agent:%s:%s", agent_id, selected_main);

done:
    cJSON_Delete(payload);
    return key;
}

static void free_offer_headers(talk_signaling_t *talk)
{
    for (size_t i = 0; i < talk->offer_header_count; ++i) {
        free(talk->offer_headers[i].name);
        free(talk->offer_headers[i].value);
    }
    free(talk->offer_headers);
}

static void free_talk_signaling(talk_signaling_t *talk)
{
    if (talk == NULL) {
        return;
    }
    free(talk->gateway_http_base_url);
    free(talk->session_key);
    free(talk->provider);
    free(talk->model);
    free(talk->voice);
    free(talk->offer_url);
    free(talk->client_secret);
    free(talk->voice_session_id);
    free_offer_headers(talk);
    free(talk);
}

static void release_talk_signaling(talk_signaling_t *talk)
{
    bool free_now = false;
    portENTER_CRITICAL(&talk->state_lock);
    if (talk->refs > 0) {
        --talk->refs;
        free_now = talk->refs == 0;
    }
    portEXIT_CRITICAL(&talk->state_lock);
    if (free_now) {
        free_talk_signaling(talk);
    }
}

static bool retain_if_running(talk_signaling_t *talk)
{
    bool running = false;
    portENTER_CRITICAL(&talk->state_lock);
    if (!talk->stopped) {
        ++talk->refs;
        running = true;
    }
    portEXIT_CRITICAL(&talk->state_lock);
    return running;
}

/* Reserve the callback's reference before enqueueing; stop may release the
 * handle meanwhile. Immediate submission failure releases the reservation. */
static esp_err_t request_talk_rpc(
    talk_signaling_t *talk,
    const char *method,
    const char *params_json,
    esp_openclaw_node_gateway_request_cb_t callback)
{
    if (!retain_if_running(talk)) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_openclaw_node_gateway_request(
        talk->operator_node, method, params_json, callback, talk);
    if (err != ESP_OK) release_talk_signaling(talk);
    return err;
}

static char *resolve_offer_url(const char *base_url, const char *offer_url)
{
    if (offer_url == NULL || offer_url[0] == '\0') {
        return NULL;
    }
    if (offer_url[0] != '/' || base_url == NULL || base_url[0] == '\0') {
        return NULL;
    }
    size_t base_len = strlen(base_url);
    while (base_len > 0 && base_url[base_len - 1] == '/') {
        --base_len;
    }
    size_t offer_len = strlen(offer_url);
    char *resolved = malloc(base_len + offer_len + 1U);
    if (resolved == NULL) {
        return NULL;
    }
    memcpy(resolved, base_url, base_len);
    memcpy(resolved + base_len, offer_url, offer_len + 1U);
    return resolved;
}

static bool copy_offer_headers(talk_signaling_t *talk, cJSON *headers)
{
    if (headers == NULL) {
        return true;
    }
    if (!cJSON_IsObject(headers)) {
        return false;
    }
    size_t count = 0;
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, headers) {
        if (!cJSON_IsString(entry) || entry->string == NULL ||
            entry->valuestring == NULL) {
            return false;
        }
        if (strcasecmp(entry->string, "authorization") == 0 ||
            strcasecmp(entry->string, "content-type") == 0 ||
            strcasecmp(entry->string, "host") == 0) {
            return false;
        }
        ++count;
    }
    if (count == 0) {
        return true;
    }
    talk->offer_headers = calloc(count, sizeof(*talk->offer_headers));
    if (talk->offer_headers == NULL) {
        return false;
    }
    cJSON_ArrayForEach(entry, headers) {
        offer_header_t *dst = &talk->offer_headers[talk->offer_header_count];
        dst->name = strdup(entry->string);
        dst->value = strdup(entry->valuestring);
        ++talk->offer_header_count;
        if (dst->name == NULL || dst->value == NULL) {
            return false;
        }
    }
    return true;
}

static void ignore_close_result(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_gateway_result_t *result,
    void *user_ctx);

static void close_voice_session(
    esp_openclaw_node_handle_t operator_node,
    const char *session_key,
    const char *voice_session_id)
{
    if (voice_session_id == NULL || voice_session_id[0] == '\0' ||
        strlen(voice_session_id) > MAX_VOICE_SESSION_ID_BYTES) {
        return;
    }
    cJSON *params = cJSON_CreateObject();
    if (params == NULL) {
        return;
    }
    cJSON_AddStringToObject(params, "sessionKey", session_key);
    cJSON_AddStringToObject(params, "voiceSessionId", voice_session_id);
    char *params_json = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (params_json != NULL) {
        (void)esp_openclaw_node_gateway_request(
            operator_node,
            "talk.client.close",
            params_json,
            ignore_close_result,
            NULL);
        free(params_json);
    }
}

static char *duplicate_voice_session_id(cJSON *value)
{
    if (!cJSON_IsString(value) || value->valuestring == NULL) {
        return NULL;
    }
    size_t len = strlen(value->valuestring);
    return len > 0 && len <= MAX_VOICE_SESSION_ID_BYTES
        ? strdup(value->valuestring)
        : NULL;
}

static bool has_gateway_control_descriptor(cJSON *payload)
{
    cJSON *control = cJSON_IsObject(payload)
        ? cJSON_GetObjectItemCaseSensitive(payload, "clientControl")
        : NULL;
    cJSON *owner = cJSON_IsObject(control) ? control->child : NULL;
    return owner != NULL && owner->next == NULL && owner->string != NULL &&
           strcmp(owner->string, "owner") == 0 && cJSON_IsString(owner) &&
           owner->valuestring != NULL && strcmp(owner->valuestring, "gateway") == 0;
}

static void signal_failed(
    talk_signaling_t *talk,
    esp_openclaw_talk_setup_result_t result,
    const char *message,
    const char *code)
{
    bool notify = false;
    portENTER_CRITICAL(&talk->state_lock);
    if (!talk->close_notified) {
        talk->close_notified = true;
        notify = true;
    }
    portEXIT_CRITICAL(&talk->state_lock);
    if (notify) {
        /* Error codes are bounded; provider/configuration payloads stay private. */
        ESP_LOGE(TAG, "%s%s%.32s", message, code != NULL ? ": " : "", code != NULL ? code : "");
        if (talk->setup_failed_cb != NULL) {
            talk->setup_failed_cb(result, talk->setup_failed_ctx);
        }
        if (talk->signaling.on_close != NULL) {
            talk->signaling.on_close(talk->signaling.ctx);
        }
    }
}

static void handle_talk_create(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_gateway_result_t *result,
    void *user_ctx)
{
    (void)node;
    talk_signaling_t *talk = user_ctx;
    cJSON *payload = result->ok && result->payload_json != NULL
        ? cJSON_Parse(result->payload_json)
        : NULL;
    cJSON *transport = cJSON_IsObject(payload)
        ? cJSON_GetObjectItemCaseSensitive(payload, "transport")
        : NULL;
    cJSON *offer_url = cJSON_IsObject(payload)
        ? cJSON_GetObjectItemCaseSensitive(payload, "offerUrl")
        : NULL;
    cJSON *client_secret = cJSON_IsObject(payload)
        ? cJSON_GetObjectItemCaseSensitive(payload, "clientSecret")
        : NULL;
    cJSON *voice_session_id = cJSON_IsObject(payload)
        ? cJSON_GetObjectItemCaseSensitive(payload, "voiceSessionId")
        : NULL;
    cJSON *offer_headers = cJSON_IsObject(payload)
        ? cJSON_GetObjectItemCaseSensitive(payload, "offerHeaders")
        : NULL;
    bool gateway_control = has_gateway_control_descriptor(payload);
    char *session_id = duplicate_voice_session_id(voice_session_id);
    bool valid = gateway_control && cJSON_IsString(transport) &&
                 strcmp(transport->valuestring, "webrtc") == 0 &&
                 cJSON_IsString(offer_url) &&
                 offer_url->valuestring[0] == '/' &&
                 cJSON_IsString(client_secret) &&
                 session_id != NULL;
    if (valid) {
        talk->offer_url = resolve_offer_url(
            talk->gateway_http_base_url,
            offer_url->valuestring);
        talk->client_secret = duplicate_optional(client_secret->valuestring);
        talk->voice_session_id = session_id;
        session_id = NULL;
        valid = talk->offer_url != NULL && talk->client_secret != NULL &&
                copy_offer_headers(talk, offer_headers);
    }
    cJSON_Delete(payload);
    if (!valid) {
        close_voice_session(
            talk->operator_node,
            talk->session_key,
            session_id != NULL ? session_id : talk->voice_session_id);
        free(session_id);
        bool upgrade_required = result->ok && !gateway_control;
        if (!result->ok && result->error_code != NULL) {
            upgrade_required = strcmp(result->error_code, "NOT_SUPPORTED") == 0;
        }
        signal_failed(
            talk,
            upgrade_required
                ? ESP_OPENCLAW_TALK_GATEWAY_UPGRADE_REQUIRED
                : ESP_OPENCLAW_TALK_SETUP_FAILED,
            upgrade_required ? "Gateway upgrade required" : "Talk setup failed",
            result->ok ? NULL : result->error_code);
        release_talk_signaling(talk);
        return;
    }

    portENTER_CRITICAL(&talk->state_lock);
    bool stopped = talk->stopped;
    if (!stopped) {
        talk->gateway_owned = true;
    }
    portEXIT_CRITICAL(&talk->state_lock);
    if (stopped) {
        close_voice_session(talk->operator_node, talk->session_key, talk->voice_session_id);
        release_talk_signaling(talk);
        return;
    }

    esp_peer_signaling_ice_info_t ice_info = {.is_initiator = true};
    if (talk->signaling.on_ice_info != NULL) {
        talk->signaling.on_ice_info(&ice_info, talk->signaling.ctx);
    }
    if (talk->signaling.on_connected != NULL) {
        talk->signaling.on_connected(talk->signaling.ctx);
    }
    release_talk_signaling(talk);
}

static esp_err_t request_talk_create(talk_signaling_t *talk)
{
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "mode", "realtime");
    cJSON_AddStringToObject(params, "transport", "webrtc");
    cJSON_AddStringToObject(params, "brain", "agent-consult");
    cJSON_AddStringToObject(params, "sessionKey", talk->session_key);
    if (talk->provider != NULL) {
        cJSON_AddStringToObject(params, "provider", talk->provider);
    }
    if (talk->model != NULL) {
        cJSON_AddStringToObject(params, "model", talk->model);
    }
    if (talk->voice != NULL) {
        cJSON_AddStringToObject(params, "voice", talk->voice);
    }
    if (talk->silence_duration_ms > 0) {
        cJSON_AddNumberToObject(
            params,
            "silenceDurationMs",
            talk->silence_duration_ms);
    }
    cJSON *capabilities = cJSON_AddArrayToObject(params, "capabilities");
    cJSON *gateway_control = cJSON_CreateString(GATEWAY_CONTROL_CAPABILITY);
    if (capabilities == NULL || gateway_control == NULL ||
        !cJSON_AddItemToArray(capabilities, gateway_control)) {
        cJSON_Delete(gateway_control);
        cJSON_Delete(params);
        return ESP_ERR_NO_MEM;
    }
    char *params_json = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (params_json == NULL) return ESP_ERR_NO_MEM;
    esp_err_t err = request_talk_rpc(talk, "talk.client.create", params_json, handle_talk_create);
    free(params_json);
    return err;
}

static void handle_talk_config(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_gateway_result_t *result,
    void *user_ctx)
{
    (void)node;
    talk_signaling_t *talk = user_ctx;
    portENTER_CRITICAL(&talk->state_lock);
    bool stopped = talk->stopped;
    portEXIT_CRITICAL(&talk->state_lock);
    if (stopped) goto done;
    if (!result->ok) {
        signal_failed(talk, ESP_OPENCLAW_TALK_SETUP_FAILED,
            "Talk configuration lookup failed", result->error_code);
        goto done;
    }
    /* Freeze the selected key before create; close must not rediscover
     * a new owner after configuration changes or cancellation. */
    talk->session_key = resolve_default_session_key(result->payload_json);
    if (talk->session_key == NULL) {
        signal_failed(talk, ESP_OPENCLAW_TALK_SETUP_FAILED,
            "Talk routing configuration is invalid", NULL);
        goto done;
    }
    esp_err_t err = request_talk_create(talk);
    if (err != ESP_OK) {
        signal_failed(talk, ESP_OPENCLAW_TALK_SETUP_FAILED,
            "Talk create request failed", esp_err_to_name(err));
    }

done:
    release_talk_signaling(talk);
}

static int talk_signaling_start(
    esp_peer_signaling_cfg_t *cfg,
    esp_peer_signaling_handle_t *handle)
{
    if (cfg == NULL || handle == NULL || cfg->extra_cfg == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    const esp_openclaw_talk_signaling_config_t *config = cfg->extra_cfg;
    if (config->operator_node == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }

    talk_signaling_t *talk = calloc(1, sizeof(*talk));
    if (talk == NULL) {
        return ESP_PEER_ERR_NO_MEM;
    }
    talk->state_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    talk->refs = 1;
    talk->signaling = *cfg;
    talk->operator_node = config->operator_node;
    talk->gateway_http_base_url = duplicate_optional(config->gateway_http_base_url);
    talk->session_key = duplicate_optional(config->session_key);
    talk->provider = duplicate_optional(config->provider);
    talk->model = duplicate_optional(config->model);
    talk->voice = duplicate_optional(config->voice);
    talk->silence_duration_ms = config->silence_duration_ms;
    talk->setup_failed_cb = config->setup_failed_cb;
    talk->setup_failed_ctx = config->setup_failed_ctx;
    if (config->session_key != NULL && config->session_key[0] != '\0' && talk->session_key == NULL) {
        free_talk_signaling(talk);
        return ESP_PEER_ERR_NO_MEM;
    }

    esp_err_t err = talk->session_key != NULL
        ? request_talk_create(talk)
        : request_talk_rpc(talk, "talk.config", "{\"includeSecrets\":false}", handle_talk_config);
    if (err != ESP_OK) {
        release_talk_signaling(talk);
        return ESP_PEER_ERR_FAIL;
    }
    *handle = talk;
    return ESP_PEER_ERR_NONE;
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    http_response_t *response = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0 || response->failed) {
        return ESP_OK;
    }
    if (response->len + (size_t)event->data_len > MAX_SDP_RESPONSE_BYTES) {
        response->failed = true;
        return ESP_FAIL;
    }
    char *resized = realloc(response->data, response->len + (size_t)event->data_len + 1U);
    if (resized == NULL) {
        response->failed = true;
        return ESP_ERR_NO_MEM;
    }
    response->data = resized;
    memcpy(response->data + response->len, event->data, (size_t)event->data_len);
    response->len += (size_t)event->data_len;
    response->data[response->len] = '\0';
    return ESP_OK;
}

static char *duplicate_sdp(const esp_peer_signaling_msg_t *message)
{
    if (message == NULL || message->data == NULL) {
        return NULL;
    }
    size_t len = message->size > 0 ? (size_t)message->size : strlen((char *)message->data);
    char *sdp = malloc(len + 1U);
    if (sdp != NULL) {
        memcpy(sdp, message->data, len);
        sdp[len] = '\0';
    }
    return sdp;
}

static int exchange_sdp(talk_signaling_t *talk, const char *sdp)
{
    http_response_t response = {0};
    esp_http_client_config_t config = {
        .url = talk->offer_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .event_handler = http_event_handler,
        .user_data = &response,
        .buffer_size = 4096,
        .buffer_size_tx = 8192,
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_PEER_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Content-Type", "application/sdp");
    size_t auth_len = strlen("Bearer ") + strlen(talk->client_secret) + 1U;
    char *authorization = malloc(auth_len);
    if (authorization == NULL) {
        esp_http_client_cleanup(client);
        return ESP_PEER_ERR_NO_MEM;
    }
    snprintf(authorization, auth_len, "Bearer %s", talk->client_secret);
    esp_http_client_set_header(client, "Authorization", authorization);
    for (size_t i = 0; i < talk->offer_header_count; ++i) {
        esp_http_client_set_header(
            client,
            talk->offer_headers[i].name,
            talk->offer_headers[i].value);
    }
    esp_http_client_set_post_field(client, sdp, (int)strlen(sdp));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    free(authorization);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || response.failed || status < 200 || status >= 300 ||
        response.data == NULL || response.len < 3 ||
        strncmp(response.data, "v=0", 3) != 0) {
        ESP_LOGE(TAG, "SDP exchange failed: status=%d err=%s", status, esp_err_to_name(err));
        free(response.data);
        return ESP_PEER_ERR_FAIL;
    }

    esp_peer_signaling_msg_t answer = {
        .type = ESP_PEER_SIGNALING_MSG_SDP,
        .data = (uint8_t *)response.data,
        .size = (int)response.len,
    };
    int callback_result = talk->signaling.on_msg != NULL
        ? talk->signaling.on_msg(&answer, talk->signaling.ctx)
        : ESP_PEER_ERR_NONE;
    free(response.data);
    return callback_result;
}

static int talk_signaling_send_msg(
    esp_peer_signaling_handle_t handle,
    esp_peer_signaling_msg_t *message)
{
    talk_signaling_t *talk = handle;
    if (talk == NULL || message == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    if (!retain_if_running(talk)) {
        return ESP_PEER_ERR_FAIL;
    }
    if (message->type == ESP_PEER_SIGNALING_MSG_BYE) {
        release_talk_signaling(talk);
        return ESP_PEER_ERR_NONE;
    }
    if (message->type != ESP_PEER_SIGNALING_MSG_SDP || !talk->gateway_owned ||
        talk->offer_url == NULL ||
        talk->client_secret == NULL) {
        release_talk_signaling(talk);
        return ESP_PEER_ERR_FAIL;
    }
    char *sdp = duplicate_sdp(message);
    if (sdp == NULL) {
        release_talk_signaling(talk);
        return ESP_PEER_ERR_NO_MEM;
    }
    int result = exchange_sdp(talk, sdp);
    free(sdp);
    release_talk_signaling(talk);
    return result;
}

static void ignore_close_result(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_gateway_result_t *result,
    void *user_ctx)
{
    (void)node;
    (void)result;
    (void)user_ctx;
}

static int talk_signaling_stop(esp_peer_signaling_handle_t handle)
{
    talk_signaling_t *talk = handle;
    if (talk == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&talk->state_lock);
    if (talk->stopped) {
        portEXIT_CRITICAL(&talk->state_lock);
        return ESP_PEER_ERR_FAIL;
    }
    talk->stopped = true;
    bool notify = !talk->close_notified;
    bool close = talk->gateway_owned;
    talk->close_notified = true;
    portEXIT_CRITICAL(&talk->state_lock);
    if (close) {
        close_voice_session(talk->operator_node, talk->session_key, talk->voice_session_id);
    }
    if (notify && talk->signaling.on_close != NULL) {
        talk->signaling.on_close(talk->signaling.ctx);
    }
    release_talk_signaling(talk);
    return ESP_PEER_ERR_NONE;
}

const esp_peer_signaling_impl_t *esp_openclaw_talk_signaling_impl(void)
{
    static const esp_peer_signaling_impl_t implementation = {
        .start = talk_signaling_start,
        .send_msg = talk_signaling_send_msg,
        .stop = talk_signaling_stop,
    };
    return &implementation;
}
