/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include "esp_openclaw_node.h"
#include "esp_peer_signaling.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_OPENCLAW_TALK_SETUP_FAILED = 0,
    ESP_OPENCLAW_TALK_GATEWAY_UPGRADE_REQUIRED,
} esp_openclaw_talk_setup_result_t;

typedef void (*esp_openclaw_talk_setup_failed_cb_t)(
    esp_openclaw_talk_setup_result_t result,
    void *user_ctx);

/** Configuration copied by the signaling implementation at start. */
typedef struct {
    /** Ready operator client with operator.talk; default routing also needs operator.read. */
    esp_openclaw_node_handle_t operator_node;
    /** HTTP(S) Gateway origin used when talk.client.create returns a relative offer URL. */
    const char *gateway_http_base_url;
    /**
     * Explicit key, or NULL/empty to resolve Gateway Talk routing.
     * Without a configured Talk owner, retain the Gateway-resolved `main` default.
     */
    const char *session_key;
    /** Optional provider override; omit to use Gateway Talk configuration. */
    const char *provider;
    /** Optional model override, including GPT-Live model ids. */
    const char *model;
    /** Optional provider voice override. */
    const char *voice;
    /** Optional notification for failures before peer creation. */
    esp_openclaw_talk_setup_failed_cb_t setup_failed_cb;
    void *setup_failed_ctx;
    /**
     * Optional provider server-VAD post-speech silence override in milliseconds.
     * Zero omits the field and preserves the Gateway configuration or default.
     */
    uint16_t silence_duration_ms;
} esp_openclaw_talk_signaling_config_t;

/**
 * Provider-neutral ESP WebRTC signaling implementation backed by OpenClaw Talk.
 *
 * `talk.client.create` supplies a single-use Gateway broker credential. The
 * ESP32 posts its SDP to the returned URL while the Gateway owns realtime
 * control, provider policy, and agent delegation.
 */
const esp_peer_signaling_impl_t *esp_openclaw_talk_signaling_impl(void);

/** A single prepared call; configuration strings are copied at preparation. */
typedef struct esp_openclaw_talk_call *esp_openclaw_talk_call_handle_t;
typedef void (*esp_openclaw_talk_closed_cb_t)(void *ctx);

esp_err_t esp_openclaw_talk_call_prepare(
    const esp_openclaw_talk_signaling_config_t *config,
    esp_openclaw_talk_call_handle_t *out_call);
/** Set before start. Notification only: never close/quiesce in a callback. */
esp_err_t esp_openclaw_talk_call_set_closed_handler(
    esp_openclaw_talk_call_handle_t call, esp_openclaw_talk_closed_cb_t cb, void *ctx);
/** Bind extra_cfg=&call, extra_size=sizeof(call); SDK copies the reference only. */
const esp_peer_signaling_impl_t *esp_openclaw_talk_call_signaling_impl(void);
/** Retain under the owner's lock before handing a call to another dispatcher. */
void esp_openclaw_talk_call_retain(esp_openclaw_talk_call_handle_t call);
void esp_openclaw_talk_call_release(esp_openclaw_talk_call_handle_t call);
/** Parse borrowed JSON synchronously. Only exact talk.event/session.closed matches. */
void esp_openclaw_talk_call_gateway_event(
    esp_openclaw_talk_call_handle_t call, esp_openclaw_node_handle_t source,
    const char *event, const char *payload_json);
/** Nonblocking, durable cancellation: seals further SDK/application dispatch. */
void esp_openclaw_talk_call_cancel(esp_openclaw_talk_call_handle_t call);
/**
 * Worker only, after start returns: cancel and drain admitted dispatches, then
 * esp_webrtc_close, then release the owner's reference. Keep callback contexts
 * alive through SDK close. Does not wait for remote replies. The operator Node
 * must outlive queued RPC cleanup; reconnect by reusing it, not destroying it.
 * Only one worker may quiesce/close a call. Never wait while holding owner locks.
 */
void esp_openclaw_talk_call_quiesce(esp_openclaw_talk_call_handle_t call);

#ifdef __cplusplus
}
#endif
