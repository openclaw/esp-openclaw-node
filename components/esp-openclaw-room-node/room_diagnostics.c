#include "room_diagnostics.h"

#include <inttypes.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "room_board.h"
#include "room_diagnostics_data.h"
#include "room_media.h"
#include "room_runtime_diagnostics.h"
#include "room_ui_controller.h"

#define TAG "room_diagnostics"
#define DIAGNOSTICS_REFRESH_MS 75
#define DIAGNOSTICS_STALE_US (1500 * 1000)

typedef struct {
    lv_obj_t *bar;
    lv_obj_t *label;
    const char *name;
} diagnostics_meter_t;

typedef enum {
    TONE_FACE_OPEN,
    TONE_FACE_O,
    TONE_FACE_HAPPY,
    TONE_FACE_SIDE_EYE,
    TONE_FACE_WORRIED,
} tone_buddy_face_t;

typedef struct {
    const char *primary, *supporting;
    uint32_t color;
    tone_buddy_face_t face;
} tone_buddy_state_desc_t;

typedef struct {
    int16_t face_x, eye_y, eye_gap;
    int16_t eye_width, eye_height, mouth_y;
} tone_buddy_layout_t;

typedef struct {
    int16_t eye_width[2], eye_height[2];
    int16_t eye_dx[2], eye_dy[2], eye_rotation[2];
    int16_t mouth_width, mouth_height, mouth_dx, mouth_y;
    int16_t mouth_start, mouth_end;
} tone_buddy_pose_t;

static const tone_buddy_state_desc_t TONE_BUDDY_STATES[] = {
    [ROOM_MEDIA_TONE_IDLE] = {
        "Test speaker", "Tap the face to play a tone.", 0x53d7ff, TONE_FACE_OPEN},
    [ROOM_MEDIA_TONE_RUNNING] = {
        "Listen for the tone...", "Sending the test tone now.", 0x3c8ee6, TONE_FACE_O},
    [ROOM_MEDIA_TONE_DONE] = {
        "Tone sent!", "Renderer accepted it. Did you hear it? Tap me again.",
        0xffd166, TONE_FACE_HAPPY},
    [ROOM_MEDIA_TONE_BUSY] = {
        "Shh, I'm talking", "Tap me after Talk ends.", 0x9b8cff, TONE_FACE_SIDE_EYE},
    [ROOM_MEDIA_TONE_ERROR] = {
        "That didn't sing", "Tap me to retry. Details are below.",
        0xff6b6b, TONE_FACE_WORRIED},
};

static lv_obj_t *modal;
static lv_obj_t *audio_text;
static lv_obj_t *system_text;
static lv_obj_t *tone_card, *tone_mouth;
static lv_obj_t *tone_eyes[2], *tone_labels[2];
static lv_timer_t *refresh_timer;
static diagnostics_meter_t mic_meter;
static diagnostics_meter_t afe_meter;
static diagnostics_meter_t renderer_meter;
static uint8_t slow_ticks;
static room_media_tone_state_t tone_buddy_last_state = ROOM_MEDIA_TONE_ERROR + 1;
static uint8_t tone_buddy_ticks_since_entry;
static bool tone_buddy_compact;
static tone_buddy_layout_t tone_buddy_layout;
/* Refresh runs only on taskLVGL. Keep its large format destinations out of
 * that task's stack without adding locking or an allocation failure path. */
static char audio_text_buffer[1400];
static char system_text_buffer[1200];

static void tone_buddy_pose(const tone_buddy_pose_t *pose, lv_color_t color)
{
    for (size_t i = 0; i < 2; ++i) {
        int32_t side = i == 0 ? -1 : 1;
        int32_t center_x = tone_buddy_layout.face_x +
            side * tone_buddy_layout.eye_gap + pose->eye_dx[i];
        int32_t center_y = tone_buddy_layout.eye_y + pose->eye_dy[i];
        lv_obj_set_size(tone_eyes[i], pose->eye_width[i], pose->eye_height[i]);
        lv_obj_set_pos(
            tone_eyes[i],
            center_x - pose->eye_width[i] / 2,
            center_y - pose->eye_height[i] / 2);
        lv_obj_set_style_bg_color(tone_eyes[i], color, 0);
        lv_obj_set_style_transform_rotation(tone_eyes[i], pose->eye_rotation[i], 0);
    }
    lv_obj_set_size(tone_mouth, pose->mouth_width, pose->mouth_height);
    lv_obj_set_pos(
        tone_mouth,
        tone_buddy_layout.face_x + pose->mouth_dx - pose->mouth_width / 2,
        pose->mouth_y);
    lv_arc_set_bg_angles(tone_mouth, pose->mouth_start, pose->mouth_end);
    lv_obj_set_style_arc_color(tone_mouth, color, LV_PART_MAIN);
}

static void tone_buddy_apply_state(room_media_tone_state_t state)
{
    const tone_buddy_state_desc_t *desc = &TONE_BUDDY_STATES[state];
    int32_t mouth_diameter = tone_buddy_compact ? 56 : 70;
    tone_buddy_pose_t pose = {
        .eye_width = {tone_buddy_layout.eye_width, tone_buddy_layout.eye_width},
        .eye_height = {tone_buddy_layout.eye_height, tone_buddy_layout.eye_height},
        .mouth_width = mouth_diameter,
        .mouth_height = mouth_diameter,
        .mouth_y = tone_buddy_layout.mouth_y - mouth_diameter + 8,
        .mouth_start = 55,
        .mouth_end = 125,
    };

    switch (desc->face) {
        case TONE_FACE_O:
            pose.eye_width[0] += tone_buddy_compact ? 8 : 12;
            pose.eye_width[1] = pose.eye_width[0];
            pose.eye_height[0] += tone_buddy_compact ? 3 : 5;
            pose.eye_height[1] = pose.eye_height[0];
            pose.mouth_width = tone_buddy_compact ? 28 : 34;
            pose.mouth_height = tone_buddy_compact ? 34 : 42;
            pose.mouth_y = tone_buddy_layout.mouth_y - pose.mouth_height / 2;
            pose.mouth_start = 0;
            pose.mouth_end = 360;
            break;
        case TONE_FACE_HAPPY:
            pose.eye_width[0] += tone_buddy_compact ? 5 : 8;
            pose.eye_width[1] = pose.eye_width[0];
            pose.eye_height[0] = pose.eye_height[1] = tone_buddy_compact ? 8 : 10;
            pose.eye_rotation[0] = 60;
            pose.eye_rotation[1] = -60;
            mouth_diameter = tone_buddy_compact ? 52 : 66;
            pose.mouth_width = pose.mouth_height = mouth_diameter;
            pose.mouth_y = tone_buddy_layout.mouth_y - mouth_diameter + 10;
            pose.mouth_start = 42;
            pose.mouth_end = 138;
            break;
        case TONE_FACE_SIDE_EYE: {
            int32_t gaze = tone_buddy_compact ? 9 : 14;
            pose.eye_height[0] = tone_buddy_compact ? 14 : 18;
            pose.eye_height[1] = pose.eye_height[0] - 3;
            pose.eye_width[1] -= tone_buddy_compact ? 10 : 14;
            pose.eye_dx[0] = pose.eye_dx[1] = gaze;
            pose.eye_dy[1] = 3;
            pose.mouth_width = pose.mouth_height = mouth_diameter + 8;
            pose.mouth_dx = gaze;
            pose.mouth_start = 78;
            pose.mouth_end = 102;
            break;
        }
        case TONE_FACE_WORRIED:
            pose.eye_height[0] -= tone_buddy_compact ? 5 : 7;
            pose.eye_height[1] = pose.eye_height[0];
            pose.eye_rotation[0] = -80;
            pose.eye_rotation[1] = 80;
            pose.mouth_y = tone_buddy_layout.mouth_y - 5;
            pose.mouth_start = 230;
            pose.mouth_end = 310;
            break;
        default:
            break;
    }

    lv_color_t color = lv_color_hex(desc->color);
    lv_obj_set_style_border_color(tone_card, color, 0);
    lv_obj_set_style_text_color(tone_labels[0], color, 0);
    lv_label_set_text_static(tone_labels[0], desc->primary);
    lv_label_set_text_static(tone_labels[1], desc->supporting);
    tone_buddy_pose(&pose, color);
}

static void tone_buddy_update(
    const room_media_tone_snapshot_t *tone,
    uint8_t renderer_level)
{
    if (tone->state != tone_buddy_last_state) {
        tone_buddy_last_state = tone->state;
        tone_buddy_ticks_since_entry = 0;
        tone_buddy_apply_state(tone->state);
    }

    if (tone->state == ROOM_MEDIA_TONE_RUNNING) {
        static const int8_t BOB[4] = {-2, 0, 2, 0};
        int32_t bob = BOB[tone_buddy_ticks_since_entry & 3U];
        int32_t eye_height = tone_buddy_layout.eye_height +
            (tone_buddy_compact ? 3 : 5);
        int32_t eye_y = tone_buddy_layout.eye_y - eye_height / 2 + bob;
        int32_t mouth_height = (tone_buddy_compact ? 24 : 30) +
            ((int32_t)renderer_level * (tone_buddy_compact ? 20 : 24)) / 100;
        /* Exactly three animated geometry writes: two-eye bob plus the
         * renderer-reactive O. Everything else stays fixed for the state. */
        lv_obj_set_y(tone_eyes[0], eye_y);
        lv_obj_set_y(tone_eyes[1], eye_y);
        lv_obj_set_height(tone_mouth, mouth_height);
    } else if (tone->state == ROOM_MEDIA_TONE_DONE) {
        static const int8_t BOUNCE[8] = {-6, -3, 1, 0, -2, 0, 1, 0};
        if (tone_buddy_ticks_since_entry <= 8U) {
            int32_t bounce = tone_buddy_ticks_since_entry < 8U
                ? BOUNCE[tone_buddy_ticks_since_entry]
                : 0;
            int32_t eye_y = tone_buddy_layout.eye_y -
                (tone_buddy_compact ? 8 : 10) / 2;
            int32_t mouth_diameter = tone_buddy_compact ? 52 : 66;
            int32_t mouth_y = tone_buddy_layout.mouth_y - mouth_diameter + 10;
            lv_obj_set_y(tone_eyes[0], eye_y + bounce);
            lv_obj_set_y(tone_eyes[1], eye_y + bounce);
            lv_obj_set_y(tone_mouth, mouth_y + bounce);
        }
    }
    if (tone_buddy_ticks_since_entry < UINT8_MAX) {
        ++tone_buddy_ticks_since_entry;
    }
}

static const char *afe_mode_name(room_diagnostics_afe_mode_t mode)
{
    return mode == ROOM_DIAGNOSTICS_AFE_AMBIENT_SR ? "ambient SR"
        : mode == ROOM_DIAGNOSTICS_AFE_TALK_VC ? "Talk VC"
        : "unknown";
}

static const char *capture_owner_name(room_diagnostics_capture_owner_t owner)
{
    return owner == ROOM_DIAGNOSTICS_CAPTURE_AMBIENT ? "ambient"
        : owner == ROOM_DIAGNOSTICS_CAPTURE_TALK ? "talk"
        : "transition";
}

static void format_age(char *buffer, size_t size, int64_t timestamp_us, int64_t now_us)
{
    if (timestamp_us <= 0 || now_us < timestamp_us) {
        snprintf(buffer, size, "never");
        return;
    }
    uint64_t age_ms = (uint64_t)(now_us - timestamp_us) / 1000U;
    if (age_ms < 1000) snprintf(buffer, size, "%" PRIu64 "ms", age_ms);
    else if (age_ms < 60000) snprintf(buffer, size, "%" PRIu64 "s", age_ms / 1000U);
    else snprintf(buffer, size, "%" PRIu64 "m", age_ms / 60000U);
}

static void update_meter(
    diagnostics_meter_t *meter,
    uint8_t level,
    int64_t timestamp_us,
    int64_t now_us)
{
    bool fresh = timestamp_us > 0 && now_us >= timestamp_us &&
        now_us - timestamp_us <= DIAGNOSTICS_STALE_US;
    char age[20];
    format_age(age, sizeof(age), timestamp_us, now_us);
    lv_bar_set_value(meter->bar, fresh ? level : 0, LV_ANIM_OFF);
    if (fresh) {
        lv_label_set_text_fmt(meter->label, "%s  %u/100  %s", meter->name, level, age);
    } else {
        lv_label_set_text_fmt(
            meter->label, "%s  stale (%s; last %u)", meter->name, age, level);
    }
}

static void update_slow_text(
    const room_audio_diagnostics_snapshot_t *audio,
    const room_media_tone_snapshot_t *tone)
{
    const char *tone_summary = tone->state == ROOM_MEDIA_TONE_RUNNING ? "Running"
        : tone->state == ROOM_MEDIA_TONE_DONE ? "Queued/accepted"
        : tone->state == ROOM_MEDIA_TONE_BUSY ? "Busy (Talk owns media)"
        : tone->state == ROOM_MEDIA_TONE_ERROR ? room_media_tone_error_name(tone->error)
        : "Not run";

    char read_age[20], fetch_age[20], wake_age[20], render_age[20];
    int64_t now_us = esp_timer_get_time();
    format_age(read_age, sizeof(read_age), audio->last_capture_read_us, now_us);
    format_age(fetch_age, sizeof(fetch_age), audio->last_fetch_us, now_us);
    format_age(wake_age, sizeof(wake_age), audio->last_wakenet_detection_us, now_us);
    format_age(render_age, sizeof(render_age), audio->last_renderer_accepted_us, now_us);
    snprintf(
        audio_text_buffer,
        sizeof(audio_text_buffer),
        "Capture owner: %s   AFE: %s   WakeNet: %s   volume: %u%%\n"
        "Raw reference: %u/100   AFE ring free: %u%%   feed/fetch: %" PRIu32 "/%" PRIu32 " B\n"
        "Capture read ok/err: %" PRIu64 "/%" PRIu64 " (last %s)\n"
        "AFE feed ok/err: %" PRIu64 "/%" PRIu64
        "   fetch ok/err/invalid: %" PRIu64 "/%" PRIu64 "/%" PRIu64 " (last %s)\n"
        "WakeNet detections: %" PRIu64 " (last %s)\n"
        "Renderer offered: %" PRIu64 " frames / %" PRIu64 " B; renderer accepted/errors: %" PRIu64 "/%" PRIu64 " (last %s)\n"
        "Local tone: %s; requested/enqueued/renderer accepted: %u/%u/%u. "
        "Accepted means the production renderer accepted PCM, not that the speaker played it.",
        capture_owner_name(audio->capture_owner),
        afe_mode_name(audio->afe_mode),
        audio->wakenet_enabled ? "on" : "off",
        audio->configured_volume,
        audio->playback_reference_level,
        audio->ringbuffer_free_percent,
        audio->feed_bytes,
        audio->fetch_bytes,
        audio->capture_read_successes,
        audio->capture_read_errors,
        read_age,
        audio->feed_successes,
        audio->feed_errors,
        audio->fetch_successes,
        audio->fetch_errors,
        audio->fetch_invalid_sizes,
        fetch_age,
        audio->wakenet_detections,
        wake_age,
        audio->renderer_frames_offered,
        audio->renderer_bytes_offered,
        audio->renderer_accepted,
        audio->renderer_errors,
        render_age,
        tone_summary,
        tone->requested_frames,
        tone->enqueued_frames,
        tone->renderer_accepted_frames);
    lv_label_set_text(audio_text, audio_text_buffer);

    room_runtime_diagnostics_snapshot_t runtime = {0};
    room_runtime_get_diagnostics(&runtime);
    char configured_input_gain[48];
    if (runtime.input_gain_configured) {
        snprintf(
            configured_input_gain,
            sizeof(configured_input_gain),
            "configured input gain: %.1f dB",
            runtime.configured_input_gain_db);
    } else {
        strlcpy(
            configured_input_gain,
            "configured input gain: board default",
            sizeof(configured_input_gain));
    }
    snprintf(
        system_text_buffer,
        sizeof(system_text_buffer),
        "Runtime\n"
        "Node: %s   operator: %s   media: %s   Talk: %s   camera: %s\n"
        "UI: %s%s%s   gateway: %s\n"
        "Canvas: %s, retained %s (%u components, %u images)\n"
        "Talk VAD silence: %u ms   player queues: %u/%u KiB\n\n"
        "System\n"
        "%s (%s)   %ux%u\n"
        "AFE layout: %s   configured volume: %u%%   %s\n"
        "Wi-Fi: %s   SSID: %s   RSSI: %d dBm   IP: %s\n"
        "Internal heap free/largest: %" PRIu32 "/%" PRIu32 " B   PSRAM free: %" PRIu32 " B\n"
        "Uptime: %" PRIu64 " s",
        runtime.node_ready ? "ready" : "not ready",
        runtime.operator_ready ? "ready" : "not ready",
        runtime.media_ready ? "ready" : "not ready",
        runtime.talk_phase,
        runtime.camera_active ? "active" : "inactive",
        runtime.ui_state,
        runtime.ui_detail[0] != '\0' ? " / " : "",
        runtime.ui_detail,
        runtime.gateway[0] != '\0' ? runtime.gateway : "none",
        runtime.canvas_active ? "active" : "hidden",
        runtime.canvas_kind,
        runtime.canvas_components,
        runtime.canvas_images,
        runtime.talk_vad_silence_ms,
        runtime.player_raw_queue_kib,
        runtime.player_render_queue_kib,
        runtime.display_name,
        runtime.model_identifier,
        runtime.display_width,
        runtime.display_height,
        runtime.afe_layout,
        runtime.configured_volume,
        configured_input_gain,
        runtime.wifi_connected ? "connected" : "disconnected",
        runtime.wifi_ssid[0] != '\0' ? runtime.wifi_ssid : "none",
        runtime.wifi_rssi,
        runtime.wifi_ip[0] != '\0' ? runtime.wifi_ip : "none",
        runtime.internal_heap_free,
        runtime.internal_heap_largest,
        runtime.psram_free,
        runtime.uptime_seconds);
    lv_label_set_text(system_text, system_text_buffer);
}

static void refresh_diagnostics(lv_timer_t *timer)
{
    (void)timer;
    /* Talk pills may be created while the modal is open. Keep the blocker on
     * top, then restore the one intentional exception: camera privacy. */
    if (modal != NULL) lv_obj_move_foreground(modal);
    room_ui_raise_camera_indicator();
    room_audio_diagnostics_snapshot_t audio = {0};
    room_diagnostics_audio_get(&audio);
    int64_t now_us = esp_timer_get_time();
    update_meter(&mic_meter, audio.mic_level, audio.last_capture_read_us, now_us);
    update_meter(&afe_meter, audio.afe_level, audio.last_fetch_us, now_us);
    update_meter(
        &renderer_meter,
        audio.renderer_level,
        audio.last_renderer_accepted_us,
        now_us);
    room_media_tone_snapshot_t tone = {0};
    room_media_get_tone_snapshot(&tone);
    tone_buddy_update(&tone, audio.renderer_level);
    if (slow_ticks == 0) update_slow_text(&audio, &tone);
    slow_ticks = (uint8_t)((slow_ticks + 1U) % 14U);
}

static void close_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_CLICKED || code == LV_EVENT_LONG_PRESSED) {
        room_diagnostics_close();
    }
}

static void tone_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    room_media_tone_snapshot_t tone = {0};
    room_media_get_tone_snapshot(&tone);
    if (tone.state != ROOM_MEDIA_TONE_RUNNING) {
        (void)room_runtime_request_test_tone();
        slow_ticks = 0;
    }
}

static void tone_buddy_clear(void)
{
    tone_labels[0] = tone_labels[1] = NULL;
    tone_eyes[0] = tone_eyes[1] = NULL;
    tone_card = tone_mouth = NULL;
    tone_buddy_last_state = ROOM_MEDIA_TONE_ERROR + 1;
    tone_buddy_ticks_since_entry = 0;
    tone_buddy_compact = false;
    tone_buddy_layout = (tone_buddy_layout_t){0};
}

static void create_tone_buddy(lv_obj_t *parent)
{
    int32_t horizontal_resolution =
        lv_display_get_horizontal_resolution(lv_obj_get_display(parent));
    int32_t card_width = horizontal_resolution - 28;
    tone_buddy_compact = horizontal_resolution < 700;
    tone_buddy_layout = tone_buddy_compact
        ? (tone_buddy_layout_t){card_width / 2, 40, 42, 42, 28, 82}
        : (tone_buddy_layout_t){176, 52, 58, 58, 36, 116};

    tone_card = lv_button_create(parent);
    tone_eyes[0] = lv_obj_create(tone_card);
    tone_eyes[1] = lv_obj_create(tone_card);
    tone_mouth = lv_arc_create(tone_card);
    tone_labels[0] = lv_label_create(tone_card);
    tone_labels[1] = lv_label_create(tone_card);
    lv_obj_t *children[] = {
        tone_eyes[0], tone_eyes[1], tone_mouth, tone_labels[0], tone_labels[1]};
    for (size_t i = 0; i < sizeof(children) / sizeof(children[0]); ++i) {
        lv_obj_remove_flag(children[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_set_size(tone_card, LV_PCT(100), 174);
    lv_obj_set_style_radius(tone_card, 22, 0);
    lv_obj_set_style_bg_color(tone_card, lv_color_hex(0x171c26), 0);
    lv_obj_set_style_bg_color(tone_card, lv_color_hex(0x202734), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(tone_card, 2, 0);
    lv_obj_set_style_border_color(tone_card, lv_color_hex(0x53d7ff), 0);
    lv_obj_set_style_shadow_width(tone_card, 0, 0);
    lv_obj_set_style_pad_all(tone_card, 0, 0);
    lv_obj_set_style_text_align(
        tone_card,
        tone_buddy_compact ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT,
        0);
    lv_obj_remove_flag(tone_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(tone_card, tone_event, LV_EVENT_CLICKED, NULL);

    for (size_t i = 0; i < 2; ++i) {
        lv_obj_remove_style_all(tone_eyes[i]);
        lv_obj_set_style_bg_opa(tone_eyes[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(tone_eyes[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_transform_pivot_x(tone_eyes[i], lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(tone_eyes[i], lv_pct(50), 0);
    }

    lv_obj_remove_style_all(tone_mouth);
    lv_obj_set_style_arc_width(tone_mouth, tone_buddy_compact ? 7 : 9, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(tone_mouth, true, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(tone_mouth, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(tone_mouth, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(tone_mouth, LV_OPA_TRANSP, LV_PART_KNOB);

    int32_t text_width = tone_buddy_compact ? card_width - 24 : card_width - 354;
    lv_align_t text_align = tone_buddy_compact ? LV_ALIGN_TOP_MID : LV_ALIGN_TOP_LEFT;
    int32_t text_x = tone_buddy_compact ? 0 : 330;
    lv_obj_set_size(tone_labels[0], text_width, 36);
    lv_obj_align(tone_labels[0], text_align, text_x, tone_buddy_compact ? 101 : 43);
    const lv_font_t *primary_font = LV_FONT_DEFAULT;
    const lv_font_t *supporting_font = LV_FONT_DEFAULT;
#if LV_FONT_MONTSERRAT_20
    primary_font = &lv_font_montserrat_20;
    supporting_font = &lv_font_montserrat_20;
#endif
#if LV_FONT_MONTSERRAT_28
    if (!tone_buddy_compact) primary_font = &lv_font_montserrat_28;
#endif
#if LV_FONT_MONTSERRAT_16
    if (tone_buddy_compact) supporting_font = &lv_font_montserrat_16;
#endif
    lv_obj_set_style_text_font(tone_labels[0], primary_font, 0);

    lv_obj_set_size(tone_labels[1], text_width, tone_buddy_compact ? 42 : 48);
    lv_obj_align(tone_labels[1], text_align, text_x, tone_buddy_compact ? 130 : 86);
    lv_label_set_long_mode(tone_labels[1], LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(tone_labels[1], lv_color_hex(0xbdc7d6), 0);
    lv_obj_set_style_text_font(tone_labels[1], supporting_font, 0);
    tone_buddy_apply_state(ROOM_MEDIA_TONE_IDLE);
}

static diagnostics_meter_t create_meter(lv_obj_t *parent, const char *name, uint32_t color)
{
    diagnostics_meter_t meter = {.name = name};
    meter.label = lv_label_create(parent);
    lv_obj_set_width(meter.label, LV_PCT(100));
    lv_obj_set_style_text_color(meter.label, lv_color_white(), 0);
    lv_label_set_text_fmt(meter.label, "%s  never", name);
    meter.bar = lv_bar_create(parent);
    lv_obj_set_size(meter.bar, LV_PCT(100), 24);
    lv_bar_set_range(meter.bar, 0, 100);
    lv_obj_set_style_bg_color(meter.bar, lv_color_hex(0x30343b), LV_PART_MAIN);
    lv_obj_set_style_bg_color(meter.bar, lv_color_hex(color), LV_PART_INDICATOR);
    return meter;
}

esp_err_t room_diagnostics_open(void)
{
    if (modal != NULL) return ESP_OK;

    modal = lv_obj_create(lv_layer_top());
    if (modal == NULL) return ESP_ERR_NO_MEM;
    lv_obj_set_size(modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(modal, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x0d1015), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 14, 0);
    lv_obj_set_style_pad_row(modal, 10, 0);
    lv_obj_set_flex_flow(modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(modal, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(modal, close_event, LV_EVENT_LONG_PRESSED, NULL);

    lv_obj_t *header = lv_obj_create(modal);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), 58);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(header);
#if LV_FONT_MONTSERRAT_28
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
#endif
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "Diagnostics");
    lv_obj_t *close = lv_button_create(header);
    lv_obj_set_size(close, 120, 52);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x3c4657), 0);
    lv_obj_add_event_cb(close, close_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_label = lv_label_create(close);
    lv_label_set_text(close_label, "Close");
    lv_obj_center(close_label);

    create_tone_buddy(modal);

    mic_meter = create_meter(modal, "MIC", 0x38c979);
    afe_meter = create_meter(modal, "AFE", 0x3c8ee6);
    renderer_meter = create_meter(modal, "RX/SPK", 0xe69b3c);

    audio_text = lv_label_create(modal);
    lv_obj_set_width(audio_text, LV_PCT(100));
    lv_label_set_long_mode(audio_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(audio_text, lv_color_hex(0xd7dce5), 0);

    system_text = lv_label_create(modal);
    lv_obj_set_width(system_text, LV_PCT(100));
    lv_label_set_long_mode(system_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(system_text, lv_color_hex(0xb9c0cc), 0);

    slow_ticks = 0;
    refresh_diagnostics(NULL);
    refresh_timer = lv_timer_create(refresh_diagnostics, DIAGNOSTICS_REFRESH_MS, NULL);
    room_ui_set_diagnostics_open(true);
    return ESP_OK;
}

esp_err_t room_diagnostics_close(void)
{
    if (modal == NULL) return ESP_OK;
    if (refresh_timer != NULL) {
        lv_timer_delete(refresh_timer);
        refresh_timer = NULL;
    }
    lv_obj_t *closing = modal;
    modal = NULL;
    audio_text = NULL;
    system_text = NULL;
    tone_buddy_clear();
    slow_ticks = 0;
    lv_obj_add_flag(closing, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(closing);
    room_ui_set_diagnostics_open(false);
    return ESP_OK;
}

static void open_async(void *arg)
{
    (void)arg;
    esp_err_t err = room_diagnostics_open();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "queued open failed: %s", esp_err_to_name(err));
    }
}

static void close_async(void *arg)
{
    (void)arg;
    esp_err_t err = room_diagnostics_close();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "queued close failed: %s", esp_err_to_name(err));
    }
}

static esp_err_t request_async(lv_async_cb_t callback)
{
    if (!room_ui_is_initialized()) return ESP_ERR_INVALID_STATE;
    /* LVGL's timer list is protected by the board port mutex. Hold it only
     * while enqueueing; object construction/deletion runs later on taskLVGL. */
    if (!room_board_display_lock(100)) return ESP_ERR_TIMEOUT;
    lv_result_t result = lv_async_call(callback, NULL);
    room_board_display_unlock();
    return result == LV_RESULT_OK ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t room_diagnostics_request_open(void)
{
    return request_async(open_async);
}

esp_err_t room_diagnostics_request_close(void)
{
    return request_async(close_async);
}

bool room_diagnostics_is_open(void)
{
    return modal != NULL;
}
