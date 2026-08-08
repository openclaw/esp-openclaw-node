#include "room_diagnostics_data.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "room_diagnostics_metrics.h"

static portMUX_TYPE audio_mux = portMUX_INITIALIZER_UNLOCKED;
static room_audio_diagnostics_snapshot_t audio_snapshot;

void room_diagnostics_audio_get(room_audio_diagnostics_snapshot_t *snapshot)
{
    if (snapshot == NULL) return;
    taskENTER_CRITICAL(&audio_mux);
    *snapshot = audio_snapshot;
    taskEXIT_CRITICAL(&audio_mux);
}

void room_diagnostics_audio_set_capture_mode(
    room_diagnostics_afe_mode_t mode,
    bool wakenet_enabled,
    uint32_t feed_bytes,
    uint32_t fetch_bytes)
{
    taskENTER_CRITICAL(&audio_mux);
    audio_snapshot.afe_mode = mode;
    audio_snapshot.wakenet_enabled = wakenet_enabled;
    audio_snapshot.feed_bytes = feed_bytes;
    audio_snapshot.fetch_bytes = fetch_bytes;
    taskEXIT_CRITICAL(&audio_mux);
}

void room_diagnostics_audio_set_capture_owner(room_diagnostics_capture_owner_t owner)
{
    taskENTER_CRITICAL(&audio_mux);
    audio_snapshot.capture_owner = owner;
    taskEXIT_CRITICAL(&audio_mux);
}

void room_diagnostics_audio_set_volume(uint8_t volume)
{
    taskENTER_CRITICAL(&audio_mux);
    audio_snapshot.configured_volume = volume;
    taskEXIT_CRITICAL(&audio_mux);
}

void room_diagnostics_audio_record_capture_read(
    const int16_t *interleaved,
    size_t sample_count,
    size_t channel_count,
    size_t mic_channel,
    size_t reference_channel,
    bool success)
{
    uint8_t mic = 0;
    uint8_t reference = 0;
    if (success && interleaved != NULL && channel_count > 0) {
        if (mic_channel < channel_count) {
            mic = room_diagnostics_pcm_level(
                interleaved + mic_channel, sample_count - mic_channel, channel_count);
        }
        if (reference_channel < channel_count) {
            reference = room_diagnostics_pcm_level(
                interleaved + reference_channel, sample_count - reference_channel, channel_count);
        }
    }
    int64_t now_us = success ? esp_timer_get_time() : 0;
    taskENTER_CRITICAL(&audio_mux);
    if (success) {
        audio_snapshot.capture_read_successes++;
        audio_snapshot.mic_level = mic;
        audio_snapshot.playback_reference_level = reference;
        audio_snapshot.last_capture_read_us = now_us;
    } else {
        audio_snapshot.capture_read_errors++;
    }
    taskEXIT_CRITICAL(&audio_mux);
}

void room_diagnostics_audio_record_feed(bool success)
{
    taskENTER_CRITICAL(&audio_mux);
    if (success) audio_snapshot.feed_successes++;
    else audio_snapshot.feed_errors++;
    taskEXIT_CRITICAL(&audio_mux);
}

void room_diagnostics_audio_record_fetch(
    const int16_t *pcm,
    size_t sample_count,
    bool success,
    bool valid_size,
    uint8_t ringbuffer_free_percent)
{
    uint8_t level = success && valid_size
        ? room_diagnostics_pcm_level(pcm, sample_count, 1)
        : 0;
    int64_t now_us = success && valid_size ? esp_timer_get_time() : 0;
    taskENTER_CRITICAL(&audio_mux);
    if (success) audio_snapshot.fetch_successes++;
    else audio_snapshot.fetch_errors++;
    if (!valid_size) audio_snapshot.fetch_invalid_sizes++;
    audio_snapshot.ringbuffer_free_percent = ringbuffer_free_percent;
    if (success && valid_size) {
        audio_snapshot.afe_level = level;
        audio_snapshot.last_fetch_us = now_us;
    }
    taskEXIT_CRITICAL(&audio_mux);
}

void room_diagnostics_audio_record_wakenet(void)
{
    int64_t now_us = esp_timer_get_time();
    taskENTER_CRITICAL(&audio_mux);
    audio_snapshot.wakenet_detections++;
    audio_snapshot.last_wakenet_detection_us = now_us;
    taskEXIT_CRITICAL(&audio_mux);
}

void room_diagnostics_audio_record_renderer_offer(
    const int16_t *pcm,
    size_t sample_count,
    size_t channel_count,
    size_t bytes)
{
    uint8_t level = room_diagnostics_pcm_level(
        pcm, sample_count, channel_count > 0 ? channel_count : 1);
    taskENTER_CRITICAL(&audio_mux);
    audio_snapshot.renderer_level = level;
    audio_snapshot.renderer_frames_offered++;
    audio_snapshot.renderer_bytes_offered += bytes;
    taskEXIT_CRITICAL(&audio_mux);
}

void room_diagnostics_audio_record_renderer_result(bool accepted)
{
    int64_t now_us = accepted ? esp_timer_get_time() : 0;
    taskENTER_CRITICAL(&audio_mux);
    if (accepted) {
        audio_snapshot.renderer_accepted++;
        audio_snapshot.last_renderer_accepted_us = now_us;
    } else {
        audio_snapshot.renderer_errors++;
    }
    taskEXIT_CRITICAL(&audio_mux);
}
