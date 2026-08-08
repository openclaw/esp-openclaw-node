/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_openclaw_node_wifi.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

static const char *TAG = "esp_openclaw_node_wifi";

static EventGroupHandle_t s_wifi_events;
static SemaphoreHandle_t s_wifi_lock;
static esp_openclaw_node_wifi_status_t s_status;
static bool s_started;
static bool s_should_reconnect;

#define WIFI_CONNECTED_BIT BIT0

static void clear_ip_fields_locked(void)
{
    s_status.connected = false;
    s_status.ip[0] = '\0';
    s_status.netmask[0] = '\0';
    s_status.gateway[0] = '\0';
    s_status.rssi = 0;
    s_status.channel = 0;
    s_status.authmode = WIFI_AUTH_OPEN;
}

static void clear_connection_state(void)
{
    if (s_wifi_events != NULL) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
    if (s_wifi_lock != NULL) {
        xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
        clear_ip_fields_locked();
        s_status.last_disconnect_reason = 0;
        xSemaphoreGive(s_wifi_lock);
    }
}

static void update_credentials_status_locked(const char *ssid)
{
    size_t ssid_len = ssid != NULL ? strnlen(ssid, sizeof(s_status.ssid) - 1U) : 0;
    s_status.has_saved_network = ssid_len > 0;
    memset(s_status.ssid, 0, sizeof(s_status.ssid));
    if (s_status.has_saved_network) {
        memcpy(s_status.ssid, ssid, ssid_len);
    }
}

static void update_credentials_status_from_config_locked(const wifi_config_t *config)
{
    size_t ssid_len = strnlen((const char *)config->sta.ssid, sizeof(config->sta.ssid));
    s_status.has_saved_network = ssid_len > 0;
    s_status.ssid[0] = '\0';
    if (s_status.has_saved_network) {
        memcpy(s_status.ssid, config->sta.ssid, ssid_len);
        s_status.ssid[ssid_len] = '\0';
    }
}

static esp_err_t copy_wifi_config_field(
    uint8_t *dst,
    size_t dst_size,
    const char *value,
    const char *field_name)
{
    memset(dst, 0, dst_size);
    if (value == NULL) {
        return ESP_OK;
    }

    size_t value_len = strlen(value);
    if (value_len > dst_size) {
        ESP_LOGW(
            TAG,
            "%s length %u exceeds supported maximum %u bytes",
            field_name,
            (unsigned)value_len,
            (unsigned)dst_size);
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(dst, value, value_len);
    return ESP_OK;
}

static esp_err_t init_wifi_config(wifi_config_t *config, const char *ssid, const char *passphrase)
{
    memset(config, 0, sizeof(*config));
    ESP_RETURN_ON_ERROR(
        copy_wifi_config_field(config->sta.ssid, sizeof(config->sta.ssid), ssid, "SSID"),
        TAG,
        "invalid SSID");
    ESP_RETURN_ON_ERROR(
        copy_wifi_config_field(
            config->sta.password,
            sizeof(config->sta.password),
            passphrase,
            "passphrase"),
        TAG,
        "invalid passphrase");
    config->sta.threshold.authmode = WIFI_AUTH_OPEN;
    config->sta.pmf_cfg.capable = true;
    config->sta.pmf_cfg.required = false;
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            if (s_wifi_lock != NULL) {
                xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
                bool should_connect = s_status.has_saved_network && s_should_reconnect;
                char ssid[sizeof(s_status.ssid)] = {0};
                strncpy(ssid, s_status.ssid, sizeof(ssid) - 1U);
                xSemaphoreGive(s_wifi_lock);
                if (should_connect) {
                    ESP_LOGI(TAG, "connecting to SSID %s", ssid);
                    if (esp_wifi_connect() != ESP_OK) {
                        ESP_LOGW(TAG, "initial wifi connect failed");
                    }
                } else {
                    ESP_LOGI(TAG, "wifi ready; waiting for credentials from REPL");
                }
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *event =
                (const wifi_event_sta_disconnected_t *)event_data;
            bool should_retry = false;
            if (s_wifi_events != NULL) {
                xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
            }
            if (s_wifi_lock != NULL) {
                xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
                clear_ip_fields_locked();
                s_status.last_disconnect_reason = (uint8_t)event->reason;
                should_retry = s_status.has_saved_network && s_should_reconnect;
                xSemaphoreGive(s_wifi_lock);
            }
            if (should_retry) {
                ESP_LOGW(TAG, "wifi disconnected, reason=%u; reconnecting", (unsigned)event->reason);
                esp_err_t err = esp_wifi_connect();
                if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
                    ESP_LOGW(TAG, "wifi reconnect failed: %s", esp_err_to_name(err));
                }
            } else {
                ESP_LOGI(TAG, "wifi disconnected, reason=%u", (unsigned)event->reason);
            }
            break;
        }
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        if (s_wifi_lock != NULL) {
            xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
            s_status.connected = true;
            s_status.last_disconnect_reason = 0;
            snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&event->ip_info.ip));
            snprintf(s_status.netmask, sizeof(s_status.netmask), IPSTR, IP2STR(&event->ip_info.netmask));
            snprintf(s_status.gateway, sizeof(s_status.gateway), IPSTR, IP2STR(&event->ip_info.gw));
            xSemaphoreGive(s_wifi_lock);
        }
        if (s_wifi_events != NULL) {
            xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

esp_err_t esp_openclaw_node_wifi_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_wifi_events = xEventGroupCreate();
    s_wifi_lock = xSemaphoreCreateMutex();
    if (s_wifi_events == NULL || s_wifi_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(&s_status, 0, sizeof(s_status));

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    ESP_RETURN_ON_ERROR(err, TAG, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL),
        TAG,
        "register WIFI_EVENT failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL),
        TAG,
        "register IP_EVENT failed");

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG, "esp_wifi_set_storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode failed");

    wifi_config_t wifi_config = {0};
    err = esp_wifi_get_config(WIFI_IF_STA, &wifi_config);
    ESP_RETURN_ON_ERROR(err, TAG, "esp_wifi_get_config failed");

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    update_credentials_status_from_config_locked(&wifi_config);
    s_should_reconnect = s_status.has_saved_network;
    clear_ip_fields_locked();
    s_status.last_disconnect_reason = 0;
    xSemaphoreGive(s_wifi_lock);

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "esp_wifi_set_ps failed");

    s_started = true;
    return ESP_OK;
}

esp_err_t esp_openclaw_node_wifi_set_credentials(const char *ssid, const char *passphrase)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started || s_wifi_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *resolved_passphrase = passphrase != NULL ? passphrase : "";
    wifi_config_t new_config = {0};
    ESP_RETURN_ON_ERROR(init_wifi_config(&new_config, ssid, resolved_passphrase), TAG, "invalid wifi config");

    ESP_RETURN_ON_ERROR(
        esp_wifi_set_config(WIFI_IF_STA, &new_config),
        TAG,
        "esp_wifi_set_config failed");

    clear_connection_state();
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    update_credentials_status_locked(ssid);
    s_should_reconnect = true;
    xSemaphoreGive(s_wifi_lock);

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT && err != ESP_ERR_WIFI_CONN) {
        return err;
    }
    err = esp_wifi_connect();
    if (err == ESP_ERR_WIFI_CONN) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "starting wifi connect failed");
    return ESP_OK;
}

esp_err_t esp_openclaw_node_wifi_clear_credentials(void)
{
    if (!s_started || s_wifi_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        return err;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_restore(), TAG, "esp_wifi_restore failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG, "esp_wifi_set_storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode failed");

    clear_connection_state();
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    update_credentials_status_locked(NULL);
    s_should_reconnect = false;
    xSemaphoreGive(s_wifi_lock);

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "esp_wifi_set_ps failed");
    return ESP_OK;
}

esp_err_t esp_openclaw_node_wifi_connect(void)
{
    if (!s_started || s_wifi_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    bool wifi_is_configured = s_status.has_saved_network;
    s_should_reconnect = wifi_is_configured;
    xSemaphoreGive(s_wifi_lock);
    if (!wifi_is_configured) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_wifi_connect();
    if (err == ESP_ERR_WIFI_CONN) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "starting wifi connect failed");
    return ESP_OK;
}

esp_err_t esp_openclaw_node_wifi_disconnect(void)
{
    if (!s_started || s_wifi_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    clear_connection_state();
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    s_should_reconnect = false;
    xSemaphoreGive(s_wifi_lock);

    esp_err_t err = esp_wifi_disconnect();
    if (err == ESP_ERR_WIFI_NOT_CONNECT) {
        err = ESP_OK;
    }
    return err;
}

bool esp_openclaw_node_wifi_wait_for_connection(TickType_t timeout_ticks)
{
    if (s_wifi_events == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdFALSE,
        timeout_ticks);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool esp_openclaw_node_wifi_is_connected(void)
{
    if (s_wifi_lock == NULL) {
        return false;
    }

    bool connected = false;
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    connected = s_status.connected;
    xSemaphoreGive(s_wifi_lock);
    return connected;
}

void esp_openclaw_node_wifi_get_status(esp_openclaw_node_wifi_status_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (s_wifi_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    *status = s_status;
    xSemaphoreGive(s_wifi_lock);

    if (status->connected) {
        wifi_ap_record_t ap_info = {0};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            status->rssi = ap_info.rssi;
            status->channel = ap_info.primary;
            status->authmode = (uint8_t)ap_info.authmode;
        }
    }
}
