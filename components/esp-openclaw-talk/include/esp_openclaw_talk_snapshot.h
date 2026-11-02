#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ESP_OPENCLAW_TALK_TEXT_BYTES 1024U
#define ESP_OPENCLAW_TALK_TURN_ID_BYTES 128U

typedef enum {
    ESP_OPENCLAW_TALK_STARTING,
    ESP_OPENCLAW_TALK_LISTENING,
    ESP_OPENCLAW_TALK_THINKING,
    ESP_OPENCLAW_TALK_SPEAKING,
    ESP_OPENCLAW_TALK_ERROR,
    ESP_OPENCLAW_TALK_CLOSED,
} esp_openclaw_talk_phase_t;

/** Latest turn only. Output activity describes Gateway events, not audible playback. */
typedef struct {
    uint64_t sequence;
    esp_openclaw_talk_phase_t phase;
    char turn_id[ESP_OPENCLAW_TALK_TURN_ID_BYTES];
    char user_text[ESP_OPENCLAW_TALK_TEXT_BYTES];
    char agent_text[ESP_OPENCLAW_TALK_TEXT_BYTES];
    bool turn_active;
    bool user_final;
    bool agent_final;
    /** Lost deltas or bounded text truncation; a full final can repair its own text. */
    bool user_incomplete;
    bool agent_incomplete;
} esp_openclaw_talk_snapshot_t;

