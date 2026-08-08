/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_openclaw_node_example_repl.h"
#include "esp_openclaw_node_example_repl_cmd.h"

#include "esp_check.h"
#include "esp_console.h"
#include "esp_log.h"

static const char *TAG = "esp_openclaw_node_repl";
static esp_console_repl_t *s_repl;

esp_err_t esp_openclaw_node_example_repl_start(esp_openclaw_node_handle_t node)
{
    if (node == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_repl != NULL) {
        return ESP_OK;
    }

    esp_console_register_help_command();
    ESP_RETURN_ON_ERROR(
        esp_openclaw_node_example_repl_register_commands(node),
        TAG,
        "registering REPL commands failed");

    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "openclaw> ";
    repl_config.max_cmdline_length = 512;

    const char *console_transport = NULL;
#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(
        esp_console_new_repl_uart(&uart_config, &repl_config, &s_repl),
        TAG,
        "creating UART REPL failed");
    console_transport = "UART";
#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
    esp_console_dev_usb_cdc_config_t usb_cdc_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(
        esp_console_new_repl_usb_cdc(&usb_cdc_config, &repl_config, &s_repl),
        TAG,
        "creating USB CDC REPL failed");
    console_transport = "USB CDC";
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t usb_serial_jtag_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(
        esp_console_new_repl_usb_serial_jtag(&usb_serial_jtag_config, &repl_config, &s_repl),
        TAG,
        "creating USB Serial/JTAG REPL failed");
    console_transport = "USB Serial/JTAG";
#else
#error Unsupported console transport for the OpenClaw REPL
#endif
    ESP_RETURN_ON_ERROR(esp_console_start_repl(s_repl), TAG, "starting REPL failed");

    ESP_LOGI(
        TAG,
        "%s REPL ready; use `wifi ...`, `gateway ...`, `reboot`, or `status`",
        console_transport);
    return ESP_OK;
}
