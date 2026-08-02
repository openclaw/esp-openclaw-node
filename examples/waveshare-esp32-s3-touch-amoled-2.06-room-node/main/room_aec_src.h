#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_capture_audio_src_if.h"

typedef struct {
    const char *mic_layout;
    void *record_handle;
    uint8_t channel;
    uint8_t channel_mask;
    bool data_on_vad;
    void (*wake_cb)(void *ctx);
    void *wake_ctx;
} room_capture_audio_aec_src_cfg_t;

esp_capture_audio_src_if_t *room_capture_new_audio_aec_src(
    room_capture_audio_aec_src_cfg_t *cfg);
