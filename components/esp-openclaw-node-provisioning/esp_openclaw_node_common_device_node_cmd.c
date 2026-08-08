/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_openclaw_node_common_device_node_cmd.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_openclaw_node_example_json.h"
#include "esp_openclaw_node_wifi.h"

static const char *TAG = "esp_openclaw_node_device_cmd";

static const char *wifi_auth_mode_name(uint8_t authmode)
{
    switch ((wifi_auth_mode_t)authmode) {
    case WIFI_AUTH_OPEN:
        return "open";
    case WIFI_AUTH_WEP:
        return "wep";
    case WIFI_AUTH_WPA_PSK:
        return "wpa-psk";
    case WIFI_AUTH_WPA2_PSK:
        return "wpa2-psk";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "wpa-wpa2-psk";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "wpa2-enterprise";
    case WIFI_AUTH_WPA3_PSK:
        return "wpa3-psk";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "wpa2-wpa3-psk";
    case WIFI_AUTH_WAPI_PSK:
        return "wapi-psk";
    default:
        return "unknown";
    }
}

static void add_wifi_status_fields(cJSON *object)
{
    esp_openclaw_node_wifi_status_t wifi = {0};
    esp_openclaw_node_wifi_get_status(&wifi);
    cJSON_AddBoolToObject(object, "connected", wifi.connected);
    cJSON_AddStringToObject(object, "ssid", wifi.ssid);
    if (wifi.ip[0] != '\0') {
        cJSON_AddStringToObject(object, "ip", wifi.ip);
        cJSON_AddStringToObject(object, "netmask", wifi.netmask);
        cJSON_AddStringToObject(object, "gateway", wifi.gateway);
    }
    if (wifi.connected) {
        cJSON_AddNumberToObject(object, "rssi", wifi.rssi);
        cJSON_AddNumberToObject(object, "channel", wifi.channel);
        cJSON_AddStringToObject(object, "authMode", wifi_auth_mode_name(wifi.authmode));
    }
    if (wifi.last_disconnect_reason != 0) {
        cJSON_AddNumberToObject(object, "lastDisconnectReason", wifi.last_disconnect_reason);
    }
}

static bool empty_object(const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    bool valid = cJSON_IsObject(root) && root->child == NULL;
    cJSON_Delete(root);
    return valid;
}

static cJSON *build_device_info_payload(
    const esp_openclaw_node_device_commands_config_t *config)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char build[40];
    snprintf(build, sizeof(build), "%s %s", app->date, app->time);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "deviceName",
        config != NULL && config->device_name != NULL ? config->device_name : "OpenClaw ESP Node");
    cJSON_AddStringToObject(root, "modelIdentifier",
        config != NULL && config->model_identifier != NULL ? config->model_identifier : CONFIG_IDF_TARGET);
    cJSON_AddStringToObject(root, "systemName", "ESP-IDF");
    cJSON_AddStringToObject(root, "systemVersion", esp_get_idf_version());
    cJSON_AddStringToObject(root, "appVersion", app->version);
    cJSON_AddStringToObject(root, "appBuild", build);
    cJSON_AddStringToObject(root, "locale",
        config != NULL && config->locale != NULL ? config->locale : "en-US");
    return root;
}

static cJSON *build_device_status_payload(
    const esp_openclaw_node_device_commands_config_t *config)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *battery = cJSON_AddObjectToObject(root, "battery");
    cJSON_AddNullToObject(battery, "level");
    cJSON_AddStringToObject(battery, "state", "unknown");
    cJSON_AddBoolToObject(battery, "lowPowerModeEnabled", false);
    cJSON *thermal = cJSON_AddObjectToObject(root, "thermal");
    cJSON_AddStringToObject(thermal, "state", "nominal");
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    if (config != NULL && config->get_storage_metrics != NULL) {
        if (config->get_storage_metrics(config->context, &total, &free_bytes) != ESP_OK ||
            free_bytes > total) {
            total = 0;
            free_bytes = 0;
        }
    }
    cJSON *storage = cJSON_AddObjectToObject(root, "storage");
    cJSON_AddNumberToObject(storage, "totalBytes", (double)total);
    cJSON_AddNumberToObject(storage, "freeBytes", (double)free_bytes);
    cJSON_AddNumberToObject(storage, "usedBytes", (double)(total - free_bytes));
    esp_openclaw_node_wifi_status_t wifi = {0};
    esp_openclaw_node_wifi_get_status(&wifi);
    cJSON *network = cJSON_AddObjectToObject(root, "network");
    cJSON_AddStringToObject(network, "status", wifi.connected ? "satisfied" : "unsatisfied");
    cJSON_AddBoolToObject(network, "isExpensive", false);
    cJSON_AddBoolToObject(network, "isConstrained", false);
    cJSON *interfaces = cJSON_AddArrayToObject(network, "interfaces");
    if (wifi.connected) cJSON_AddItemToArray(interfaces, cJSON_CreateString("wifi"));
    cJSON_AddNumberToObject(root, "uptimeSeconds", esp_timer_get_time() / 1000000.0);
    return root;
}

static cJSON *build_wifi_status_payload(void)
{
    cJSON *root = cJSON_CreateObject();
    add_wifi_status_fields(root);
    return root;
}

static esp_err_t handle_device_info(
    esp_openclaw_node_handle_t node,
    void *context,
    const char *params_json,
    size_t params_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node;
    if (!empty_object(params_json, params_len)) {
        out_error->code = "INVALID_PARAMS";
        out_error->message = "device.info accepts only {}";
        return ESP_ERR_INVALID_ARG;
    }
    return esp_openclaw_node_example_take_json_payload(
        build_device_info_payload(context),
        out_payload_json);
}

static esp_err_t handle_device_status(
    esp_openclaw_node_handle_t node,
    void *context,
    const char *params_json,
    size_t params_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node;
    if (!empty_object(params_json, params_len)) {
        out_error->code = "INVALID_PARAMS";
        out_error->message = "device.status accepts only {}";
        return ESP_ERR_INVALID_ARG;
    }
    return esp_openclaw_node_example_take_json_payload(
        build_device_status_payload(context),
        out_payload_json);
}

static esp_err_t handle_wifi_status(
    esp_openclaw_node_handle_t node,
    void *context,
    const char *params_json,
    size_t params_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node;
    (void)context;
    if (!empty_object(params_json, params_len)) {
        out_error->code = "INVALID_PARAMS";
        out_error->message = "wifi.status accepts only {}";
        return ESP_ERR_INVALID_ARG;
    }
    return esp_openclaw_node_example_take_json_payload(
        build_wifi_status_payload(),
        out_payload_json);
}

esp_err_t esp_openclaw_node_common_register_device_node_commands(esp_openclaw_node_handle_t node)
{
    return esp_openclaw_node_register_device_commands(node, NULL);
}

esp_err_t esp_openclaw_node_register_device_commands(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_device_commands_config_t *config)
{
    const esp_openclaw_node_command_t DEVICE_INFO_COMMAND = {
        .name = "device.info",
        .handler = handle_device_info,
        .context = (void *)config,
    };
    const esp_openclaw_node_command_t DEVICE_STATUS_COMMAND = {
        .name = "device.status",
        .handler = handle_device_status,
        .context = (void *)config,
    };
    static const esp_openclaw_node_command_t WIFI_STATUS_COMMAND = {
        .name = "wifi.status",
        .handler = handle_wifi_status,
        .context = NULL,
    };

    ESP_RETURN_ON_ERROR(
        esp_openclaw_node_register_capability(node, "device"),
        TAG,
        "registering device capability failed");
    ESP_RETURN_ON_ERROR(
        esp_openclaw_node_register_capability(node, "wifi"),
        TAG,
        "registering wifi capability failed");
    ESP_RETURN_ON_ERROR(
        esp_openclaw_node_register_command(node, &DEVICE_INFO_COMMAND),
        TAG,
        "registering device.info failed");
    ESP_RETURN_ON_ERROR(
        esp_openclaw_node_register_command(node, &DEVICE_STATUS_COMMAND),
        TAG,
        "registering device.status failed");
    ESP_RETURN_ON_ERROR(
        esp_openclaw_node_register_command(node, &WIFI_STATUS_COMMAND),
        TAG,
        "registering wifi.status failed");
    return ESP_OK;
}
