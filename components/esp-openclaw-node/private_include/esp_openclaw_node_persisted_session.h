/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint8_t version;
    char *gateway_uri;
    char *device_token;
} esp_openclaw_node_persisted_session_t;

esp_err_t esp_openclaw_node_persisted_session_load(
    const char *role,
    esp_openclaw_node_persisted_session_t *session);

void esp_openclaw_node_persisted_session_free(esp_openclaw_node_persisted_session_t *session);

esp_err_t esp_openclaw_node_persisted_session_store(
    const char *role,
    esp_openclaw_node_persisted_session_t *session,
    const esp_openclaw_node_persisted_session_t *update);

bool esp_openclaw_node_persisted_session_is_present(const esp_openclaw_node_persisted_session_t *session);
