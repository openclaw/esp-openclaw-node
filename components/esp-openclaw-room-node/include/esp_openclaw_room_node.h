/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_codec_dev.h"
#include "esp_err.h"
#include "esp_openclaw_node.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lv_display_t *(*start)(void *ctx);
    esp_err_t (*setup_local_input)(void *ctx, void (*toggle_view)(void));
    bool (*lock)(void *ctx, uint32_t timeout_ms);
    void (*unlock)(void *ctx);
    esp_err_t (*set_brightness)(void *ctx, int percent);
    uint16_t native_width;
    uint16_t native_height;
    uint16_t safe_inset;
    bool animated_face; /**< Enable the procedural face when the display pipeline can sustain it. */
    uint16_t animation_frame_ms; /**< Sustainable face cadence for this display pipeline; 0 uses 16 ms. */
    void *ctx;
} esp_openclaw_room_display_port_t;

typedef struct {
    esp_codec_dev_handle_t record;
    esp_codec_dev_handle_t playback;
} esp_openclaw_room_audio_handles_t;

typedef struct {
    esp_err_t (*open)(void *ctx, esp_openclaw_room_audio_handles_t *handles);
    const char *afe_layout;
    uint8_t record_channels;
    uint8_t channel_mask;
    uint8_t playback_volume;
    bool configure_input_gain; /**< Apply input_gain_db instead of preserving the codec/board default. */
    float input_gain_db;
    void *ctx;
} esp_openclaw_room_audio_port_t;

typedef struct {
    esp_err_t (*prepare_runtime)(void *ctx);
    esp_err_t (*prepare_network)(void *ctx);
    esp_err_t (*register_commands)(void *ctx, esp_openclaw_node_handle_t node);
    void *ctx;
} esp_openclaw_room_services_port_t;

/** Optional board-owned file namespace. Omit it to advertise no file commands. */
typedef struct {
    const char *public_root;
    bool (*is_available)(void *ctx);
    esp_err_t (*get_metrics)(void *ctx, uint64_t *total_bytes, uint64_t *free_bytes);
    void *ctx;
} esp_openclaw_room_storage_port_t;

typedef struct {
    const char *display_name;
    const char *model_identifier;
    esp_openclaw_room_display_port_t display;
    esp_openclaw_room_audio_port_t audio;
    esp_openclaw_room_services_port_t services;
    esp_openclaw_room_storage_port_t storage;
} esp_openclaw_room_node_config_t;

/** Start the canonical room-node product. This call owns the product lifecycle. */
esp_err_t esp_openclaw_room_node_start(const esp_openclaw_room_node_config_t *config);

/** Board camera adapters use this to serialize capture against Talk media. */
bool esp_openclaw_room_node_try_acquire_camera(void);
void esp_openclaw_room_node_release_camera(void);

/** Arm/disarm the shared, visibly flushed camera privacy indicator. */
esp_err_t esp_openclaw_room_node_camera_indicator_begin(void);
void esp_openclaw_room_node_camera_indicator_end(void);

#ifdef __cplusplus
}
#endif
