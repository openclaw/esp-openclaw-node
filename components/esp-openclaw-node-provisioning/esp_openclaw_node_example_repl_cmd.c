/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_openclaw_node_example_repl_cmd.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_console.h"
#include "esp_system.h"
#include "esp_openclaw_node_wifi.h"

static const char *TAG = "esp_openclaw_node_repl_cmd";
static esp_openclaw_node_handle_t s_node;

#define ESP_OPENCLAW_NODE_REPL_WIFI_CONNECT_TIMEOUT_TICKS pdMS_TO_TICKS(30000)

static int wait_for_wifi_before_connect(const char *request_name)
{
    if (esp_openclaw_node_wifi_is_connected()) {
        return 0;
    }

    printf("waiting for Wi-Fi before %s connect\n", request_name);
    if (!esp_openclaw_node_wifi_wait_for_connection(ESP_OPENCLAW_NODE_REPL_WIFI_CONNECT_TIMEOUT_TICKS)) {
        printf("%s connect not sent because Wi-Fi is still not connected\n", request_name);
        return 1;
    }
    return 0;
}

static int request_gateway_connect(
    const esp_openclaw_node_connect_request_t *request,
    const char *request_name)
{
    if (s_node == NULL) {
        printf("node is not initialized\n");
        return 1;
    }

    if (wait_for_wifi_before_connect(request_name) != 0) {
        return 1;
    }

    esp_err_t err = esp_openclaw_node_request_connect(s_node, request);
    if (err != ESP_OK) {
        printf("%s connect request failed: %s\n", request_name, esp_err_to_name(err));
        return 1;
    }

    printf("%s connect requested\n", request_name);
    return 0;
}

static int connect_setup_code_from_repl(const char *setup_code)
{
    if (setup_code == NULL || setup_code[0] == '\0') {
        printf("setup-code must not be empty\n");
        return 1;
    }

    const esp_openclaw_node_connect_request_t request = {
        .source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_SETUP_CODE,
        .gateway_uri = NULL,
        .value = setup_code,
    };
    return request_gateway_connect(&request, "setup-code");
}

static int connect_secret_from_repl(
    const char *gateway_uri,
    esp_openclaw_node_connect_source_t source,
    const char *secret,
    const char *secret_name)
{
    const esp_openclaw_node_connect_request_t request = {
        .source = source,
        .gateway_uri = gateway_uri,
        .value = secret,
    };
    return request_gateway_connect(&request, secret_name);
}

static int connect_no_auth_from_repl(const char *gateway_uri)
{
    const esp_openclaw_node_connect_request_t request = {
        .source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_NO_AUTH,
        .gateway_uri = gateway_uri,
        .value = NULL,
    };
    return request_gateway_connect(&request, "no-auth");
}

static int reconnect_saved_session_from_repl(void)
{
    const esp_openclaw_node_connect_request_t request =
        (esp_openclaw_node_connect_request_t){
            .source = ESP_OPENCLAW_NODE_CONNECT_SOURCE_SAVED_SESSION,
            .gateway_uri = NULL,
            .value = NULL,
        };
    return request_gateway_connect(&request, "saved-session");
}

static int disconnect_node_from_repl(void)
{
    if (s_node == NULL) {
        printf("node is not initialized\n");
        return 1;
    }

    esp_err_t err = esp_openclaw_node_request_disconnect(s_node);
    if (err != ESP_OK) {
        printf("disconnect request failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("disconnect requested\n");
    return 0;
}

static int handle_status_command(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (s_node == NULL) {
        printf("node is not initialized\n");
        return 1;
    }

    esp_openclaw_node_wifi_status_t wifi_status = {0};
    esp_openclaw_node_wifi_get_status(&wifi_status);

    printf("saved session available: %s\n", esp_openclaw_node_has_saved_session(s_node) ? "yes" : "no");
    printf("wifi configured: %s\n", wifi_status.has_saved_network ? "yes" : "no");
    if (wifi_status.has_saved_network && wifi_status.ssid[0] != '\0') {
        printf("wifi ssid: %s\n", wifi_status.ssid);
    }
    printf("wifi connected: %s\n", wifi_status.connected ? "yes" : "no");
    if (!wifi_status.connected && wifi_status.last_disconnect_reason != 0) {
        printf("wifi disconnect reason: %u\n", (unsigned)wifi_status.last_disconnect_reason);
    }
    if (wifi_status.connected && wifi_status.ip[0] != '\0') {
        printf("wifi ip: %s\n", wifi_status.ip);
    }
    return 0;
}

static int handle_wifi_command(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "set") == 0) {
        const char *passphrase = argc >= 4 ? argv[3] : "";
        esp_err_t err = esp_openclaw_node_wifi_set_credentials(argv[2], passphrase);
        if (err != ESP_OK) {
            printf("wifi set failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("wifi credentials saved\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "clear") == 0) {
        esp_err_t err = esp_openclaw_node_wifi_clear_credentials();
        if (err != ESP_OK) {
            printf("wifi clear failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("wifi credentials cleared\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "connect") == 0) {
        esp_err_t err = esp_openclaw_node_wifi_connect();
        if (err != ESP_OK) {
            printf("wifi connect failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("wifi connect requested\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "disconnect") == 0) {
        esp_err_t err = esp_openclaw_node_wifi_disconnect();
        if (err != ESP_OK) {
            printf("wifi disconnect failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("wifi disconnect requested\n");
        return 0;
    }

    printf("usage: wifi set <ssid> [passphrase]\n");
    printf("       wifi clear\n");
    printf("       wifi connect\n");
    printf("       wifi disconnect\n");
    return 1;
}

static int handle_gateway_command(int argc, char **argv)
{
    if (s_node == NULL) {
        printf("node is not initialized\n");
        return 1;
    }

    if (argc == 3 && strcmp(argv[1], "setup-code") == 0) {
        return connect_setup_code_from_repl(argv[2]);
    }
    if (argc == 4 && strcmp(argv[1], "token") == 0) {
        return connect_secret_from_repl(
            argv[2],
            ESP_OPENCLAW_NODE_CONNECT_SOURCE_GATEWAY_TOKEN,
            argv[3],
            "token");
    }
    if (argc == 4 && strcmp(argv[1], "password") == 0) {
        return connect_secret_from_repl(
            argv[2],
            ESP_OPENCLAW_NODE_CONNECT_SOURCE_GATEWAY_PASSWORD,
            argv[3],
            "password");
    }
    if (argc == 3 && strcmp(argv[1], "no-auth") == 0) {
        return connect_no_auth_from_repl(argv[2]);
    }
    if (argc == 2 && strcmp(argv[1], "connect") == 0) {
        return reconnect_saved_session_from_repl();
    }
    if (argc == 2 && strcmp(argv[1], "disconnect") == 0) {
        return disconnect_node_from_repl();
    }

    printf("usage: gateway setup-code <code>\n");
    printf("       gateway token <ws://host:port> <token>\n");
    printf("       gateway password <ws://host:port> <password>\n");
    printf("       gateway no-auth <ws://host:port>\n");
    printf("       gateway connect\n");
    printf("       gateway disconnect\n");
    return 1;
}

static int handle_reboot_command(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("rebooting\n");
    fflush(stdout);
    esp_restart();
    return 0;
}

esp_err_t esp_openclaw_node_example_repl_register_commands(esp_openclaw_node_handle_t node)
{
    if (node == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_node = node;

    const esp_console_cmd_t status_command = {
        .command = "status",
        .help = "Show saved-session availability and Wi-Fi state",
        .hint = NULL,
        .func = handle_status_command,
    };
    ESP_RETURN_ON_ERROR(
        esp_console_cmd_register(&status_command),
        TAG,
        "registering status command failed");

    const esp_console_cmd_t wifi_command = {
        .command = "wifi",
        .help = "Set, clear, connect, or disconnect Wi-Fi credentials",
        .hint = "set <ssid> [passphrase] | clear | connect | disconnect",
        .func = handle_wifi_command,
    };
    ESP_RETURN_ON_ERROR(
        esp_console_cmd_register(&wifi_command),
        TAG,
        "registering wifi command failed");

    const esp_console_cmd_t gateway_command = {
        .command = "gateway",
        .help = "Connect with setup-code, explicit auth, or saved session; or disconnect",
        .hint = "setup-code <code> | token <uri> <token> | password <uri> <password> | no-auth <uri> | connect | disconnect",
        .func = handle_gateway_command,
    };
    ESP_RETURN_ON_ERROR(
        esp_console_cmd_register(&gateway_command),
        TAG,
        "registering gateway command failed");

    const esp_console_cmd_t reboot_command = {
        .command = "reboot",
        .help = "Reboot the board immediately",
        .hint = NULL,
        .func = handle_reboot_command,
    };
    ESP_RETURN_ON_ERROR(
        esp_console_cmd_register(&reboot_command),
        TAG,
        "registering reboot command failed");

    return ESP_OK;
}
