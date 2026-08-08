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
    char *offer_url;
    char *client_secret;
    char *voice_session_id;
    offer_header_t *offer_headers;
    size_t offer_header_count;
    portMUX_TYPE state_lock;
    size_t refs;
    bool create_pending;
    bool stopped;
    bool close_notified;
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

static void free_offer_headers(talk_signaling_t *talk)
{
    for (size_t i = 0; i < talk->offer_header_count; ++i) {
        free(talk->offer_headers[i].name);
        free(talk->offer_headers[i].value);
    }
    free(talk->offer_headers);
    talk->offer_headers = NULL;
    talk->offer_header_count = 0;
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

static void retain_talk_signaling(talk_signaling_t *talk)
{
    portENTER_CRITICAL(&talk->state_lock);
    ++talk->refs;
    portEXIT_CRITICAL(&talk->state_lock);
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

static char *resolve_offer_url(const char *base_url, const char *offer_url)
{
    if (offer_url == NULL || offer_url[0] == '\0') {
        return NULL;
    }
    if (strncmp(offer_url, "https://", 8) == 0 ||
        strncmp(offer_url, "http://", 7) == 0) {
        return strdup(offer_url);
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
        if (dst->name == NULL || dst->value == NULL) {
            return false;
        }
        ++talk->offer_header_count;
    }
    return true;
}

static void signal_failed(talk_signaling_t *talk, const char *message)
{
    ESP_LOGE(TAG, "%s", message);
    bool notify = false;
    portENTER_CRITICAL(&talk->state_lock);
    if (!talk->close_notified) {
        talk->close_notified = true;
        notify = true;
    }
    portEXIT_CRITICAL(&talk->state_lock);
    if (notify && talk->signaling.on_close != NULL) {
        talk->signaling.on_close(talk->signaling.ctx);
    }
}

static void handle_talk_create(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_gateway_result_t *result,
    void *user_ctx)
{
    (void)node;
    talk_signaling_t *talk = user_ctx;
    portENTER_CRITICAL(&talk->state_lock);
    talk->create_pending = false;
    bool stopped = talk->stopped;
    portEXIT_CRITICAL(&talk->state_lock);
    if (stopped) {
        release_talk_signaling(talk);
        return;
    }
    if (!result->ok || result->payload_json == NULL) {
        signal_failed(talk, "Gateway rejected talk.client.create");
        release_talk_signaling(talk);
        return;
    }

    cJSON *payload = cJSON_Parse(result->payload_json);
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
    bool valid = cJSON_IsString(transport) &&
                 strcmp(transport->valuestring, "webrtc") == 0 &&
                 cJSON_IsString(offer_url) &&
                 cJSON_IsString(client_secret) &&
                 cJSON_IsString(voice_session_id);
    if (valid) {
        talk->offer_url = resolve_offer_url(
            talk->gateway_http_base_url,
            offer_url->valuestring);
        talk->client_secret = duplicate_optional(client_secret->valuestring);
        talk->voice_session_id = duplicate_optional(voice_session_id->valuestring);
        valid = talk->offer_url != NULL && talk->client_secret != NULL &&
                talk->voice_session_id != NULL &&
                copy_offer_headers(talk, offer_headers);
    }
    cJSON_Delete(payload);
    if (!valid) {
        signal_failed(talk, "talk.client.create returned an invalid WebRTC offer");
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
    talk->session_key = strdup(
        config->session_key != NULL && config->session_key[0] != '\0'
            ? config->session_key
            : "main");
    talk->provider = duplicate_optional(config->provider);
    talk->model = duplicate_optional(config->model);
    talk->voice = duplicate_optional(config->voice);
    talk->silence_duration_ms = config->silence_duration_ms;
    if (talk->session_key == NULL) {
        free_talk_signaling(talk);
        return ESP_PEER_ERR_NO_MEM;
    }

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
    char *params_json = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    if (params_json == NULL) {
        free_talk_signaling(talk);
        return ESP_PEER_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&talk->state_lock);
    talk->create_pending = true;
    portEXIT_CRITICAL(&talk->state_lock);
    retain_talk_signaling(talk);
    esp_err_t err = esp_openclaw_node_gateway_request(
        talk->operator_node,
        "talk.client.create",
        params_json,
        handle_talk_create,
        talk);
    free(params_json);
    if (err != ESP_OK) {
        release_talk_signaling(talk);
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
    if (message->type != ESP_PEER_SIGNALING_MSG_SDP || talk->offer_url == NULL ||
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
    talk->close_notified = true;
    portEXIT_CRITICAL(&talk->state_lock);
    if (talk->voice_session_id != NULL) {
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "sessionKey", talk->session_key);
        cJSON_AddStringToObject(params, "voiceSessionId", talk->voice_session_id);
        char *params_json = cJSON_PrintUnformatted(params);
        cJSON_Delete(params);
        if (params_json != NULL) {
            (void)esp_openclaw_node_gateway_request(
                talk->operator_node,
                "talk.client.close",
                params_json,
                ignore_close_result,
                NULL);
            free(params_json);
        }
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
