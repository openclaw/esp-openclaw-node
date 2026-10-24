#pragma once

#include "esp_err.h"
#include "lvgl.h"

/* Render inputs from the existing face lifecycle and tween rig. */
typedef struct {
    uint32_t time_ms;
    int16_t eye_open_pct;
    int16_t eye_width_pct;
    int16_t gaze_x;
    int16_t gaze_y;
    int16_t smile;
    int16_t mouth_open_pct;
    int16_t claw_lift;
    uint8_t speech_level;
    bool speaking;
    bool attentive;
} room_lobster_pose_t;

esp_err_t room_lobster_create(lv_obj_t *parent);
void room_lobster_update(const room_lobster_pose_t *pose);
