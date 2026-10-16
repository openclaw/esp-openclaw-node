#pragma once
#include "esp_event.h"
typedef struct wifi_ap_record_t wifi_ap_record_t;
ESP_EVENT_DECLARE_BASE(WIFI_EVENT);
enum { WIFI_EVENT_STA_START, WIFI_EVENT_STA_STOP, WIFI_EVENT_STA_CONNECTED, WIFI_EVENT_STA_DISCONNECTED };
