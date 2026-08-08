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
    /** Ready operator-role client with operator.talk scope. */
    esp_openclaw_node_handle_t operator_node;
    /** HTTP(S) Gateway origin used when talk.client.create returns a relative offer URL. */
    const char *gateway_http_base_url;
    /** Agent session key; defaults to `main`. */
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

#ifdef __cplusplus
}
#endif
