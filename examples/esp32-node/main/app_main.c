/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_openclaw_node_example_cmd.h"
#include "esp_openclaw_node_example_repl.h"
#include "esp_openclaw_node_example_repl_cmd.h"
#include "esp_openclaw_node_example_saved_session_reconnect.h"
#include "esp_openclaw_node.h"
#include "esp_openclaw_node_wifi.h"
#include "compact_board.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#if CONFIG_EXAMPLE_BOARD_M5STICKC || CONFIG_EXAMPLE_BOARD_T_EMBED
#include "esp_netif_sntp.h"
#endif
#include "nvs_flash.h"
#include "soc/soc_caps.h"

static const char *TAG = "app_main";
static esp_openclaw_node_handle_t s_node;
static esp_openclaw_node_example_saved_session_reconnect_t s_saved_session_reconnect;

#if CONFIG_EXAMPLE_BOARD_M5STICKC || CONFIG_EXAMPLE_BOARD_T_EMBED
static void handle_time_sync(struct timeval *time)
{
    (void)time;
    ESP_LOGI(TAG, "clock synchronized for TLS certificate validation");
}

static void handle_wifi_ip(void *context, esp_event_base_t base, int32_t id, void *data)
{
    (void)context; (void)base; (void)id; (void)data;
    esp_err_t err = esp_netif_sntp_start();
    if (err != ESP_OK) ESP_LOGW(TAG, "clock synchronization start failed: %s", esp_err_to_name(err));
}

static void start_clock_sync(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.start = false;
    config.wait_for_sync = false;
    config.sync_cb = handle_time_sync;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&config));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, handle_wifi_ip, NULL));
}
#endif

static void handle_node_event(
    esp_openclaw_node_handle_t node,
    esp_openclaw_node_event_t event,
    const void *event_data,
    void *user_ctx)
{
    (void)node;

    esp_openclaw_node_example_saved_session_reconnect_handle_event(
        (esp_openclaw_node_example_saved_session_reconnect_t *)user_ctx,
        event,
        event_data);

    if (event == ESP_OPENCLAW_NODE_EVENT_CONNECTED) {
        compact_board_set_connected(true);
        ESP_LOGI(TAG, "OpenClaw session connected");
    } else if (event == ESP_OPENCLAW_NODE_EVENT_CONNECT_FAILED) {
        compact_board_set_connected(false);
        const esp_openclaw_node_connect_failed_event_t *failed = event_data;
        ESP_LOGW(
            TAG,
            "OpenClaw connect failed: reason=%d local_err=%s gateway_detail=%s",
            failed != NULL ? failed->reason : -1,
            failed != NULL ? esp_err_to_name(failed->local_err) : "n/a",
            failed != NULL && failed->gateway_detail_code != NULL ? failed->gateway_detail_code : "");
    } else if (event == ESP_OPENCLAW_NODE_EVENT_DISCONNECTED) {
        compact_board_set_connected(false);
        const esp_openclaw_node_disconnected_event_t *disconnected = event_data;
        ESP_LOGW(
            TAG,
            "OpenClaw disconnected: reason=%d local_err=%s",
            disconnected != NULL ? disconnected->reason : -1,
            disconnected != NULL ? esp_err_to_name(disconnected->local_err) : "n/a");
    }
}

#if !SOC_WIFI_SUPPORTED
#error "examples/esp32-node is a Wi-Fi-specific example and requires a Wi-Fi-capable ESP32 target"
#endif

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_EXAMPLE_BOARD_M5STICKC || CONFIG_EXAMPLE_BOARD_T_EMBED
    start_clock_sync();
#endif
    ESP_ERROR_CHECK(esp_openclaw_node_wifi_start());
    ESP_ERROR_CHECK(compact_board_start());

    esp_openclaw_node_config_t node_config = {0};
    esp_openclaw_node_config_init_default(&node_config);
#if CONFIG_EXAMPLE_BOARD_M5STICKC || CONFIG_EXAMPLE_BOARD_T_EMBED
    if (CONFIG_EXAMPLE_GATEWAY_TLS_SERVER_NAME[0] != '\0') {
        node_config.tls_common_name = CONFIG_EXAMPLE_GATEWAY_TLS_SERVER_NAME;
    }
#endif
#if CONFIG_EXAMPLE_BOARD_M5STICKC
    node_config.display_name = "M5StickC";
    node_config.model_identifier = "m5stack-m5stickc";
#elif CONFIG_EXAMPLE_BOARD_T_EMBED
    node_config.display_name = "T-Embed";
    node_config.model_identifier = "lilygo-t-embed";
#endif
    node_config.event_cb = handle_node_event;
    node_config.event_user_ctx = &s_saved_session_reconnect;
    ESP_ERROR_CHECK(esp_openclaw_node_create(&node_config, &s_node));
    ESP_ERROR_CHECK(esp_openclaw_node_example_register_node_commands(s_node));
    ESP_ERROR_CHECK(
        esp_openclaw_node_example_saved_session_reconnect_start(
            &s_saved_session_reconnect,
            s_node,
            "esp_openclaw_node_reconnect"));
    ESP_ERROR_CHECK(esp_openclaw_node_example_repl_start(s_node));

    if (!esp_openclaw_node_wifi_is_connected()) {
        ESP_LOGI(TAG, "wifi not connected yet; provision it from the REPL or wait for Wi-Fi to reconnect");
        ESP_LOGI(TAG, "after Wi-Fi is up, use `gateway setup-code`, `gateway token`, `gateway password`, or `gateway no-auth` for a first connect");
        ESP_LOGI(TAG, "saved-session reconnect runs automatically after Wi-Fi returns when a saved session is present");
    }
}
