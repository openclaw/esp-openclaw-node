#pragma once
#include <stdint.h>
#include "esp_err.h"
#define ESP_EVENT_DECLARE_BASE(name) extern const char *name
typedef const char *esp_event_base_t;
#define ESP_EVENT_ANY_ID -1
ESP_EVENT_DECLARE_BASE(IP_EVENT);
enum { IP_EVENT_STA_GOT_IP, IP_EVENT_STA_LOST_IP };
esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id,
    void (*handler)(void *, esp_event_base_t, int32_t, void *), void *ctx);
esp_err_t esp_event_loop_create_default(void);
