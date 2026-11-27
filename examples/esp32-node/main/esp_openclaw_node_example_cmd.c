/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_openclaw_node_example_cmd.h"

#include "esp_openclaw_node_common_device_node_cmd.h"
#include "esp_check.h"
#include "soc/soc_caps.h"
#include "esp_openclaw_node_example_adc_node_cmd.h"
#include "esp_openclaw_node_example_gpio_node_cmd.h"
#include "compact_board.h"

static const char *TAG = "esp_openclaw_node_example";

esp_err_t esp_openclaw_node_example_register_node_commands(esp_openclaw_node_handle_t node)
{
#if CONFIG_EXAMPLE_BOARD_M5STICKC
    static const esp_openclaw_node_device_commands_config_t device = {
        .device_name = "M5StickC", .model_identifier = "m5stack-m5stickc",
    };
#elif CONFIG_EXAMPLE_BOARD_T_EMBED
    static const esp_openclaw_node_device_commands_config_t device = {
        .device_name = "T-Embed", .model_identifier = "lilygo-t-embed",
    };
#else
    static const esp_openclaw_node_device_commands_config_t device = {0};
#endif
    ESP_RETURN_ON_ERROR(
        esp_openclaw_node_register_device_commands(node, &device),
        TAG,
        "registering device commands failed");
    ESP_RETURN_ON_ERROR(
        esp_openclaw_node_example_register_gpio_node_commands(node),
        TAG,
        "registering GPIO commands failed");
#if SOC_ADC_SUPPORTED
    ESP_RETURN_ON_ERROR(
        esp_openclaw_node_example_register_adc_node_commands(node),
        TAG,
        "registering ADC commands failed");
#endif
    ESP_RETURN_ON_ERROR(compact_board_register_commands(node), TAG, "registering display commands failed");
    return ESP_OK;
}
