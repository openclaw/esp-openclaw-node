#include "room_watch_status.h"

#include "esp_openclaw_node_wifi.h"
#include "openclaw_status_bar.h"

static openclaw_status_bar_t status_bar;
static bool wifi_connected, gateway_connected, display_awake;
static lv_timer_t *status_timer;

static void refresh_status(lv_timer_t *timer)
{
    (void)timer;
    if (!display_awake || !lv_obj_is_visible(status_bar.root)) return;
    esp_openclaw_node_wifi_status_t wifi = {0};
    esp_openclaw_node_wifi_get_status(&wifi);
    const openclaw_status_facts_t status = {
        .wifi_connected = wifi_connected && wifi.connected,
        .signal_known = wifi.ap_info_valid,
        .rssi = wifi.rssi,
        .gateway_connected = gateway_connected,
    };
    openclaw_status_bar_set(&status_bar, &status);
}

void room_watch_status_create(lv_obj_t *parent)
{
    openclaw_status_bar_create(&status_bar, parent);
    lv_obj_set_pos(status_bar.root, 248, 10);
    lv_obj_set_hidden(status_bar.root, true);
    status_timer = lv_timer_create(refresh_status, 1000, NULL);
    if (!display_awake) lv_timer_pause(status_timer);
}

void room_watch_status_set_awake(bool awake)
{
    if (display_awake == awake) return;
    display_awake = awake;
    if (status_timer == NULL) return;
    if (awake) {
        lv_timer_resume(status_timer);
        lv_timer_reset(status_timer);
        refresh_status(NULL);
    } else lv_timer_pause(status_timer);
}

void room_watch_status_render(const room_ui_facts_t *facts, bool hidden)
{
    if (status_bar.root == NULL) return;
    bool was_hidden = !lv_obj_is_visible(status_bar.root);
    lv_obj_set_hidden(status_bar.root, hidden);
    if (hidden) return;
    bool wifi = facts->wifi == ROOM_UI_WIFI_CONNECTED;
    bool gateway = facts->gateway == ROOM_UI_GATEWAY_CONNECTED;
    bool changed = wifi_connected != wifi || gateway_connected != gateway;
    wifi_connected = wifi;
    gateway_connected = gateway;
    if (was_hidden || changed) refresh_status(NULL);
}
