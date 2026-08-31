#pragma once
#include "esp_err.h"
#include "esp_log.h"
#define ESP_RETURN_ON_ERROR(expr, tag, ...) do { \
    esp_err_t host_error = (expr); \
    if (host_error != ESP_OK) { ESP_LOGE(tag, __VA_ARGS__); return host_error; } \
} while (0)
