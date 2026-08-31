#pragma once
#include "esp_err.h"
#define ESP_EVENT_DECLARE_BASE(name) extern const char *name
esp_err_t esp_event_loop_create_default(void);
