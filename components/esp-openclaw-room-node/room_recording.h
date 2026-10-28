#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef enum {
    ROOM_RECORDING_IDLE = 0,
    ROOM_RECORDING_STARTING,
    ROOM_RECORDING_ACTIVE,
    ROOM_RECORDING_STOPPING,
    ROOM_RECORDING_READY,
    ROOM_RECORDING_CANCELED,
    ROOM_RECORDING_FAILED,
} room_recording_state_t;

typedef enum {
    ROOM_RECORDING_ERROR_NONE = 0,
    ROOM_RECORDING_ERROR_MEMORY,
    ROOM_RECORDING_ERROR_CODEC,
    ROOM_RECORDING_ERROR_OVERFLOW,
    ROOM_RECORDING_ERROR_SOURCE,
    ROOM_RECORDING_ERROR_EMPTY,
} room_recording_error_t;

typedef struct {
    room_recording_state_t state;
    room_recording_error_t error;
    size_t audio_bytes;
} room_recording_snapshot_t;

/* Internal media owner: reserves the shared gate until worker cleanup finishes. */
esp_err_t room_recording_start(SemaphoreHandle_t media_gate);
bool room_recording_active(void);
/* Copies mono PCM16 at 16 kHz before the caller releases its capture frame.
 * Never waits for the worker. Malformed frames or overflow fail the recording. */
void room_recording_feed(const void *pcm, size_t bytes);
void room_recording_stop(void);
void room_recording_cancel(void);
void room_recording_snapshot(room_recording_snapshot_t *snapshot);
/* Transfers a completed, at most 30 second, G.711 mu-law 8 kHz mono draft.
 * The caller owns the returned allocation and must free it. */
esp_err_t room_recording_take(uint8_t **audio, size_t *bytes);
