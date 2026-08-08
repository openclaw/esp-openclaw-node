#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool node_ready;
    bool operator_ready;
    bool media_ready;
    bool camera_active;
    bool diagnostics_open;
    char talk_phase[12];
    char ui_state[16];
    char ui_detail[48];
    char gateway[64];
    bool canvas_active;
    char canvas_kind[12];
    uint16_t canvas_components;
    uint8_t canvas_images;
    char display_name[48];
    char model_identifier[48];
    uint16_t display_width;
    uint16_t display_height;
    char afe_layout[16];
    uint8_t configured_volume;
    bool input_gain_configured;
    float configured_input_gain_db;
    bool wifi_connected;
    char wifi_ssid[33];
    int8_t wifi_rssi;
    char wifi_ip[16];
    uint32_t internal_heap_free;
    uint32_t internal_heap_largest;
    uint32_t psram_free;
    uint64_t uptime_seconds;
} room_runtime_diagnostics_snapshot_t;

void room_runtime_get_diagnostics(room_runtime_diagnostics_snapshot_t *snapshot);
esp_err_t room_runtime_request_test_tone(void);
