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
#include "freertos/idf_additions.h"
#include "media_lib_err.h"
#include "room_aec_src.h"
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

static audio_render_handle_t render_tap_init(void *cfg, int cfg_size)
{
    (void)cfg;
    (void)cfg_size;
    return render_tap_target;
}

static int render_tap_open(audio_render_handle_t render, av_render_audio_frame_info_t *info)
{
    return audio_render_open(render, info);
}

static int render_tap_write(audio_render_handle_t render, av_render_audio_frame_t *frame)
{
    if (frame != NULL && frame->data != NULL && frame->size >= 2) {
        const int16_t *samples = (const int16_t *)frame->data;
        size_t count = (size_t)frame->size / 2;
        /* Stride so any frame costs at most ~128 reads on the render task. */
        size_t step = count / 128 + 1;
        uint32_t sum = 0;
        size_t taken = 0;
        for (size_t i = 0; i < count; i += step) {
            int32_t value = samples[i];
            sum += (uint32_t)(value < 0 ? -value : value);
            ++taken;
        }
        uint32_t mean = taken > 0 ? sum / taken : 0;
        /* Speech mean-abs rarely exceeds ~4000 at our volume; clamp to full open. */
        uint32_t level = mean >= 4000 ? 255 : (mean * 255) / 4000;
        room_face_set_speech_level((uint8_t)level);
    }
    return audio_render_write(render, frame);
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

esp_err_t room_media_init(room_wake_callback_t callback, void *ctx)
{
    wake_callback = callback;
    wake_callback_ctx = ctx;

    esp_codec_dev_handle_t record = NULL;
    esp_codec_dev_handle_t playback = NULL;
    ESP_RETURN_ON_ERROR(
        room_audio_codecs_init(&record, &playback),
        TAG,
        "audio codec init");
    const esp_openclaw_room_node_config_t *board = room_board_config();
    if (esp_codec_dev_set_out_vol(playback, board->audio.playback_volume) != 0) {
        return ESP_FAIL;
    }

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
    ESP_LOGI(TAG, "24 kHz dual-channel capture and device AEC are active");
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
     * with AEC and VAD intact but WakeNet absent. After Talk closes, enabling
     * the ambient sink reopens the source with WakeNet restored.
     */
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
