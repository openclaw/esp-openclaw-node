/*
 * Procedural agent face for the round AMOLED: two expressive eyes and a
 * speech-reactive mouth, drawn with plain LVGL objects so redraws stay small.
 * A 33 ms LVGL timer owns all geometry; everything the outside world can
 * change (state, mood, speech level) is written atomically and applied on the
 * next tick, so no caller needs anything beyond the display lock LVGL already
 * requires.
 */
#include "room_face.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "room_canvas.h"
#include "room_ui.h"

#define TAG "room_face"

#define FACE_WIDTH 410
#define FACE_HEIGHT 502
/* Eye centers sit above the display midline so the mouth clears the rounded
 * bottom corners; all coordinates assume the 32 px safe area. */
#define EYE_BASE_W 96
#define EYE_BASE_H 116
#define EYE_GAP 78
#define EYE_CENTER_Y 200
#define MOUTH_CENTER_Y 342
#define MOUTH_W 132
#define FACE_TICK_MS 33
/* Speech level decays to closed when the model pauses mid-utterance. */
#define SPEECH_LEVEL_STALE_US 300000

typedef enum {
    ROOM_FACE_MOOD_NEUTRAL = 0,
    ROOM_FACE_MOOD_HAPPY,
    ROOM_FACE_MOOD_EXCITED,
    ROOM_FACE_MOOD_THINKING,
    ROOM_FACE_MOOD_SLEEPY,
    ROOM_FACE_MOOD_SAD,
    ROOM_FACE_MOOD_COUNT,
} room_face_mood_t;

typedef struct {
    const char *name;
    uint32_t color;
    /* Percent of the base eye height/width the mood settles at. */
    uint8_t eye_h_pct;
    uint8_t eye_w_pct;
    int8_t eye_dy;
    /* Saccade cadence; excited darts, sleepy barely moves. */
    uint16_t saccade_min_ms;
    uint16_t saccade_max_ms;
} room_face_mood_style_t;

static const room_face_mood_style_t MOODS[ROOM_FACE_MOOD_COUNT] = {
    [ROOM_FACE_MOOD_NEUTRAL] = {"neutral", 0x53d7ff, 100, 100, 0, 1600, 3200},
    [ROOM_FACE_MOOD_HAPPY] = {"happy", 0xffd166, 46, 108, -10, 1400, 2800},
    [ROOM_FACE_MOOD_EXCITED] = {"excited", 0xff7a4d, 112, 104, -4, 500, 1200},
    [ROOM_FACE_MOOD_THINKING] = {"thinking", 0x9b8cff, 62, 96, -14, 2200, 3600},
    [ROOM_FACE_MOOD_SLEEPY] = {"sleepy", 0x62b8a8, 34, 100, 14, 3600, 5600},
    [ROOM_FACE_MOOD_SAD] = {"sad", 0x5f8bff, 52, 92, 16, 2600, 4200},
};

static lv_obj_t *face_root;
static lv_obj_t *eye_left;
static lv_obj_t *eye_right;
static lv_obj_t *mouth;
static lv_timer_t *face_timer;

/* Cross-task inputs, applied by the timer tick. */
static volatile room_face_state_t face_state;
/* Hint lifetime is owned by the face tick (one timeline owner under the LVGL
 * lock); zero while no hint is armed. */
static int64_t hint_until_us;
static volatile uint8_t face_mood_index;
static volatile uint8_t speech_level;
/* 32-bit millisecond stamp: aligned 32-bit stores are atomic on Xtensa, so
 * the audio task can write it lock-free; unsigned math survives wraparound. */
static volatile uint32_t speech_level_at_ms;
static volatile int64_t mood_hold_until_us;

/* Timer-task-only animation state. */
static int64_t next_blink_us;
static int64_t blink_started_us;
static int64_t next_saccade_us;
static int32_t saccade_x, saccade_y;
static int32_t eased_dx, eased_dy;
static int32_t mouth_height_q8;
static uint32_t tick_count;

static uint32_t face_rand(uint32_t bound)
{
    /* xorshift keeps blinks/saccades unsynchronized without libc rand state. */
    static uint32_t state = 0x9d2c5680;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state % bound;
}

static void schedule_blink(int64_t now_us)
{
    next_blink_us = now_us + 2200000 + (int64_t)face_rand(2600) * 1000;
}

static void schedule_saccade(int64_t now_us, const room_face_mood_style_t *mood)
{
    uint32_t span = (uint32_t)(mood->saccade_max_ms - mood->saccade_min_ms);
    next_saccade_us = now_us + (int64_t)mood->saccade_min_ms * 1000 +
        (int64_t)face_rand(span) * 1000;
    saccade_x = (int32_t)face_rand(29) - 14;
    saccade_y = (int32_t)face_rand(17) - 8;
}

static void face_hint_refresh(void *arg)
{
    (void)arg;
    /* A face.set may have re-armed the hint between expiry and this callback
     * (both run under the LVGL lock); the repaint belongs only to a hint that
     * is still expired. */
    if (face_state == ROOM_FACE_HINT && hint_until_us != 0) {
        return;
    }
    /* Repaint the stored UI state: idle hides the face and darkens the panel. */
    room_ui_refresh();
}

static void face_tick(lv_timer_t *timer)
{
    (void)timer;
    room_face_state_t state = face_state;
    if (state == ROOM_FACE_HIDDEN) {
        return;
    }
    int64_t now_us = esp_timer_get_time();
    ++tick_count;
    if (state == ROOM_FACE_HINT && hint_until_us != 0 && now_us >= hint_until_us) {
        /* Expire the hint from the tick itself: it runs under the LVGL lock,
         * retries every frame, and a re-armed deadline simply moves it. Keep
         * the deadline armed if the async alloc fails so the next tick retries
         * instead of leaving the panel lit forever. */
        if (lv_async_call(face_hint_refresh, NULL) == LV_RESULT_OK) {
            hint_until_us = 0;
        }
        return;
    }

    room_face_mood_t mood_index = (room_face_mood_t)face_mood_index;
    if (mood_hold_until_us != 0 && now_us > mood_hold_until_us) {
        mood_hold_until_us = 0;
        face_mood_index = ROOM_FACE_MOOD_NEUTRAL;
        mood_index = ROOM_FACE_MOOD_NEUTRAL;
    }
    /* Thinking is a state-derived look unless the agent pinned a mood. */
    if (state == ROOM_FACE_THINKING && mood_index == ROOM_FACE_MOOD_NEUTRAL) {
        mood_index = ROOM_FACE_MOOD_THINKING;
    }
    /* The hint renders exactly like listening. */
    if (state == ROOM_FACE_HINT) {
        state = ROOM_FACE_LISTENING;
    }
    const room_face_mood_style_t *mood = &MOODS[mood_index];

    /* Blink: 130 ms close-then-open envelope. */
    if (blink_started_us == 0 && now_us >= next_blink_us) {
        blink_started_us = now_us;
    }
    int32_t blink_pct = 100;
    if (blink_started_us != 0) {
        int64_t phase_us = now_us - blink_started_us;
        if (phase_us >= 130000) {
            blink_started_us = 0;
            schedule_blink(now_us);
        } else {
            int32_t p = (int32_t)(phase_us / 1000);
            blink_pct = p < 65 ? 100 - (p * 94) / 65 : 6 + ((p - 65) * 94) / 65;
        }
    }

    /* Saccades ease toward the target so motion reads organic, not stepped. */
    if (now_us >= next_saccade_us) {
        schedule_saccade(now_us, mood);
    }
    eased_dx += (saccade_x - eased_dx) / 4;
    eased_dy += (saccade_y - eased_dy) / 4;

    int32_t eye_w = (EYE_BASE_W * mood->eye_w_pct) / 100;
    int32_t eye_h = (EYE_BASE_H * mood->eye_h_pct) / 100;
    if (state == ROOM_FACE_LISTENING) {
        eye_h = (eye_h * 112) / 100;
        eye_w = (eye_w * 106) / 100;
    }
    eye_h = (eye_h * blink_pct) / 100;
    if (eye_h < 6) {
        eye_h = 6;
    }
    /* Gentle breathing bob while listening; thinking gazes up and away. */
    int32_t bob = 0;
    if (state == ROOM_FACE_LISTENING) {
        static const int8_t BOB[8] = {0, 2, 4, 2, 0, -2, -4, -2};
        bob = BOB[(tick_count / 6) % 8];
    }
    int32_t gaze_dx = state == ROOM_FACE_THINKING ? -12 : 0;
    int32_t gaze_dy = state == ROOM_FACE_THINKING ? -14 : 0;

    int32_t center_x = FACE_WIDTH / 2 + eased_dx + gaze_dx;
    int32_t center_y = EYE_CENTER_Y + mood->eye_dy + eased_dy + gaze_dy + bob;
    lv_color_t color = lv_color_hex(mood->color);
    lv_obj_set_style_bg_color(eye_left, color, 0);
    lv_obj_set_style_bg_color(eye_right, color, 0);
    lv_obj_set_style_bg_color(mouth, color, 0);
    lv_obj_set_size(eye_left, eye_w, eye_h);
    lv_obj_set_size(eye_right, eye_w, eye_h);
    lv_obj_set_pos(eye_left, center_x - EYE_GAP - eye_w / 2, center_y - eye_h / 2);
    lv_obj_set_pos(eye_right, center_x + EYE_GAP - eye_w / 2, center_y - eye_h / 2);

    if (state == ROOM_FACE_SPEAKING) {
        int32_t level = speech_level;
        uint32_t age_ms = (uint32_t)(now_us / 1000) - speech_level_at_ms;
        if (age_ms > SPEECH_LEVEL_STALE_US / 1000) {
            level = 0;
        }
        /* Fast attack, slow decay: mouths open quickly and close smoothly. */
        int32_t target_q8 = (8 + (level * 44) / 255) << 8;
        int32_t rate = target_q8 > mouth_height_q8 ? 2 : 6;
        mouth_height_q8 += (target_q8 - mouth_height_q8) / rate;
        int32_t mouth_h = mouth_height_q8 >> 8;
        lv_obj_clear_flag(mouth, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(mouth, MOUTH_W, mouth_h);
        lv_obj_set_pos(mouth, (FACE_WIDTH - MOUTH_W) / 2 + eased_dx / 2, MOUTH_CENTER_Y - mouth_h / 2);
    } else {
        lv_obj_add_flag(mouth, LV_OBJ_FLAG_HIDDEN);
        mouth_height_q8 = 8 << 8;
    }
}

esp_err_t room_face_create(lv_obj_t *parent)
{
    face_root = lv_obj_create(parent);
    if (face_root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(face_root);
    lv_obj_set_size(face_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(face_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(face_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(face_root, LV_OBJ_FLAG_SCROLLABLE);
    /* Taps must keep reaching the status screen's canvas toggle. */
    lv_obj_remove_flag(face_root, LV_OBJ_FLAG_CLICKABLE);

    eye_left = lv_obj_create(face_root);
    eye_right = lv_obj_create(face_root);
    mouth = lv_obj_create(face_root);
    if (eye_left == NULL || eye_right == NULL || mouth == NULL) {
        lv_obj_delete(face_root);
        face_root = NULL;
        return ESP_ERR_NO_MEM;
    }
    lv_obj_t *parts[] = {eye_left, eye_right, mouth};
    for (size_t i = 0; i < 3; ++i) {
        lv_obj_remove_style_all(parts[i]);
        lv_obj_set_style_bg_opa(parts[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(parts[i], 26, 0);
        lv_obj_remove_flag(parts[i], LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_set_style_radius(mouth, 12, 0);
    lv_obj_add_flag(face_root, LV_OBJ_FLAG_HIDDEN);

    face_timer = lv_timer_create(face_tick, FACE_TICK_MS, NULL);
    if (face_timer == NULL) {
        lv_obj_delete(face_root);
        face_root = NULL;
        return ESP_ERR_NO_MEM;
    }
    lv_timer_pause(face_timer);
    return ESP_OK;
}

void room_face_show(room_face_state_t state)
{
    if (face_root == NULL || state == ROOM_FACE_HIDDEN) {
        return;
    }
    face_state = state;
    int64_t now_us = esp_timer_get_time();
    if (next_blink_us == 0) {
        schedule_blink(now_us);
        schedule_saccade(now_us, &MOODS[ROOM_FACE_MOOD_NEUTRAL]);
    }
    lv_obj_clear_flag(face_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(face_root);
    lv_timer_resume(face_timer);
}

void room_face_show_hint(int64_t until_us)
{
    room_face_show(ROOM_FACE_HINT);
    if (face_state == ROOM_FACE_HINT) {
        hint_until_us = until_us;
    }
}

void room_face_hide(void)
{
    if (face_root == NULL) {
        return;
    }
    face_state = ROOM_FACE_HIDDEN;
    hint_until_us = 0;
    lv_obj_add_flag(face_root, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(face_timer);
}

bool room_face_is_visible(void)
{
    return face_state != ROOM_FACE_HIDDEN;
}

void room_face_set_speech_level(uint8_t level)
{
    speech_level = level;
    speech_level_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

void room_face_reset_mood(void)
{
    face_mood_index = ROOM_FACE_MOOD_NEUTRAL;
    mood_hold_until_us = 0;
}

static esp_err_t handle_face_set(
    esp_openclaw_node_handle_t node,
    void *context,
    const char *params_json,
    size_t params_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node;
    (void)context;
    cJSON *params = cJSON_ParseWithLength(params_json, params_len);
    if (!cJSON_IsObject(params)) {
        cJSON_Delete(params);
        out_error->code = "INVALID_PARAMS";
        out_error->message = "face.set params must be a JSON object";
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *mood = cJSON_GetObjectItemCaseSensitive(params, "mood");
    cJSON *hold = cJSON_GetObjectItemCaseSensitive(params, "holdMs");
    const char *mood_text = cJSON_IsString(mood) ? mood->valuestring : NULL;
    int mood_index = -1;
    for (int i = 0; mood_text != NULL && i < ROOM_FACE_MOOD_COUNT; ++i) {
        if (strcmp(mood_text, MOODS[i].name) == 0) {
            mood_index = i;
            break;
        }
    }
    bool hold_valid = hold == NULL ||
        (cJSON_IsNumber(hold) && hold->valuedouble >= 0 && hold->valuedouble <= 600000);
    if (mood_index < 0 || !hold_valid) {
        cJSON_Delete(params);
        out_error->code = "INVALID_PARAMS";
        out_error->message =
            "face.set requires mood neutral|happy|excited|thinking|sleepy|sad and optional holdMs 0..600000";
        return ESP_ERR_INVALID_ARG;
    }
    int64_t hold_ms = hold != NULL ? (int64_t)hold->valuedouble : 0;
    cJSON_Delete(params);

    /* One display-lock hold makes routing atomic against talk-state
     * transitions, which repaint under the same lock. The lock is recursive,
     * so the hint path below may take it again. */
    if (!bsp_display_lock(200)) {
        out_error->code = "UNAVAILABLE";
        out_error->message = "the display is busy; retry face.set";
        return ESP_ERR_TIMEOUT;
    }
    bool talk_active = room_ui_talk_face_active();
    bool canvas_covering = room_canvas_is_active();
    if (talk_active) {
        /* Mood rides the (possibly canvas-covered) talk face; hold 0 keeps it
         * until the call ends. */
        face_mood_index = (uint8_t)mood_index;
        mood_hold_until_us =
            hold_ms > 0 ? esp_timer_get_time() + hold_ms * 1000 : 0;
    } else {
        /* No talk session: the mood always times out. Without canvas the face
         * pops up on its own so the expression has a visible outcome; under
         * canvas it is stored only, applying if the face shows in time. */
        int64_t show_ms = hold_ms > 0 ? hold_ms : 8000;
        face_mood_index = (uint8_t)mood_index;
        mood_hold_until_us = esp_timer_get_time() + show_ms * 1000;
        if (!canvas_covering) {
            room_ui_show_face_hint((uint32_t)show_ms);
        }
    }
    bool visible = room_face_is_visible();
    bsp_display_unlock();

    char payload[96];
    snprintf(
        payload,
        sizeof(payload),
        "{\"mood\":\"%s\",\"holdMs\":%lld,\"visible\":%s}",
        MOODS[mood_index].name,
        (long long)hold_ms,
        visible ? "true" : "false");
    *out_payload_json = strdup(payload);
    if (*out_payload_json == NULL) {
        out_error->code = "INTERNAL";
        out_error->message = "not enough memory for the command result";
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t room_face_register_node_commands(esp_openclaw_node_handle_t node)
{
    const esp_openclaw_node_command_t command = {
        .name = "face.set",
        .handler = handle_face_set,
    };
    return esp_openclaw_node_register_command(node, &command);
}
