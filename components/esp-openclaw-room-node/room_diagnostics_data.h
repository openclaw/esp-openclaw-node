#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    ROOM_DIAGNOSTICS_AFE_UNKNOWN = 0,
    ROOM_DIAGNOSTICS_AFE_AMBIENT_SR,
    ROOM_DIAGNOSTICS_AFE_TALK_VC,
} room_diagnostics_afe_mode_t;

typedef enum {
    ROOM_DIAGNOSTICS_CAPTURE_TRANSITION = 0,
    ROOM_DIAGNOSTICS_CAPTURE_AMBIENT,
    ROOM_DIAGNOSTICS_CAPTURE_TALK,
} room_diagnostics_capture_owner_t;

typedef struct {
    uint8_t mic_level;
    uint8_t playback_reference_level;
    uint8_t afe_level;
    uint8_t renderer_level;
    room_diagnostics_afe_mode_t afe_mode;
    room_diagnostics_capture_owner_t capture_owner;
    bool wakenet_enabled;
    uint8_t configured_volume;
    float configured_playback_gain_db;
    uint8_t ringbuffer_free_percent;
    uint32_t feed_bytes;
    uint32_t fetch_bytes;
    uint64_t capture_read_successes;
    uint64_t capture_read_errors;
    uint64_t feed_successes;
    uint64_t feed_errors;
    uint64_t fetch_successes;
    uint64_t fetch_errors;
    uint64_t fetch_invalid_sizes;
    uint64_t wakenet_detections;
    uint64_t renderer_frames_offered;
    uint64_t renderer_bytes_offered;
    uint64_t renderer_accepted;
    uint64_t renderer_errors;
    int64_t last_capture_read_us;
    int64_t last_fetch_us;
    int64_t last_wakenet_detection_us;
    int64_t last_renderer_accepted_us;
} room_audio_diagnostics_snapshot_t;

void room_diagnostics_audio_get(room_audio_diagnostics_snapshot_t *snapshot);
void room_diagnostics_audio_set_capture_mode(
    room_diagnostics_afe_mode_t mode,
    bool wakenet_enabled,
    uint32_t feed_bytes,
    uint32_t fetch_bytes);
void room_diagnostics_audio_set_capture_owner(room_diagnostics_capture_owner_t owner);
void room_diagnostics_audio_set_output(uint8_t volume, float playback_gain_db);
void room_diagnostics_audio_record_capture_read(
    const int16_t *interleaved,
    size_t sample_count,
    size_t channel_count,
    size_t mic_channel,
    size_t reference_channel,
    bool success);
void room_diagnostics_audio_record_feed(bool success);
void room_diagnostics_audio_record_fetch(
    const int16_t *pcm,
    size_t sample_count,
    bool success,
    bool valid_size,
    uint8_t ringbuffer_free_percent);
void room_diagnostics_audio_record_wakenet(void);
void room_diagnostics_audio_record_renderer_offer(
    const int16_t *pcm,
    size_t sample_count,
    size_t channel_count,
    size_t bytes);
void room_diagnostics_audio_record_renderer_result(bool accepted);
