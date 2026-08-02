#pragma once

#include "esp_err.h"
#include "esp_webrtc.h"

typedef void (*room_wake_callback_t)(const char *wake_word, void *ctx);

esp_err_t room_media_init(room_wake_callback_t callback, void *ctx);
/** Enable or disable the ambient wake scan path (disable while Talk owns the pipeline). */
esp_err_t room_media_set_ambient_wake(bool enabled);

esp_err_t room_media_get_webrtc_provider(esp_webrtc_media_provider_t *provider);
