/* The mobile OpenClaw mascot in its canonical 120 x 120 coordinate space. */
#include "room_lobster.h"

#include "watch_lobster_parts.h"

#define LOBSTER_SIZE 288
#define INK 0x050810

extern const uint8_t lobster_pixels[] asm("_binary_watch_lobster_argb8888_start");
static lv_obj_t *root, *parts[5], *eyes[2], *glows[2], *happy_eyes[2], *mouth, *mouth_o;
static lv_image_dsc_t images[5];
static lv_point_precise_t mouth_points[9], happy_points[2][9];

static int32_t unit(int32_t n) { return n * LOBSTER_SIZE / 120; }

static void bare(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

static void visible(lv_obj_t *obj, bool show)
{
    if (show) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void round_part(lv_obj_t *obj, uint32_t color)
{
    bare(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
}

esp_err_t room_lobster_create(lv_obj_t *parent)
{
    root = lv_obj_create(parent);
    if (root == NULL) return ESP_ERR_NO_MEM;
    bare(root);
    lv_obj_set_size(root, LOBSTER_SIZE, LOBSTER_SIZE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    for (int i = 0; i < 5; ++i) {
        images[i] = (lv_image_dsc_t){
            .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888,
                       .w = LOBSTER_PARTS[i].width, .h = LOBSTER_PARTS[i].height,
                       .stride = LOBSTER_PARTS[i].width * 4},
            .data_size = LOBSTER_PARTS[i].width * LOBSTER_PARTS[i].height * 4,
            .data = lobster_pixels + LOBSTER_PARTS[i].offset,
        };
        parts[i] = lv_image_create(root);
        if (parts[i] == NULL) return ESP_ERR_NO_MEM;
        bare(parts[i]);
        lv_image_set_src(parts[i], &images[i]);
        lv_obj_set_pos(parts[i], LOBSTER_PARTS[i].x, LOBSTER_PARTS[i].y);
    }
    static const int16_t pivot_x[] = {0, 26, 94, 38, 82};
    static const int16_t pivot_y[] = {0, 53, 53, 11, 11};
    for (int i = 1; i < 5; ++i) {
        lv_image_set_pivot(parts[i], unit(pivot_x[i]) - LOBSTER_PARTS[i].x,
                          unit(pivot_y[i]) - LOBSTER_PARTS[i].y);
    }
    for (int i = 0; i < 2; ++i) {
        eyes[i] = lv_obj_create(root);
        glows[i] = lv_obj_create(root);
        happy_eyes[i] = lv_line_create(root);
        if (eyes[i] == NULL || glows[i] == NULL || happy_eyes[i] == NULL) return ESP_ERR_NO_MEM;
        round_part(eyes[i], INK);
        round_part(glows[i], 0x00e5cc);
        bare(happy_eyes[i]);
        lv_obj_set_style_line_color(happy_eyes[i], lv_color_hex(INK), 0);
        lv_obj_set_style_line_width(happy_eyes[i], 6, 0);
        lv_obj_set_style_line_rounded(happy_eyes[i], true, 0);
    }
    mouth = lv_line_create(root);
    mouth_o = lv_obj_create(root);
    if (mouth == NULL || mouth_o == NULL) return ESP_ERR_NO_MEM;
    bare(mouth);
    lv_obj_set_style_line_color(mouth, lv_color_hex(INK), 0);
    lv_obj_set_style_line_width(mouth, 5, 0);
    lv_obj_set_style_line_rounded(mouth, true, 0);
    round_part(mouth_o, INK);
    room_lobster_update(&(room_lobster_pose_t){.eye_open_pct = 100, .eye_width_pct = 100});
    return ESP_OK;
}

void room_lobster_update(const room_lobster_pose_t *pose)
{
    /* Integer sine, no per-frame heap allocation or image decoding. */
    int32_t wave = lv_trigo_sin((pose->time_ms % 4000) * 360 / 4000);
    int32_t bob = wave * (pose->attentive ? 5 : 3) / 32768;
    lv_obj_t *parent = lv_obj_get_parent(root);
    lv_obj_set_pos(root, (lv_obj_get_width(parent) - LOBSTER_SIZE) / 2,
                   (lv_obj_get_height(parent) - LOBSTER_SIZE) / 2 + bob);
    int32_t snap_phase = pose->time_ms % 4000;
    int32_t left_snap = snap_phase > 3400 && snap_phase < 3800
        ? lv_trigo_sin((snap_phase - 3400) * 180 / 400) * 80 / 32768 : 0;
    int32_t right_snap = snap_phase > 3600
        ? lv_trigo_sin((snap_phase - 3600) * 180 / 400) * 80 / 32768 : 0;
    int32_t lift = pose->claw_lift * 10 + (pose->attentive ? wave * 45 / 32768 : 0);
    lv_image_set_rotation(parts[1], lift - left_snap);
    lv_image_set_rotation(parts[2], -lift - right_snap);
    int32_t wiggle = lv_trigo_sin((pose->time_ms % 2000) * 360 / 2000) * 30 / 32768;
    int32_t droop = pose->smile < 0 ? -pose->smile : 0;
    lv_image_set_rotation(parts[3], wiggle - droop);
    lv_image_set_rotation(parts[4], wiggle + droop);

    int32_t open = LV_CLAMP(0, pose->eye_open_pct, 130);
    int32_t happy = LV_CLAMP(0, (pose->smile - 50) * 2, 100);
    for (int i = 0; i < 2; ++i) {
        int32_t x = unit(i == 0 ? 45 : 75) + pose->gaze_x / 5;
        int32_t y = unit(35) + pose->gaze_y / 5;
        int32_t w = unit(12) * pose->eye_width_pct / 100;
        int32_t h = LV_MAX(3, unit(12) * open / 100);
        lv_obj_set_size(eyes[i], w, h);
        lv_obj_set_pos(eyes[i], x - w / 2, y - h / 2);
        lv_obj_set_style_opa(eyes[i], LV_OPA_COVER * (100 - happy) / 100, 0);
        int32_t glow = unit(4) * LV_MIN(open, 100) / 100;
        lv_obj_set_size(glows[i], glow, glow);
        lv_obj_set_pos(glows[i], x + unit(1) - glow / 2, y - unit(1) - glow / 2);
        visible(glows[i], open > 30 && happy < 80);
        lv_obj_set_style_opa(glows[i], LV_OPA_COVER * (100 - happy) / 100, 0);
        for (int j = 0; j < 9; ++j) {
            happy_points[i][j] = (lv_point_precise_t){
                .x = j * unit(12) / 8, .y = unit(4) * (j - 4) * (j - 4) / 16};
        }
        lv_line_set_points(happy_eyes[i], happy_points[i], 9);
        lv_obj_set_pos(happy_eyes[i], x - unit(6), y - unit(2));
        lv_obj_set_style_opa(happy_eyes[i], LV_OPA_COVER * happy * LV_MIN(open * 2, 100) / 100, 0);
    }
    bool round = pose->mouth_open_pct > 4 || pose->speaking;
    visible(mouth_o, round);
    visible(mouth, !round && (pose->smile > 5 || pose->smile < -5));
    if (round) {
        int32_t opening = pose->mouth_open_pct > 4 ? pose->mouth_open_pct : pose->speech_level * 100 / 255;
        int32_t w = unit(pose->speaking ? 12 : 8);
        int32_t h = unit(2) + unit(10) * opening / 100;
        lv_obj_set_size(mouth_o, w, h);
        lv_obj_set_pos(mouth_o, unit(60) - w / 2, unit(51) - h / 2);
    } else {
        for (int i = 0; i < 9; ++i) {
            mouth_points[i] = (lv_point_precise_t){
                .x = i * unit(15) / 8,
                .y = unit(8) + unit(4) * pose->smile * i * (8 - i) / 1600};
        }
        lv_line_set_points(mouth, mouth_points, 9);
        lv_obj_set_pos(mouth, unit(60) - unit(15) / 2, unit(49) - unit(8));
    }
}
