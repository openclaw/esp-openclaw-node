/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "esp_openclaw_node.h"

typedef esp_err_t (*esp_openclaw_node_storage_metrics_fn_t)(
    void *context,
    uint64_t *total_bytes,
    uint64_t *free_bytes);

/** Optional product metadata and status hooks for the canonical device commands. */
typedef struct {
    const char *device_name;
    const char *model_identifier;
    const char *locale;
    esp_openclaw_node_storage_metrics_fn_t get_storage_metrics;
    void *context;
} esp_openclaw_node_device_commands_config_t;

/**
 * @brief Register the common device commands shared by the examples.
 *
 * The helper registers `device.info`, `device.status`, and `wifi.status`.
 *
 * @param[in] node OpenClaw Node instance to extend.
 *
 * @return
 *      - `ESP_OK` on success
 *      - an ESP-IDF error code if registration fails
 */
esp_err_t esp_openclaw_node_common_register_device_node_commands(esp_openclaw_node_handle_t node);

esp_err_t esp_openclaw_node_register_device_commands(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_device_commands_config_t *config);
