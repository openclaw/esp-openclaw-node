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

static lv_obj_t *modal;
static lv_obj_t *audio_text;
static lv_obj_t *system_text;
static lv_obj_t *tone_button_label;
static lv_timer_t *refresh_timer;
static diagnostics_meter_t mic_meter;
static diagnostics_meter_t afe_meter;
static diagnostics_meter_t renderer_meter;
static uint8_t slow_ticks;
/* Refresh runs only on taskLVGL. Keep its large format destinations out of
 * that task's stack without adding locking or an allocation failure path. */
static char audio_text_buffer[1400];
static char system_text_buffer[1200];

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

static void update_slow_text(const room_audio_diagnostics_snapshot_t *audio)
{
    room_media_tone_snapshot_t tone = {0};
    room_media_get_tone_snapshot(&tone);
    const char *tone_summary = tone.state == ROOM_MEDIA_TONE_RUNNING ? "Running"
        : tone.state == ROOM_MEDIA_TONE_DONE ? "Queued/accepted"
        : tone.state == ROOM_MEDIA_TONE_BUSY ? "Busy (Talk owns media)"
        : tone.state == ROOM_MEDIA_TONE_ERROR ? room_media_tone_error_name(tone.error)
        : "Not run";
    lv_label_set_text(tone_button_label,
        tone.state == ROOM_MEDIA_TONE_RUNNING ? "Running"
        : tone.state == ROOM_MEDIA_TONE_DONE ? "Queued/accepted"
        : tone.state == ROOM_MEDIA_TONE_BUSY ? "Busy"
        : tone.state == ROOM_MEDIA_TONE_ERROR ? "Tone error"
        : "Play test tone");

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
        tone.requested_frames,
        tone.enqueued_frames,
        tone.renderer_accepted_frames);
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
        "Canvas: %s, retained %s (%u components, %u images)\n\n"
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
    if (slow_ticks == 0) update_slow_text(&audio);
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

    mic_meter = create_meter(modal, "MIC", 0x38c979);
    afe_meter = create_meter(modal, "AFE", 0x3c8ee6);
    renderer_meter = create_meter(modal, "RX/SPK", 0xe69b3c);

    audio_text = lv_label_create(modal);
    lv_obj_set_width(audio_text, LV_PCT(100));
    lv_label_set_long_mode(audio_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(audio_text, lv_color_hex(0xd7dce5), 0);

    lv_obj_t *tone_button = lv_button_create(modal);
    lv_obj_set_size(tone_button, LV_PCT(100), 58);
    lv_obj_set_style_bg_color(tone_button, lv_color_hex(0x176b43), 0);
    lv_obj_add_event_cb(tone_button, tone_event, LV_EVENT_CLICKED, NULL);
    tone_button_label = lv_label_create(tone_button);
    lv_label_set_text(tone_button_label, "Play test tone");
    lv_obj_center(tone_button_label);

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
    tone_button_label = NULL;
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
