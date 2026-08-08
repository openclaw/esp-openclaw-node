/*
 * Procedural agent face for the round AMOLED, built on a tween rig: every
 * visible parameter (eye geometry, gaze, tilt, eyelids, color, mouth) carries
 * a current value and an eased target, so mood and state changes glide
 * instead of snapping. Layered on top: a blink choreographer (asymmetric
 * close/open, double blinks, cat-style slow blinks, saccade masking), gaze
 * with anticipation and overshoot, breathing, speech-reactive mouth bars, and
 * a small authored keyframe gesture table (surprise, yawn, nod, shake).
 *
 * Concurrency model: the tick runs under the LVGL port mutex; every outside
 * writer (commands, gestures, show/hide) holds the same recursive display
 * lock, so all animation state is single-lock. The speech level alone is
 * written lock-free from the audio task as aligned 32-bit values.
 */
#include "room_face.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "room_board.h"

#define TAG "room_face"

/* Geometry is resolved from the active LVGL display. */
static int32_t face_width;
static int32_t face_height;
static int32_t eye_base_w;
static int32_t eye_base_h;
static int32_t eye_gap;
static int32_t eye_center_y;
static int32_t mouth_center_y;
#define FACE_DEFAULT_TICK_MS 16
static uint16_t face_tick_ms = FACE_DEFAULT_TICK_MS;
/* Speech level decays to closed when the model pauses mid-utterance. */
#define SPEECH_STALE_MS 300

/* ---------------------------------------------------------------- tweens */

typedef enum {
    FACE_EASE_LINEAR = 0,
    FACE_EASE_OUT_CUBIC,
    /* Anticipation dip, ~9% overshoot, settle: the "organic motion" curve. */
    FACE_EASE_OUT_BACK,
    /* Opens past target then settles: blink-open bounce. */
    FACE_EASE_OUT_BOUNCE,
    FACE_EASE_IN_QUAD,
} face_ease_t;

typedef struct {
    int32_t from;
    int32_t to;
    int32_t value;
    uint16_t duration_ms;
    uint16_t elapsed_ms;
    uint8_t ease;
} face_tween_t;

/* Piecewise curves; p and the result are Q10 (0..1024). Segment corners are
 * softened by the 60 Hz tick rate, and the shape (dip/overshoot/settle) is
 * what the eye actually reads. */
static int32_t ease_apply(uint8_t ease, int32_t p)
{
    switch (ease) {
    case FACE_EASE_OUT_CUBIC: {
        int32_t inv = 1024 - p;
        return 1024 - (int32_t)(((int64_t)inv * inv * inv) >> 20);
    }
    case FACE_EASE_OUT_BACK:
        if (p < 150) {
            return -(p * 45) / 150;
        }
        if (p < 650) {
            return -45 + ((p - 150) * (1120 + 45)) / 500;
        }
        return 1120 - ((p - 650) * (1120 - 1024)) / 374;
    case FACE_EASE_OUT_BOUNCE:
        if (p < 800) {
            int32_t inv = 1024 - (p * 1024) / 800;
            return 1078 - (int32_t)(((int64_t)inv * inv * 1078) >> 20);
        }
        return 1078 - ((p - 800) * (1078 - 1024)) / 224;
    case FACE_EASE_IN_QUAD:
        return (int32_t)(((int64_t)p * p) >> 10);
    default:
        return p;
    }
}

static void tween_go(face_tween_t *t, int32_t to, uint16_t duration_ms, uint8_t ease)
{
    if (t->to == to && t->elapsed_ms < t->duration_ms) {
        return;
    }
    t->from = t->value;
    t->to = to;
    t->duration_ms = duration_ms > 0 ? duration_ms : 1;
    t->elapsed_ms = 0;
    t->ease = ease;
}

static void tween_jump(face_tween_t *t, int32_t to)
{
    t->from = to;
    t->to = to;
    t->value = to;
    t->elapsed_ms = t->duration_ms = 1;
}

static void tween_step(face_tween_t *t, uint16_t dt_ms)
{
    if (t->elapsed_ms >= t->duration_ms) {
        t->value = t->to;
        return;
    }
    t->elapsed_ms = (uint16_t)(t->elapsed_ms + dt_ms);
    if (t->elapsed_ms >= t->duration_ms) {
        t->value = t->to;
        return;
    }
    int32_t p = ((int32_t)t->elapsed_ms * 1024) / t->duration_ms;
    int32_t eased = ease_apply(t->ease, p);
    t->value = t->from + (int32_t)(((int64_t)(t->to - t->from) * eased) >> 10);
}

/* ------------------------------------------------------------------ moods */

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
    uint8_t eye_h_pct;
    uint8_t eye_w_pct;
    int8_t eye_dy;
    /* Signed outer-corner lift in 0.1 deg: positive reads delighted, negative
     * reads worried — the whole eyebrow-less emotional vocabulary. */
    int16_t tilt_ddeg;
    uint8_t lid_pct;
    /* Resting mouth curve: positive smiles, negative frowns, 0 flat. */
    int8_t smile;
    uint16_t saccade_min_ms;
    uint16_t saccade_max_ms;
    /* Chance (percent) a scheduled blink becomes a slow, held cat blink. */
    uint8_t slow_blink_pct;
} room_face_mood_style_t;

static const room_face_mood_style_t MOODS[ROOM_FACE_MOOD_COUNT] = {
    [ROOM_FACE_MOOD_NEUTRAL] = {"neutral", 0x53d7ff, 100, 100, 0, 0, 0, 24, 1600, 3200, 4},
    [ROOM_FACE_MOOD_HAPPY] = {"happy", 0xffd166, 72, 106, -8, 34, 0, 96, 1400, 2800, 30},
    [ROOM_FACE_MOOD_EXCITED] = {"excited", 0xff7a4d, 114, 96, -4, 16, 0, 80, 500, 1100, 0},
    [ROOM_FACE_MOOD_THINKING] = {"thinking", 0x9b8cff, 80, 96, -12, -10, 12, 8, 2200, 3600, 0},
    [ROOM_FACE_MOOD_SLEEPY] = {"sleepy", 0x62b8a8, 70, 102, 12, 0, 45, 16, 3600, 5600, 45},
    [ROOM_FACE_MOOD_SAD] = {"sad", 0x5f8bff, 78, 94, 14, -34, 18, -72, 2600, 4200, 0},
};

/* --------------------------------------------------------------- gestures */

#define FACE_KF_KEEP INT16_MIN
/* A keyframe overrides rig targets; FACE_KF_KEEP leaves that channel on its
 * mood baseline. `restore` retargets everything back over duration_ms. */
typedef struct {
    uint16_t duration_ms;
    uint8_t ease;
    bool restore;
    int16_t eye_h_pct;
    int16_t eye_w_pct;
    int16_t eye_dy;
    int16_t tilt_ddeg;
    int16_t gaze_x;
    /* 0 hides the "o" mouth; >0 shows a tall oval that percent of max. */
    int16_t mouth_o_pct;
} face_keyframe_t;

static const face_keyframe_t GESTURE_SURPRISE[] = {
    {90, FACE_EASE_OUT_CUBIC, false, 132, 110, -6, 0, FACE_KF_KEEP, 0},
    {140, FACE_EASE_LINEAR, false, 132, 110, -6, 0, FACE_KF_KEEP, 0},
    {320, FACE_EASE_OUT_BACK, true, 0, 0, 0, 0, 0, 0},
};
static const face_keyframe_t GESTURE_YAWN[] = {
    {380, FACE_EASE_OUT_CUBIC, false, 26, 108, 8, 0, FACE_KF_KEEP, 88},
    {260, FACE_EASE_LINEAR, false, 26, 108, 8, 0, FACE_KF_KEEP, 88},
    {420, FACE_EASE_OUT_CUBIC, true, 0, 0, 0, 0, 0, 0},
};
static const face_keyframe_t GESTURE_NOD[] = {
    {150, FACE_EASE_OUT_CUBIC, false, 92, 100, 16, FACE_KF_KEEP, FACE_KF_KEEP, 0},
    {140, FACE_EASE_OUT_CUBIC, false, 100, 100, -4, FACE_KF_KEEP, FACE_KF_KEEP, 0},
    {150, FACE_EASE_OUT_CUBIC, false, 92, 100, 16, FACE_KF_KEEP, FACE_KF_KEEP, 0},
    {200, FACE_EASE_OUT_BACK, true, 0, 0, 0, 0, 0, 0},
};
static const face_keyframe_t GESTURE_SHAKE[] = {
    {120, FACE_EASE_OUT_CUBIC, false, FACE_KF_KEEP, FACE_KF_KEEP, FACE_KF_KEEP, FACE_KF_KEEP, -14, 0},
    {130, FACE_EASE_OUT_CUBIC, false, FACE_KF_KEEP, FACE_KF_KEEP, FACE_KF_KEEP, FACE_KF_KEEP, 14, 0},
    {120, FACE_EASE_OUT_CUBIC, false, FACE_KF_KEEP, FACE_KF_KEEP, FACE_KF_KEEP, FACE_KF_KEEP, -8, 0},
    {180, FACE_EASE_OUT_BACK, true, 0, 0, 0, 0, 0, 0},
};

typedef struct {
    const face_keyframe_t *frames;
    uint8_t count;
    const char *name;
} face_gesture_def_t;

static const face_gesture_def_t GESTURES[] = {
    [ROOM_FACE_GESTURE_SURPRISE] = {GESTURE_SURPRISE, 3, "surprise"},
    [ROOM_FACE_GESTURE_YAWN] = {GESTURE_YAWN, 3, "yawn"},
    [ROOM_FACE_GESTURE_NOD] = {GESTURE_NOD, 4, "nod"},
    [ROOM_FACE_GESTURE_SHAKE] = {GESTURE_SHAKE, 4, "shake"},
};

/* -------------------------------------------------------------- rig state */

static lv_obj_t *face_root;
static lv_obj_t *eye_left;
static lv_obj_t *eye_right;
static lv_obj_t *lid_left;
static lv_obj_t *lid_right;
static lv_obj_t *mouth_arc;
static lv_obj_t *mouth_o;
static lv_obj_t *bars[3];
static lv_timer_t *face_timer;

static volatile room_face_state_t face_state;
static room_face_controller_t controller;

void room_face_set_controller(const room_face_controller_t *value)
{
    controller = value != NULL ? *value : (room_face_controller_t){0};
}
static int64_t hint_until_us;
static bool hint_yawned;
static int64_t hint_shown_at_us;
static volatile uint8_t face_mood_index;
static volatile int64_t mood_hold_until_us;

/* Lock-free audio inputs (aligned 32-bit stores are atomic on Xtensa). */
static volatile uint8_t speech_level;
static volatile uint32_t speech_level_at_ms;

/* All tweens step under the LVGL lock. */
static face_tween_t tw_eye_h;   /* percent of EYE_BASE_H */
static face_tween_t tw_eye_w;   /* percent of EYE_BASE_W */
static face_tween_t tw_eye_dy;  /* px */
static face_tween_t tw_tilt;    /* 0.1 deg, +outer-up */
static face_tween_t tw_lid;     /* percent of eye height covered */
static face_tween_t tw_gaze_x;  /* px */
static face_tween_t tw_gaze_y;  /* px */
static face_tween_t tw_smile;   /* -100..100 */
static face_tween_t tw_r, tw_g, tw_b;
static face_tween_t tw_mouth_o; /* percent, 0 hidden */
static face_tween_t tw_sparkle; /* extra eye percent while speech peaks */

/* Blink choreography. */
typedef enum { BLINK_IDLE = 0, BLINK_CLOSING, BLINK_HOLD, BLINK_OPENING } blink_phase_t;
static blink_phase_t blink_phase;
static uint16_t blink_elapsed_ms;
static bool blink_slow;
static bool blink_double_pending;
static int64_t next_blink_us;
static int64_t last_blink_us;
static int32_t blink_q10 = 1024; /* 1024 open .. 0 closed; may settle >1024 */

/* Gaze scheduling. */
static int64_t next_saccade_us;

/* Gesture playback. */
static const face_gesture_def_t *active_gesture;
static uint8_t gesture_frame;
static bool gesture_pending_advance;

static uint32_t tick_count;
static int64_t last_tick_us;

static uint32_t face_rand(uint32_t bound)
{
    static uint32_t rng = 0x9d2c5680;
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng % bound;
}

static const room_face_mood_style_t *current_mood(room_face_state_t state)
{
    room_face_mood_t index = (room_face_mood_t)face_mood_index;
    if (state == ROOM_FACE_THINKING && index == ROOM_FACE_MOOD_NEUTRAL) {
        return &MOODS[ROOM_FACE_MOOD_THINKING];
    }
    return &MOODS[index];
}

/* Retarget the rig for the current state+mood; gestures suspend this. */
static void face_retarget(uint16_t duration_ms, uint8_t ease)
{
    room_face_state_t state = face_state;
    const room_face_mood_style_t *mood = current_mood(state);
    int32_t eye_h = mood->eye_h_pct;
    int32_t eye_w = mood->eye_w_pct;
    if (state == ROOM_FACE_LISTENING || state == ROOM_FACE_HINT) {
        eye_h = (eye_h * 110) / 100;
        eye_w = (eye_w * 104) / 100;
    }
    tween_go(&tw_eye_h, eye_h, duration_ms, ease);
    tween_go(&tw_eye_w, eye_w, duration_ms, ease);
    tween_go(&tw_eye_dy, mood->eye_dy, duration_ms, ease);
    tween_go(&tw_tilt, mood->tilt_ddeg, duration_ms, ease);
    tween_go(&tw_lid, mood->lid_pct, duration_ms, ease);
    tween_go(&tw_smile, mood->smile, duration_ms, ease);
    tween_go(&tw_mouth_o, 0, duration_ms, FACE_EASE_OUT_CUBIC);
    lv_color_t c = lv_color_hex(mood->color);
    tween_go(&tw_r, c.red, duration_ms, FACE_EASE_OUT_CUBIC);
    tween_go(&tw_g, c.green, duration_ms, FACE_EASE_OUT_CUBIC);
    tween_go(&tw_b, c.blue, duration_ms, FACE_EASE_OUT_CUBIC);
}

static void gesture_apply_frame(void)
{
    const face_keyframe_t *kf = &active_gesture->frames[gesture_frame];
    if (kf->restore) {
        face_retarget(kf->duration_ms, kf->ease);
        return;
    }
    const room_face_mood_style_t *mood = current_mood(face_state);
    if (kf->eye_h_pct != FACE_KF_KEEP) {
        tween_go(&tw_eye_h, kf->eye_h_pct, kf->duration_ms, kf->ease);
    }
    if (kf->eye_w_pct != FACE_KF_KEEP) {
        tween_go(&tw_eye_w, kf->eye_w_pct, kf->duration_ms, kf->ease);
    }
    if (kf->eye_dy != FACE_KF_KEEP) {
        tween_go(&tw_eye_dy, mood->eye_dy + kf->eye_dy, kf->duration_ms, kf->ease);
    }
    if (kf->tilt_ddeg != FACE_KF_KEEP) {
        tween_go(&tw_tilt, kf->tilt_ddeg, kf->duration_ms, kf->ease);
    }
    if (kf->gaze_x != FACE_KF_KEEP) {
        tween_go(&tw_gaze_x, kf->gaze_x, kf->duration_ms, kf->ease);
    }
    tween_go(&tw_mouth_o, kf->mouth_o_pct, kf->duration_ms, kf->ease);
}

static void gesture_start(room_face_gesture_t gesture)
{
    active_gesture = &GESTURES[gesture];
    gesture_frame = 0;
    gesture_pending_advance = false;
    gesture_apply_frame();
}

static void gesture_step(void)
{
    if (active_gesture == NULL) {
        return;
    }
    /* Frames advance when their primary tween lands; eye_h drives most
     * gestures and gaze drives shake, so wait on whichever the frame set. */
    const face_keyframe_t *kf = &active_gesture->frames[gesture_frame];
    const face_tween_t *lead =
        kf->gaze_x != FACE_KF_KEEP && kf->eye_h_pct == FACE_KF_KEEP ? &tw_gaze_x : &tw_eye_h;
    if (kf->restore) {
        lead = &tw_eye_h;
    }
    if (lead->elapsed_ms < lead->duration_ms) {
        return;
    }
    if (gesture_frame + 1 >= active_gesture->count) {
        active_gesture = NULL;
        return;
    }
    ++gesture_frame;
    gesture_apply_frame();
}

/* ------------------------------------------------------------------ blink */

static void schedule_blink(int64_t now_us, const room_face_mood_style_t *mood)
{
    next_blink_us = now_us + 2200000 + (int64_t)face_rand(2600) * 1000;
    blink_slow = face_rand(100) < mood->slow_blink_pct;
}

static void trigger_blink(bool slow, bool make_double)
{
    if (blink_phase != BLINK_IDLE) {
        return;
    }
    blink_phase = BLINK_CLOSING;
    blink_elapsed_ms = 0;
    blink_slow = slow;
    blink_double_pending = make_double;
    last_blink_us = esp_timer_get_time();
}

static void blink_step(uint16_t dt_ms, int64_t now_us, const room_face_mood_style_t *mood)
{
    if (blink_phase == BLINK_IDLE) {
        if (now_us >= next_blink_us) {
            trigger_blink(blink_slow, face_rand(100) < 15);
            schedule_blink(now_us, mood);
        }
        blink_q10 = 1024;
        return;
    }
    blink_elapsed_ms = (uint16_t)(blink_elapsed_ms + dt_ms);
    uint16_t close_ms = blink_slow ? 150 : 60;
    uint16_t hold_ms = blink_slow ? 160 : 30;
    uint16_t open_ms = blink_slow ? 230 : 115;
    switch (blink_phase) {
    case BLINK_CLOSING: {
        int32_t p = blink_elapsed_ms >= close_ms ? 1024 : ((int32_t)blink_elapsed_ms * 1024) / close_ms;
        /* Lids fall: accelerating close reads natural. */
        blink_q10 = 1024 - ease_apply(FACE_EASE_IN_QUAD, p);
        if (blink_elapsed_ms >= close_ms) {
            blink_phase = BLINK_HOLD;
            blink_elapsed_ms = 0;
        }
        break;
    }
    case BLINK_HOLD:
        blink_q10 = 0;
        if (blink_elapsed_ms >= hold_ms) {
            blink_phase = BLINK_OPENING;
            blink_elapsed_ms = 0;
        }
        break;
    case BLINK_OPENING: {
        int32_t p = blink_elapsed_ms >= open_ms ? 1024 : ((int32_t)blink_elapsed_ms * 1024) / open_ms;
        /* Decelerating open with a small settle bounce past fully-open. */
        blink_q10 = ease_apply(FACE_EASE_OUT_BOUNCE, p);
        if (blink_elapsed_ms >= open_ms) {
            blink_phase = BLINK_IDLE;
            blink_q10 = 1024;
            if (blink_double_pending) {
                blink_double_pending = false;
                /* Second blink of a double lands ~140 ms after the first. */
                next_blink_us = esp_timer_get_time() + 140000;
                blink_slow = false;
            }
        }
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------- gaze */

static void schedule_saccade(int64_t now_us, const room_face_mood_style_t *mood)
{
    uint32_t span = (uint32_t)(mood->saccade_max_ms - mood->saccade_min_ms);
    next_saccade_us = now_us + (int64_t)mood->saccade_min_ms * 1000 + (int64_t)face_rand(span) * 1000;
    int32_t gx = (int32_t)face_rand(29) - 14;
    int32_t gy = (int32_t)face_rand(17) - 8;
    if (face_state == ROOM_FACE_THINKING) {
        gx -= 10;
        gy -= 12;
    }
    int32_t jump_x = gx - tw_gaze_x.value;
    if (jump_x < 0) {
        jump_x = -jump_x;
    }
    /* Big gaze jumps are masked with a blink, as real eyes do. */
    if (jump_x > 18 && blink_phase == BLINK_IDLE &&
        esp_timer_get_time() - last_blink_us > 800000) {
        trigger_blink(false, false);
    }
    uint16_t travel_ms = 220 + (uint16_t)face_rand(90);
    tween_go(&tw_gaze_x, gx, travel_ms, FACE_EASE_OUT_BACK);
    tween_go(&tw_gaze_y, gy, travel_ms, FACE_EASE_OUT_BACK);
}

/* ------------------------------------------------------------------ mouth */

/* Rolling speech levels feed three bars with staggered delays, so the mouth
 * reads as a wave moving through it rather than three copies of one value. */
#define LEVEL_RING 12
static uint8_t level_ring[LEVEL_RING];
static uint8_t level_ring_at;

static void mouth_apply(int32_t smile, lv_color_t color)
{
    bool speaking = face_state == ROOM_FACE_SPEAKING;
    bool o_mouth = tw_mouth_o.value > 4;
    if (o_mouth) {
        int32_t h = 10 + (tw_mouth_o.value * 52) / 100;
        int32_t w = 44 - (h * 14) / 62;
        lv_obj_clear_flag(mouth_o, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(mouth_o, w, h);
        lv_obj_set_pos(mouth_o, face_width / 2 - w / 2, mouth_center_y - h / 2);
        lv_obj_set_style_bg_color(mouth_o, color, 0);
    } else {
        lv_obj_add_flag(mouth_o, LV_OBJ_FLAG_HIDDEN);
    }

    if (speaking && !o_mouth) {
        lv_obj_add_flag(mouth_arc, LV_OBJ_FLAG_HIDDEN);
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        uint8_t live = now_ms - speech_level_at_ms > SPEECH_STALE_MS ? 0 : speech_level;
        level_ring[level_ring_at] = live;
        level_ring_at = (uint8_t)((level_ring_at + 1) % LEVEL_RING);
        static const uint8_t TAPS[3] = {1, 5, 9};
        for (int i = 0; i < 3; ++i) {
            uint8_t tap = level_ring[(level_ring_at + LEVEL_RING - TAPS[i]) % LEVEL_RING];
            int32_t h = 8 + ((int32_t)tap * 46) / 255;
            /* Tall bars narrow slightly: "oh" vs "ee". */
            int32_t w = 32 - (h * 6) / 54;
            lv_obj_clear_flag(bars[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(bars[i], w, h);
            int32_t spacing = face_width * 12 / 100;
            int32_t bar_width = face_width * 8 / 100;
            lv_obj_set_pos(bars[i], face_width / 2 + (i - 1) * spacing - w / 2 + bar_width / 16,
                           mouth_center_y - h / 2);
            lv_obj_set_style_bg_color(bars[i], color, 0);
        }
        return;
    }
    for (int i = 0; i < 3; ++i) {
        lv_obj_add_flag(bars[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (o_mouth) {
        lv_obj_add_flag(mouth_arc, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    /* Resting mouth: an arc segment. Positive curve smiles (bottom of a
     * circle above the mouth line), negative frowns (top of one below). */
    int32_t curve = smile;
    if (curve > -8 && curve < 8) {
        curve = curve < 0 ? -8 : 8;
    }
    int32_t mag = curve < 0 ? -curve : curve;
    int32_t diameter = 300 - mag; /* deeper curve = tighter circle */
    lv_obj_clear_flag(mouth_arc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(mouth_arc, diameter, diameter);
    if (curve > 0) {
        lv_arc_set_bg_angles(mouth_arc, 55, 125);
        lv_obj_set_pos(mouth_arc, face_width / 2 - diameter / 2,
                       mouth_center_y - diameter + face_height * 5 / 100);
    } else {
        lv_arc_set_bg_angles(mouth_arc, 235, 305);
        lv_obj_set_pos(mouth_arc, face_width / 2 - diameter / 2,
                       mouth_center_y - face_height * 5 / 100);
    }
    lv_obj_set_style_arc_color(mouth_arc, color, LV_PART_MAIN);
}

/* ------------------------------------------------------------------- tick */

static void face_hint_refresh(void *arg)
{
    (void)arg;
    if (face_state == ROOM_FACE_HINT && hint_until_us != 0) {
        return;
    }
    if (controller.refresh != NULL) controller.refresh();
}

static void face_tick(lv_timer_t *timer)
{
    (void)timer;
    room_face_state_t state = face_state;
    if (state == ROOM_FACE_HIDDEN) {
        return;
    }
    int64_t now_us = esp_timer_get_time();
    uint16_t dt_ms = (uint16_t)((now_us - last_tick_us) / 1000);
    last_tick_us = now_us;
    if (dt_ms < 4) {
        dt_ms = 4;
    } else if (dt_ms > 50) {
        dt_ms = 50;
    }
    ++tick_count;

    if (state == ROOM_FACE_HINT && hint_until_us != 0 && now_us >= hint_until_us) {
        if (lv_async_call(face_hint_refresh, NULL) == LV_RESULT_OK) {
            hint_until_us = 0;
        }
        return;
    }
    /* A long-held hint earns one yawn. */
    if (state == ROOM_FACE_HINT && !hint_yawned && active_gesture == NULL &&
        hint_until_us - now_us > 6000000 && now_us - hint_shown_at_us > 12000000) {
        hint_yawned = true;
        gesture_start(ROOM_FACE_GESTURE_YAWN);
    }

    room_face_mood_t mood_index = (room_face_mood_t)face_mood_index;
    if (mood_hold_until_us != 0 && now_us > mood_hold_until_us) {
        mood_hold_until_us = 0;
        face_mood_index = ROOM_FACE_MOOD_NEUTRAL;
        face_retarget(300, FACE_EASE_OUT_CUBIC);
        mood_index = ROOM_FACE_MOOD_NEUTRAL;
    }
    const room_face_mood_style_t *mood = current_mood(state);
    (void)mood_index;

    gesture_step();
    blink_step(dt_ms, now_us, mood);
    if (active_gesture == NULL && now_us >= next_saccade_us) {
        schedule_saccade(now_us, mood);
    }

    face_tween_t *all[] = {&tw_eye_h, &tw_eye_w, &tw_eye_dy, &tw_tilt, &tw_lid,
                           &tw_gaze_x, &tw_gaze_y, &tw_smile, &tw_r, &tw_g, &tw_b,
                           &tw_mouth_o, &tw_sparkle};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        tween_step(all[i], dt_ms);
    }

    /* Speech sparkle: eyes swell slightly on emphasis. */
    if (state == ROOM_FACE_SPEAKING) {
        uint32_t now_ms = (uint32_t)(now_us / 1000);
        bool peak = now_ms - speech_level_at_ms <= SPEECH_STALE_MS && speech_level > 205;
        tween_go(&tw_sparkle, peak ? 6 : 0, peak ? 90 : 320, FACE_EASE_OUT_CUBIC);
    } else if (tw_sparkle.to != 0) {
        tween_go(&tw_sparkle, 0, 200, FACE_EASE_OUT_CUBIC);
    }

    /* Breathing: slow size-and-bob pulse; stronger while listening. */
    static const int8_t BREATH[16] = {0, 2, 4, 6, 7, 8, 7, 6, 4, 2, 0, -2, -4, -5, -4, -2};
    uint32_t breath_period_ticks = 4200 / face_tick_ms; /* ~0.24 Hz */
    int32_t breath = BREATH[(tick_count * 16 / breath_period_ticks) % 16];
    bool attentive = state == ROOM_FACE_LISTENING || state == ROOM_FACE_HINT;
    int32_t bob = attentive ? (breath * 4) / 8 : (breath * 2) / 8;
    int32_t breath_pct = breath / 4; /* +-1% size */

    /* Compose eye geometry: mood/gesture base x blink x breath x sparkle,
     * with squash widening the eye as the lid drops (volume conservation). */
    int32_t h_pct = tw_eye_h.value + breath_pct + tw_sparkle.value;
    int32_t w_pct = tw_eye_w.value + breath_pct / 2 + tw_sparkle.value;
    int32_t eye_h = (eye_base_h * h_pct) / 100;
    int32_t eye_w = (eye_base_w * w_pct) / 100;
    int32_t bq = blink_q10;
    if (bq < 0) {
        bq = 0;
    }
    int32_t closed_q10 = bq > 1024 ? 0 : 1024 - bq;
    eye_h = (int32_t)(((int64_t)eye_h * bq) >> 10);
    eye_w = eye_w + (int32_t)(((int64_t)eye_w * closed_q10) >> 10) / 6;
    if (eye_h < 6) {
        eye_h = 6;
    }

    int32_t center_x = face_width / 2 + tw_gaze_x.value;
    int32_t center_y = eye_center_y + tw_eye_dy.value + tw_gaze_y.value + bob;
    lv_color_t color = lv_color_make((uint8_t)tw_r.value, (uint8_t)tw_g.value, (uint8_t)tw_b.value);

    lv_obj_t *eyes[2] = {eye_left, eye_right};
    lv_obj_t *lids[2] = {lid_left, lid_right};
    for (int i = 0; i < 2; ++i) {
        int32_t ex = center_x + (i == 0 ? -eye_gap : eye_gap) - eye_w / 2;
        lv_obj_set_style_bg_color(eyes[i], color, 0);
        lv_obj_set_size(eyes[i], eye_w, eye_h);
        lv_obj_set_pos(eyes[i], ex, center_y - eye_h / 2);
        /* +tilt lifts outer corners (delight); -tilt lifts inner (worry). */
        lv_obj_set_style_transform_rotation(eyes[i], i == 0 ? tw_tilt.value : -tw_tilt.value, 0);
        int32_t lid_h = (eye_h * tw_lid.value) / 100;
        if (lid_h > 2) {
            lv_obj_clear_flag(lids[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(lids[i], eye_w + 10, lid_h + 6);
            lv_obj_set_pos(lids[i], ex - 5, center_y - eye_h / 2 - 6);
        } else {
            lv_obj_add_flag(lids[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    mouth_apply(tw_smile.value, color);
}

/* -------------------------------------------------------------- lifecycle */

esp_err_t room_face_create(lv_obj_t *parent)
{
    lv_display_t *display = lv_obj_get_display(parent);
    face_width = lv_display_get_horizontal_resolution(display);
    face_height = lv_display_get_vertical_resolution(display);
    const esp_openclaw_room_node_config_t *board = room_board_config();
    face_tick_ms = board != NULL && board->display.animation_frame_ms > 0
        ? board->display.animation_frame_ms
        : FACE_DEFAULT_TICK_MS;
    eye_base_w = face_width * 23 / 100;
    eye_base_h = face_height * 23 / 100;
    eye_gap = face_width * 19 / 100;
    eye_center_y = face_height * 40 / 100;
    mouth_center_y = face_height * 68 / 100;
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
    lid_left = lv_obj_create(face_root);
    lid_right = lv_obj_create(face_root);
    mouth_arc = lv_arc_create(face_root);
    mouth_o = lv_obj_create(face_root);
    bars[0] = lv_obj_create(face_root);
    bars[1] = lv_obj_create(face_root);
    bars[2] = lv_obj_create(face_root);
    lv_obj_t *parts[] = {eye_left, eye_right, lid_left, lid_right, mouth_o,
                         bars[0], bars[1], bars[2]};
    bool ok = mouth_arc != NULL;
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) {
        if (parts[i] == NULL) {
            ok = false;
            continue;
        }
        lv_obj_remove_style_all(parts[i]);
        lv_obj_set_style_bg_opa(parts[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(parts[i], 26, 0);
        lv_obj_remove_flag(parts[i], LV_OBJ_FLAG_CLICKABLE);
    }
    if (!ok) {
        lv_obj_delete(face_root);
        face_root = NULL;
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *eye = i == 0 ? eye_left : eye_right;
        lv_obj_set_style_transform_pivot_x(eye, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(eye, lv_pct(50), 0);
    }
    lv_obj_set_style_bg_color(lid_left, lv_color_black(), 0);
    lv_obj_set_style_bg_color(lid_right, lv_color_black(), 0);
    lv_obj_set_style_radius(lid_left, 8, 0);
    lv_obj_set_style_radius(lid_right, 8, 0);
    lv_obj_set_style_radius(mouth_o, 18, 0);
    lv_obj_set_style_radius(bars[0], 10, 0);
    lv_obj_set_style_radius(bars[1], 10, 0);
    lv_obj_set_style_radius(bars[2], 10, 0);
    /* The arc is a bare stroke: style its background arc, hide the rest. */
    lv_obj_remove_style_all(mouth_arc);
    lv_obj_set_style_arc_width(mouth_arc, 9, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(mouth_arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(mouth_arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(mouth_arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(mouth_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_remove_flag(mouth_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(face_root, LV_OBJ_FLAG_HIDDEN);

    tween_jump(&tw_eye_h, 100);
    tween_jump(&tw_eye_w, 100);
    tween_jump(&tw_gaze_x, 0);
    tween_jump(&tw_gaze_y, 0);
    lv_color_t c = lv_color_hex(MOODS[ROOM_FACE_MOOD_NEUTRAL].color);
    tween_jump(&tw_r, c.red);
    tween_jump(&tw_g, c.green);
    tween_jump(&tw_b, c.blue);
    tween_jump(&tw_smile, MOODS[ROOM_FACE_MOOD_NEUTRAL].smile);

    face_timer = lv_timer_create(face_tick, face_tick_ms, NULL);
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
    bool waking = face_state == ROOM_FACE_HIDDEN;
    face_state = state;
    int64_t now_us = esp_timer_get_time();
    last_tick_us = now_us;
    if (next_blink_us == 0) {
        schedule_blink(now_us, &MOODS[ROOM_FACE_MOOD_NEUTRAL]);
        next_saccade_us = now_us + 600000;
    }
    face_retarget(220, FACE_EASE_OUT_CUBIC);
    if (waking && active_gesture == NULL) {
        /* Coming back from dark or canvas: a quick double blink says "hi". */
        trigger_blink(false, true);
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
        hint_shown_at_us = esp_timer_get_time();
        hint_yawned = false;
    }
}

void room_face_hide(void)
{
    if (face_root == NULL) {
        return;
    }
    face_state = ROOM_FACE_HIDDEN;
    hint_until_us = 0;
    active_gesture = NULL;
    lv_obj_add_flag(face_root, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(face_timer);
}

bool room_face_is_visible(void)
{
    return face_state != ROOM_FACE_HIDDEN;
}

void room_face_reset_mood(void)
{
    face_mood_index = ROOM_FACE_MOOD_NEUTRAL;
    mood_hold_until_us = 0;
}

void room_face_play_gesture(room_face_gesture_t gesture)
{
    if ((int)gesture < 0 || (int)gesture >= (int)(sizeof(GESTURES) / sizeof(GESTURES[0]))) {
        return;
    }
    if (!room_board_display_lock(100)) {
        return;
    }
    if (face_root != NULL && face_state != ROOM_FACE_HIDDEN) {
        gesture_start(gesture);
        ESP_LOGI(TAG, "gesture: %s", GESTURES[gesture].name);
    }
    room_board_display_unlock();
}

void room_face_set_speech_level(uint8_t level)
{
    speech_level = level;
    speech_level_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

/* --------------------------------------------------------------- commands */

static esp_err_t face_command_error(
    esp_openclaw_node_error_t *out_error,
    const char *code,
    const char *message,
    esp_err_t err)
{
    out_error->code = code;
    out_error->message = message;
    return err;
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
        return face_command_error(
            out_error, "INVALID_PARAMS", "face.set params must be a JSON object", ESP_ERR_INVALID_ARG);
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
        return face_command_error(
            out_error,
            "INVALID_PARAMS",
            "face.set requires mood neutral|happy|excited|thinking|sleepy|sad and optional holdMs 0..600000",
            ESP_ERR_INVALID_ARG);
    }
    int64_t hold_ms = hold != NULL ? (int64_t)hold->valuedouble : 0;
    cJSON_Delete(params);

    /* One display-lock hold makes routing atomic against talk-state
     * transitions, which repaint under the same lock. The lock is recursive,
     * so the hint path below may take it again. */
    if (!room_board_display_lock(200)) {
        return face_command_error(
            out_error, "UNAVAILABLE", "the display is busy; retry face.set", ESP_ERR_TIMEOUT);
    }
    bool talk_active = controller.talk_active != NULL && controller.talk_active();
    bool canvas_covering = controller.canvas_active != NULL && controller.canvas_active();
    if (talk_active) {
        /* Mood rides the (possibly canvas-covered) talk face; hold 0 keeps it
         * until the call ends. */
        face_mood_index = (uint8_t)mood_index;
        mood_hold_until_us = hold_ms > 0 ? esp_timer_get_time() + hold_ms * 1000 : 0;
        face_retarget(260, FACE_EASE_OUT_CUBIC);
    } else {
        /* No talk session: the mood always times out. Without canvas the face
         * pops up on its own so the expression has a visible outcome; under
         * canvas it is stored only, applying if the face shows in time. */
        int64_t show_ms = hold_ms > 0 ? hold_ms : 8000;
        face_mood_index = (uint8_t)mood_index;
        mood_hold_until_us = esp_timer_get_time() + show_ms * 1000;
        if (!canvas_covering) {
            if (controller.show_hint != NULL) controller.show_hint((uint32_t)show_ms);
        }
        face_retarget(260, FACE_EASE_OUT_CUBIC);
    }
    bool visible = room_face_is_visible();
    room_board_display_unlock();

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
        return face_command_error(
            out_error, "INTERNAL", "not enough memory for the command result", ESP_ERR_NO_MEM);
    }
    return ESP_OK;
}

static esp_err_t handle_face_gesture(
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
    cJSON *name = cJSON_IsObject(params)
        ? cJSON_GetObjectItemCaseSensitive(params, "name")
        : NULL;
    const char *text = cJSON_IsString(name) ? name->valuestring : NULL;
    int index = -1;
    for (int i = 0; text != NULL && i < (int)(sizeof(GESTURES) / sizeof(GESTURES[0])); ++i) {
        if (strcmp(text, GESTURES[i].name) == 0) {
            index = i;
            break;
        }
    }
    cJSON_Delete(params);
    if (index < 0) {
        return face_command_error(
            out_error,
            "INVALID_PARAMS",
            "face.gesture requires name surprise|yawn|nod|shake",
            ESP_ERR_INVALID_ARG);
    }

    if (!room_board_display_lock(200)) {
        return face_command_error(
            out_error, "UNAVAILABLE", "the display is busy; retry face.gesture", ESP_ERR_TIMEOUT);
    }
    bool visible = room_face_is_visible();
    bool canvas_covering = controller.canvas_active != NULL && controller.canvas_active();
    if (!visible && !canvas_covering) {
        /* Give the gesture a stage: pop the hint face up briefly. */
        if (controller.show_hint != NULL) controller.show_hint(6000);
        visible = room_face_is_visible();
    }
    if (visible) {
        gesture_start((room_face_gesture_t)index);
    }
    room_board_display_unlock();

    char payload[80];
    snprintf(
        payload,
        sizeof(payload),
        "{\"gesture\":\"%s\",\"played\":%s}",
        GESTURES[index].name,
        visible ? "true" : "false");
    *out_payload_json = strdup(payload);
    if (*out_payload_json == NULL) {
        return face_command_error(
            out_error, "INTERNAL", "not enough memory for the command result", ESP_ERR_NO_MEM);
    }
    return ESP_OK;
}

esp_err_t room_face_register_node_commands(esp_openclaw_node_handle_t node)
{
    static const esp_openclaw_node_command_t COMMANDS[] = {
        {.name = "face.set", .handler = handle_face_set},
        {.name = "face.gesture", .handler = handle_face_gesture},
    };
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); ++i) {
        esp_err_t err = esp_openclaw_node_register_command(node, &COMMANDS[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
