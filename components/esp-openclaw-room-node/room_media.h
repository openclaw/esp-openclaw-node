#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_webrtc.h"

typedef void (*room_wake_callback_t)(const char *wake_word, void *ctx);
typedef bool (*room_media_talk_busy_cb_t)(void *ctx);

typedef enum {
    ROOM_MEDIA_TONE_IDLE = 0,
    ROOM_MEDIA_TONE_RUNNING,
    ROOM_MEDIA_TONE_DONE,
    ROOM_MEDIA_TONE_BUSY,
    ROOM_MEDIA_TONE_ERROR,
} room_media_tone_state_t;

typedef enum {
    ROOM_MEDIA_TONE_ERROR_NONE = 0,
    ROOM_MEDIA_TONE_ERROR_UNAVAILABLE,
    ROOM_MEDIA_TONE_ERROR_TASK,
    ROOM_MEDIA_TONE_ERROR_RESET,
    ROOM_MEDIA_TONE_ERROR_STREAM,
    ROOM_MEDIA_TONE_ERROR_FRAME_INFO,
    ROOM_MEDIA_TONE_ERROR_FEED,
    ROOM_MEDIA_TONE_ERROR_EOS,
    ROOM_MEDIA_TONE_ERROR_RENDER_TIMEOUT,
} room_media_tone_error_t;

typedef struct {
    room_media_tone_state_t state;
    room_media_tone_error_t error;
    uint16_t requested_frames;
    uint16_t enqueued_frames;
    uint16_t renderer_accepted_frames;
} room_media_tone_snapshot_t;

esp_err_t room_media_init(room_wake_callback_t callback, void *ctx);
/** Enable or disable the ambient wake scan path (disable while Talk owns the pipeline). */
esp_err_t room_media_set_ambient_wake(bool enabled);

esp_err_t room_media_get_webrtc_provider(esp_webrtc_media_provider_t *provider);

/** Serialize the full Talk lifetime against the local speaker test. */
void room_media_begin_talk(void);
void room_media_end_talk(bool capture_remained_ambient);

/** Queue the local speaker test; generated and fed from a worker task. */
esp_err_t room_media_request_test_tone(room_media_talk_busy_cb_t busy_cb, void *ctx);
void room_media_get_tone_snapshot(room_media_tone_snapshot_t *snapshot);
const char *room_media_tone_state_name(room_media_tone_state_t state);
const char *room_media_tone_error_name(room_media_tone_error_t error);
