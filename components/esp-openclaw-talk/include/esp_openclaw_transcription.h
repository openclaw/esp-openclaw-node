/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_openclaw_node.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_OPENCLAW_TRANSCRIPTION_CHUNK_BYTES 3200U
#define ESP_OPENCLAW_TRANSCRIPTION_AUDIO_BYTES 240000U /* 30 seconds at 8 kHz. */
#define ESP_OPENCLAW_TRANSCRIPTION_TEXT_BYTES 2048U

typedef struct esp_openclaw_transcription *esp_openclaw_transcription_handle_t;

typedef enum {
    ESP_OPENCLAW_TRANSCRIPTION_STARTING,
    ESP_OPENCLAW_TRANSCRIPTION_READY,
    ESP_OPENCLAW_TRANSCRIPTION_SENDING,
    ESP_OPENCLAW_TRANSCRIPTION_FINISHING,
    ESP_OPENCLAW_TRANSCRIPTION_COMPLETE,
    ESP_OPENCLAW_TRANSCRIPTION_CANCELED,
    ESP_OPENCLAW_TRANSCRIPTION_FAILED,
} esp_openclaw_transcription_state_t;

typedef enum {
    ESP_OPENCLAW_TRANSCRIPTION_ERROR_NONE,
    ESP_OPENCLAW_TRANSCRIPTION_ERROR_GATEWAY,
    ESP_OPENCLAW_TRANSCRIPTION_ERROR_FORMAT,
    ESP_OPENCLAW_TRANSCRIPTION_ERROR_TOO_LONG,
    ESP_OPENCLAW_TRANSCRIPTION_ERROR_CONNECTION,
    ESP_OPENCLAW_TRANSCRIPTION_ERROR_TIMEOUT,
} esp_openclaw_transcription_error_t;

typedef struct {
    esp_openclaw_transcription_state_t state;
    esp_openclaw_transcription_error_t error;
    size_t audio_bytes;
    /** Final segments only. Sendable only when COMPLETE; empty means no speech. */
    char text[ESP_OPENCLAW_TRANSCRIPTION_TEXT_BYTES];
} esp_openclaw_transcription_snapshot_t;

/**
 * Start a transcription-only session on the current operator.talk connection.
 * No agent session, microphone, scope upgrade, or chat submission is created.
 * Serialize begin/publication with event forwarding so an early ready event
 * cannot pass through the owner before it has published the returned handle.
 * The node must outlive this handle AND its outstanding Gateway callbacks.
 */
esp_err_t esp_openclaw_transcription_begin(esp_openclaw_node_handle_t node,
    esp_openclaw_transcription_handle_t *out);
/** READY only; copies one chunk of raw G.711 mu-law / 8 kHz mono, not PCM/WAV.
 * Wait for READY again after each successful submission; never replay a chunk.
 */
esp_err_t esp_openclaw_transcription_append(esp_openclaw_transcription_handle_t handle,
    const uint8_t *audio, size_t length);
/** READY only, after the last append acknowledgement. Wait for COMPLETE,
 * not the close RPC acknowledgement: provider finals arrive during shutdown.
 */
esp_err_t esp_openclaw_transcription_finish(esp_openclaw_transcription_handle_t handle);
/** Forward borrowed talk.event payloads from the same node's event callback. */
void esp_openclaw_transcription_event(esp_openclaw_transcription_handle_t handle,
    esp_openclaw_node_handle_t source, const char *event, const char *payload_json);
/** Call periodically (e.g. every 100 ms) to enforce connection and time bounds. */
void esp_openclaw_transcription_poll(esp_openclaw_transcription_handle_t handle);
void esp_openclaw_transcription_copy(esp_openclaw_transcription_handle_t handle,
    esp_openclaw_transcription_snapshot_t *out);
/** Discard all text and close this session on its original connection only. */
void esp_openclaw_transcription_cancel(esp_openclaw_transcription_handle_t handle);
/** Cancel and try to release the owner's reference. Stop forwarding events first.
 * ESP_ERR_INVALID_STATE retains the handle while a create/audio response or
 * queue space is needed for cleanup. Keep polling and retry destruction.
 * ESP_OK releases the handle; an outstanding close callback retains it safely.
 */
esp_err_t esp_openclaw_transcription_destroy(esp_openclaw_transcription_handle_t handle);

#ifdef __cplusplus
}
#endif
