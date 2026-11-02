#include "openclaw_preferences.h"

#include <stdint.h>
#include "nvs.h"

static const char *const keys[] = {"reading", "density", "brightness"};
static esp_err_t errors[OPENCLAW_PREFERENCE_COUNT];

static bool valid(openclaw_preference_t preference, int value)
{
    switch (preference) {
    case OPENCLAW_PREFERENCE_READING:
    case OPENCLAW_PREFERENCE_DENSITY: return value >= 0 && value <= 1;
    case OPENCLAW_PREFERENCE_BRIGHTNESS: return value >= 10 && value <= 100;
    default: return false;
    }
}

int openclaw_preferences_load(openclaw_preference_t preference, int fallback)
{
    if (!valid(preference, fallback) &&
        !(preference == OPENCLAW_PREFERENCE_BRIGHTNESS && fallback == -1)) return fallback;
    nvs_handle_t handle;
    uint8_t value = 0;
    esp_err_t err = nvs_open("openclaw_ui", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_u8(handle, keys[preference], &value);
        nvs_close(handle);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        errors[preference] = ESP_OK;
        return fallback;
    }
    if (err == ESP_OK && !valid(preference, value)) err = ESP_ERR_INVALID_ARG;
    errors[preference] = err;
    return err == ESP_OK ? value : fallback;
}

esp_err_t openclaw_preferences_save(openclaw_preference_t preference, int value)
{
    if (!valid(preference, value)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("openclaw_ui", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, keys[preference], (uint8_t)value);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    errors[preference] = err;
    return err;
}

bool openclaw_preferences_saved(openclaw_preference_t preference)
{
    return preference >= 0 && preference < OPENCLAW_PREFERENCE_COUNT && errors[preference] == ESP_OK;
}
