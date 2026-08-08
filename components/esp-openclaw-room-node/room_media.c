#include "room_media.h"

#include <stdlib.h>
#include <string.h>

#include "av_render.h"
#include "av_render_default.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_enc_default.h"
#include "esp_capture.h"
#include "esp_capture_sink.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "media_lib_err.h"
#include "room_aec_src.h"
#include "room_diagnostics_data.h"
#include "room_face.h"
#include "room_board.h"

#define TAG "room_media"

static esp_capture_handle_t capture;
static esp_capture_audio_src_if_t *audio_source;
static esp_capture_sink_handle_t talk_sink;
static esp_capture_sink_handle_t wake_sink;
static av_render_handle_t player;
static room_wake_callback_t wake_callback;
static void *wake_callback_ctx;
/* A binary gate, rather than a task-owned FreeRTOS mutex, spans Talk start on
 * one task and teardown on another. The diagnostics snapshot is the state
 * owner; this gate is the single serialization path for Talk and local tone. */
static SemaphoreHandle_t media_owner_gate;
static portMUX_TYPE tone_mux = portMUX_INITIALIZER_UNLOCKED;
static room_media_tone_snapshot_t tone_snapshot;
static bool tone_task_active;
static bool media_initialized;
static room_media_talk_busy_cb_t tone_busy_cb;
static void *tone_busy_ctx;
/* Only one tone worker can exist. Its frame storage therefore needs no lock
 * and must not consume a third of the worker's 4 KiB stack. */
enum {
    TONE_SAMPLES_PER_CHANNEL = 320,
    TONE_FRAME_MS = 20,
    TONE_FRAMES = 50,
    TONE_DURATION_MS = TONE_FRAME_MS * TONE_FRAMES,
    TONE_RENDER_WAIT_MS = 2200,
};
static int16_t tone_pcm[TONE_SAMPLES_PER_CHANNEL * 2];

static void room_afe_wake(void *ctx)
{
    (void)ctx;
    if (wake_callback != NULL) {
        wake_callback("hiesp", wake_callback_ctx);
    }
}

/*
 * Audio render tap: forwards every playback frame to the real I2S render and
 * feeds its mean amplitude to the face, so the mouth follows the model's
 * actual speech rather than a canned animation. The tap wraps the allocated
 * i2s render handle through the public audio_render_* dispatch API, so it
 * needs no knowledge of the render's internals.
 */
static audio_render_handle_t render_tap_target;
static uint8_t render_tap_channels = 2;

static audio_render_handle_t render_tap_init(void *cfg, int cfg_size)
{
    (void)cfg;
    (void)cfg_size;
    return render_tap_target;
}

static int render_tap_open(audio_render_handle_t render, av_render_audio_frame_info_t *info)
{
    if (info != NULL && info->channel > 0) render_tap_channels = info->channel;
    return audio_render_open(render, info);
}

static int render_tap_write(audio_render_handle_t render, av_render_audio_frame_t *frame)
{
    if (frame != NULL && frame->data != NULL && frame->size >= 2) {
        const int16_t *samples = (const int16_t *)frame->data;
        size_t count = (size_t)frame->size / 2;
        room_diagnostics_audio_record_renderer_offer(
            samples, count, render_tap_channels, (size_t)frame->size);
        room_audio_diagnostics_snapshot_t snapshot = {0};
        room_diagnostics_audio_get(&snapshot);
        room_face_set_speech_level((uint8_t)((snapshot.renderer_level * 255U) / 100U));
    }
    int result = audio_render_write(render, frame);
    if (frame != NULL && frame->data != NULL && frame->size > 0) {
        room_diagnostics_audio_record_renderer_result(result == 0);
    }
    return result;
}

static int render_tap_get_latency(audio_render_handle_t render, uint32_t *latency)
{
    return audio_render_get_latency(render, latency);
}

static int render_tap_get_frame_info(audio_render_handle_t render, av_render_audio_frame_info_t *info)
{
    return audio_render_get_frame_info(render, info);
}

static int render_tap_set_speed(audio_render_handle_t render, float speed)
{
    return audio_render_set_speed(render, speed);
}

static int render_tap_close(audio_render_handle_t render)
{
    return audio_render_close(render);
}

static void render_tap_deinit(audio_render_handle_t render)
{
    /* Release the wrapped I2S renderer so freeing the tap frees the chain. */
    audio_render_free_handle(render);
}

static audio_render_handle_t room_media_wrap_render(audio_render_handle_t inner)
{
    render_tap_target = inner;
    audio_render_cfg_t tap_cfg = {
        .ops = {
            .init = render_tap_init,
            .open = render_tap_open,
            .write = render_tap_write,
            .get_latency = render_tap_get_latency,
            .get_frame_info = render_tap_get_frame_info,
            .set_speed = render_tap_set_speed,
            .close = render_tap_close,
            .deinit = render_tap_deinit,
        },
    };
    audio_render_handle_t tap = audio_render_alloc_handle(&tap_cfg);
    return tap != NULL ? tap : inner;
}

static esp_err_t room_audio_codecs_init(
    esp_codec_dev_handle_t *record,
    esp_codec_dev_handle_t *playback)
{
    const esp_openclaw_room_node_config_t *board = room_board_config();
    esp_openclaw_room_audio_handles_t handles = {0};
    ESP_RETURN_ON_FALSE(board != NULL, ESP_ERR_INVALID_STATE, TAG, "board not bound");
    ESP_RETURN_ON_ERROR(board->audio.open(board->audio.ctx, &handles), TAG, "board audio open");
    *record = handles.record;
    *playback = handles.playback;
    return *record != NULL && *playback != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/*
 * The AFE only runs its WakeNet pass inside fetch(), and fetch() is driven by a
 * consumer reading the sink. Nothing else reads this path while no Talk call is
 * active, so drain it here: the frames are discarded, but draining keeps the
 * pipeline turning (and its feed ringbuffer from overflowing) so ambient wake
 * detections keep arriving through the AFE callback.
 */
static void wake_drain_task(void *arg)
{
    (void)arg;
    for (;;) {
        esp_capture_stream_frame_t frame = {.stream_type = ESP_CAPTURE_STREAM_TYPE_AUDIO};
        if (esp_capture_sink_acquire_frame(wake_sink, &frame, false) != ESP_CAPTURE_ERR_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        esp_capture_sink_release_frame(wake_sink, &frame);
    }
}

static void tone_set_result(
    room_media_tone_state_t state,
    room_media_tone_error_t error,
    uint16_t enqueued,
    uint16_t accepted)
{
    taskENTER_CRITICAL(&tone_mux);
    tone_snapshot.state = state;
    tone_snapshot.error = error;
    tone_snapshot.enqueued_frames = enqueued;
    tone_snapshot.renderer_accepted_frames = accepted;
    tone_task_active = false;
    taskEXIT_CRITICAL(&tone_mux);
}

static void test_tone_task(void *arg)
{
    (void)arg;
    static const int16_t wave[16] = {
        0, 8867, 16384, 21406, 23170, 21406, 16384, 8867,
        0, -8867, -16384, -21406, -23170, -21406, -16384, -8867,
    };
    uint16_t enqueued = 0;
    uint16_t accepted = 0;
    room_media_tone_error_t error = ROOM_MEDIA_TONE_ERROR_NONE;

    if (tone_busy_cb != NULL && tone_busy_cb(tone_busy_ctx)) {
        xSemaphoreGive(media_owner_gate);
        tone_set_result(ROOM_MEDIA_TONE_BUSY, ROOM_MEDIA_TONE_ERROR_NONE, 0, 0);
        vTaskDelete(NULL);
        return;
    }
    room_audio_diagnostics_snapshot_t before = {0};
    room_diagnostics_audio_get(&before);
    if (av_render_reset(player) != ESP_MEDIA_ERR_OK) {
        error = ROOM_MEDIA_TONE_ERROR_RESET;
        goto done;
    }
    av_render_audio_frame_info_t frame_info = {
        .sample_rate = 16000,
        .channel = 2,
        .bits_per_sample = 16,
    };
    /* This pinned av_render stores the pre-stream format while returning
     * WRONG_STATE; the same public-API quirk is handled during player init. */
    int frame_info_result = av_render_set_fixed_frame_info(player, &frame_info);
    if (frame_info_result != ESP_MEDIA_ERR_OK &&
        frame_info_result != ESP_MEDIA_ERR_WRONG_STATE) {
        error = ROOM_MEDIA_TONE_ERROR_FRAME_INFO;
        goto done;
    }
    av_render_audio_info_t stream = {
        .codec = AV_RENDER_AUDIO_CODEC_PCM,
        .sample_rate = 16000,
        .channel = 2,
        .bits_per_sample = 16,
    };
    if (av_render_add_audio_stream(player, &stream) != ESP_MEDIA_ERR_OK) {
        error = ROOM_MEDIA_TONE_ERROR_STREAM;
        goto done;
    }
    for (uint16_t frame_index = 0; frame_index < TONE_FRAMES; ++frame_index) {
        for (size_t i = 0; i < TONE_SAMPLES_PER_CHANNEL; ++i) {
            int16_t sample = wave[i & 15U];
            tone_pcm[i * 2] = sample;
            tone_pcm[i * 2 + 1] = sample;
        }
        av_render_audio_data_t data = {
            .pts = (uint32_t)frame_index * TONE_FRAME_MS,
            .data = (uint8_t *)tone_pcm,
            .size = sizeof(tone_pcm),
        };
        if (av_render_add_audio_data(player, &data) != ESP_MEDIA_ERR_OK) {
            error = ROOM_MEDIA_TONE_ERROR_FEED;
            goto done;
        }
        ++enqueued;
    }
    av_render_audio_data_t eos = {.pts = TONE_DURATION_MS, .eos = true};
    if (av_render_add_audio_data(player, &eos) != ESP_MEDIA_ERR_OK) {
        error = ROOM_MEDIA_TONE_ERROR_EOS;
        goto done;
    }
    for (int waited_ms = 0; waited_ms < TONE_RENDER_WAIT_MS; waited_ms += TONE_FRAME_MS) {
        room_audio_diagnostics_snapshot_t now = {0};
        room_diagnostics_audio_get(&now);
        uint64_t delta = now.renderer_accepted - before.renderer_accepted;
        accepted = delta > UINT16_MAX ? UINT16_MAX : (uint16_t)delta;
        if (accepted >= TONE_FRAMES) break;
        vTaskDelay(pdMS_TO_TICKS(TONE_FRAME_MS));
    }
    if (accepted < TONE_FRAMES) error = ROOM_MEDIA_TONE_ERROR_RENDER_TIMEOUT;

done:
    int final_reset = av_render_reset(player);
    if (error == ROOM_MEDIA_TONE_ERROR_NONE && final_reset != ESP_MEDIA_ERR_OK) {
        error = ROOM_MEDIA_TONE_ERROR_RESET;
    }
    xSemaphoreGive(media_owner_gate);
    tone_set_result(
        error == ROOM_MEDIA_TONE_ERROR_NONE ? ROOM_MEDIA_TONE_DONE : ROOM_MEDIA_TONE_ERROR,
        error,
        enqueued,
        accepted);
    vTaskDelete(NULL);
}

esp_err_t room_media_init(room_wake_callback_t callback, void *ctx)
{
    wake_callback = callback;
    wake_callback_ctx = ctx;
    media_owner_gate = xSemaphoreCreateBinary();
    if (media_owner_gate == NULL) return ESP_ERR_NO_MEM;
    xSemaphoreGive(media_owner_gate);

    esp_codec_dev_handle_t record = NULL;
    esp_codec_dev_handle_t playback = NULL;
    ESP_RETURN_ON_ERROR(
        room_audio_codecs_init(&record, &playback),
        TAG,
        "audio codec init");
    const esp_openclaw_room_node_config_t *board = room_board_config();
    if (board->audio.configure_input_gain) {
        int input_gain_result = esp_codec_dev_set_in_gain(record, board->audio.input_gain_db);
        ESP_RETURN_ON_FALSE(
            input_gain_result == ESP_CODEC_DEV_OK,
            ESP_FAIL,
            TAG,
            "input gain set failed: %d",
            input_gain_result);
        ESP_LOGI(TAG, "configured input gain override: %.1f dB", board->audio.input_gain_db);
    } else {
        ESP_LOGI(TAG, "no input gain override configured; preserving codec/board default");
    }
    if (esp_codec_dev_set_out_vol(playback, board->audio.playback_volume) != 0) {
        return ESP_FAIL;
    }
    room_diagnostics_audio_set_volume(board->audio.playback_volume);

    room_capture_audio_aec_src_cfg_t source_cfg = {
        .mic_layout = board->audio.afe_layout,
        .record_handle = record,
        .channel = board->audio.record_channels,
        .channel_mask = board->audio.channel_mask,
        .wake_cb = room_afe_wake,
    };
    audio_source = room_capture_new_audio_aec_src(&source_cfg);
    if (audio_source == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_capture_cfg_t capture_cfg = {
        .sync_mode = ESP_CAPTURE_SYNC_MODE_AUDIO,
        .audio_src = audio_source,
    };
    if (esp_capture_open(&capture_cfg, &capture) != ESP_CAPTURE_ERR_OK) {
        return ESP_FAIL;
    }

    esp_audio_enc_register_default();
    esp_audio_dec_register_default();
    esp_capture_sink_cfg_t talk_cfg = {
        .audio_info = {
            .format_id = ESP_CAPTURE_FMT_ID_OPUS,
            .sample_rate = 16000,
            .channel = 1,
            .bits_per_sample = 16,
        },
    };
    /*
     * Sink 1 exists only to keep the AEC/AFE pipeline running whenever no Talk
     * call is active: WakeNet lives inside that pipeline, so without an
     * always-on path the AFE never fetches and ambient wake never fires.
     * Nothing consumes its frames; the wake callback comes from the AFE.
     */
    esp_capture_sink_cfg_t wake_cfg = {
        .audio_info = {
            .format_id = ESP_CAPTURE_FMT_ID_PCM,
            .sample_rate = 16000,
            .channel = 1,
            .bits_per_sample = 16,
        },
    };
    // esp_webrtc owns sink 0 and retrieves this exact pre-created path after capture starts.
    if (esp_capture_sink_setup(capture, 0, &talk_cfg, &talk_sink) != ESP_CAPTURE_ERR_OK ||
        esp_capture_sink_setup(capture, 1, &wake_cfg, &wake_sink) != ESP_CAPTURE_ERR_OK ||
        esp_capture_sink_enable(wake_sink, ESP_CAPTURE_RUN_MODE_ALWAYS) != ESP_CAPTURE_ERR_OK ||
        esp_capture_start(capture) != ESP_CAPTURE_ERR_OK) {
        return ESP_FAIL;
    }

    i2s_render_cfg_t render_cfg = {.play_handle = playback};
    audio_render_handle_t renderer = av_render_alloc_i2s_render(&render_cfg);
    if (renderer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    av_render_cfg_t player_cfg = {
        .audio_render = room_media_wrap_render(renderer),
        .audio_raw_fifo_size = 8 * 4096,
        .audio_render_fifo_size = 100 * 1024,
        .allow_drop_data = false,
    };
    player = av_render_open(&player_cfg);
    if (player == NULL) {
        return ESP_ERR_NO_MEM;
    }
    av_render_audio_frame_info_t frame_info = {
        .sample_rate = 16000,
        .channel = 2,
        .bits_per_sample = 16,
    };
    // Pinned av_render copies this pre-stream setting despite returning WRONG_STATE.
    int fixed_info_result = av_render_set_fixed_frame_info(player, &frame_info);
    if (fixed_info_result != ESP_MEDIA_ERR_OK && fixed_info_result != ESP_MEDIA_ERR_WRONG_STATE) {
        return ESP_FAIL;
    }

    if (xTaskCreateWithCaps(
            wake_drain_task,
            "wake_drain",
            3072,
            NULL,
            5,
            NULL,
            MALLOC_CAP_SPIRAM) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ambient WakeNet detections are wired from the AFE");
    ESP_LOGI(TAG, "16 kHz capture with device AEC is active");
    room_diagnostics_audio_set_capture_owner(ROOM_DIAGNOSTICS_CAPTURE_AMBIENT);
    media_initialized = true;
    return ESP_OK;
}

esp_err_t room_media_set_ambient_wake(bool enabled)
{
    if (audio_source == NULL || wake_sink == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /*
     * Change the next-open AFE config before handing capture ownership over.
     * Disabling the ambient sink stops the shared source; Talk then reopens it
     * with AEC+NLP while WakeNet/NS/VAD are disabled. After Talk closes,
     * enabling the ambient sink reopens the source with WakeNet restored.
     */
    room_diagnostics_audio_set_capture_owner(ROOM_DIAGNOSTICS_CAPTURE_TRANSITION);
    esp_capture_err_t source_err =
        room_capture_audio_aec_src_set_wakenet_enabled(audio_source, enabled);
    if (source_err != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "failed to set WakeNet mode for next AFE open: %d", source_err);
        return ESP_FAIL;
    }
    esp_capture_run_mode_t mode = enabled
        ? ESP_CAPTURE_RUN_MODE_ALWAYS
        : ESP_CAPTURE_RUN_MODE_DISABLE;
    esp_capture_err_t sink_err = esp_capture_sink_enable(wake_sink, mode);
    if (sink_err != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "failed to %s ambient capture sink: %d", enabled ? "enable" : "disable", sink_err);
        return ESP_FAIL;
    }
    ESP_LOGI(
        TAG,
        "%s owns capture; WakeNet %s for AFE reopen",
        enabled ? "ambient wake" : "Talk",
        enabled ? "enabled" : "disabled");
    room_diagnostics_audio_set_capture_owner(
        enabled ? ROOM_DIAGNOSTICS_CAPTURE_AMBIENT : ROOM_DIAGNOSTICS_CAPTURE_TALK);
    return ESP_OK;
}

esp_err_t room_media_get_webrtc_provider(esp_webrtc_media_provider_t *provider)
{
    if (provider == NULL || capture == NULL || player == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    provider->capture = capture;
    provider->player = player;
    return ESP_OK;
}

void room_media_begin_talk(void)
{
    if (media_owner_gate != NULL) {
        xSemaphoreTake(media_owner_gate, portMAX_DELAY);
        room_diagnostics_audio_set_capture_owner(ROOM_DIAGNOSTICS_CAPTURE_TRANSITION);
    }
}

void room_media_end_talk(bool capture_remained_ambient)
{
    if (capture_remained_ambient) {
        room_diagnostics_audio_set_capture_owner(ROOM_DIAGNOSTICS_CAPTURE_AMBIENT);
    }
    if (media_owner_gate != NULL) xSemaphoreGive(media_owner_gate);
}

esp_err_t room_media_request_test_tone(room_media_talk_busy_cb_t busy_cb, void *ctx)
{
    if (!media_initialized || player == NULL || media_owner_gate == NULL) {
        tone_set_result(
            ROOM_MEDIA_TONE_ERROR,
            ROOM_MEDIA_TONE_ERROR_UNAVAILABLE,
            0,
            0);
        return ESP_ERR_INVALID_STATE;
    }
    taskENTER_CRITICAL(&tone_mux);
    if (tone_task_active) {
        taskEXIT_CRITICAL(&tone_mux);
        return ESP_ERR_INVALID_STATE;
    }
    tone_task_active = true;
    taskEXIT_CRITICAL(&tone_mux);
    if (busy_cb != NULL && busy_cb(ctx)) {
        tone_set_result(ROOM_MEDIA_TONE_BUSY, ROOM_MEDIA_TONE_ERROR_NONE, 0, 0);
        return ESP_ERR_INVALID_STATE;
    }
    /* The request reserves the one media owner before scheduling work; the
     * worker inherits this ownership and releases it on every exit path. */
    if (xSemaphoreTake(media_owner_gate, 0) != pdTRUE) {
        tone_set_result(ROOM_MEDIA_TONE_BUSY, ROOM_MEDIA_TONE_ERROR_NONE, 0, 0);
        return ESP_ERR_INVALID_STATE;
    }
    taskENTER_CRITICAL(&tone_mux);
    tone_busy_cb = busy_cb;
    tone_busy_ctx = ctx;
    tone_snapshot = (room_media_tone_snapshot_t) {
        .state = ROOM_MEDIA_TONE_RUNNING,
        .requested_frames = TONE_FRAMES,
    };
    taskEXIT_CRITICAL(&tone_mux);
    if (xTaskCreateWithCaps(
            test_tone_task,
            "speaker_test",
            4096,
            NULL,
            6,
            NULL,
            MALLOC_CAP_SPIRAM) != pdPASS) {
        xSemaphoreGive(media_owner_gate);
        tone_set_result(ROOM_MEDIA_TONE_ERROR, ROOM_MEDIA_TONE_ERROR_TASK, 0, 0);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void room_media_get_tone_snapshot(room_media_tone_snapshot_t *snapshot)
{
    if (snapshot == NULL) return;
    taskENTER_CRITICAL(&tone_mux);
    *snapshot = tone_snapshot;
    taskEXIT_CRITICAL(&tone_mux);
}

const char *room_media_tone_state_name(room_media_tone_state_t state)
{
    switch (state) {
        case ROOM_MEDIA_TONE_RUNNING: return "running";
        case ROOM_MEDIA_TONE_DONE: return "done";
        case ROOM_MEDIA_TONE_BUSY: return "busy";
        case ROOM_MEDIA_TONE_ERROR: return "error";
        default: return "idle";
    }
}

const char *room_media_tone_error_name(room_media_tone_error_t error)
{
    switch (error) {
        case ROOM_MEDIA_TONE_ERROR_UNAVAILABLE: return "unavailable";
        case ROOM_MEDIA_TONE_ERROR_TASK: return "worker task";
        case ROOM_MEDIA_TONE_ERROR_RESET: return "player reset";
        case ROOM_MEDIA_TONE_ERROR_STREAM: return "PCM stream";
        case ROOM_MEDIA_TONE_ERROR_FRAME_INFO: return "frame format";
        case ROOM_MEDIA_TONE_ERROR_FEED: return "frame enqueue";
        case ROOM_MEDIA_TONE_ERROR_EOS: return "EOS enqueue";
        case ROOM_MEDIA_TONE_ERROR_RENDER_TIMEOUT: return "renderer timeout";
        default: return "none";
    }
}
