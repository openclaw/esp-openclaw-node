#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_openclaw_transcription.h"
#include "esp_openclaw_chat.h"

typedef enum {
    ROOM_VOICE_UNAVAILABLE = 0,
    ROOM_VOICE_READY,
    ROOM_VOICE_STARTING,
    ROOM_VOICE_RECORDING,
    ROOM_VOICE_FINISHING,
    ROOM_VOICE_TRANSCRIBING,
    ROOM_VOICE_CANCELING,
    ROOM_VOICE_DRAFT,
    ROOM_VOICE_ERROR,
    ROOM_VOICE_TRANSCRIPTION_ERROR,
    ROOM_VOICE_SENDING,
    ROOM_VOICE_ACCEPTED,
    ROOM_VOICE_SENT,
    ROOM_VOICE_SEND_UNKNOWN,
    ROOM_VOICE_SEND_FAILED,
} room_voice_state_t;

typedef enum { ROOM_VOICE_BEGIN, ROOM_VOICE_FINISH, ROOM_VOICE_CANCEL, ROOM_VOICE_SEND, ROOM_VOICE_HIDE } room_voice_action_t;

typedef struct {
    room_voice_state_t state;
    uint32_t generation;
    uint32_t duration_ms;
    uint64_t recipient_revision;
    uint64_t owner_incarnation;
    char recipient[65];
    char detail[96];
    char text[ESP_OPENCLAW_TRANSCRIPTION_TEXT_BYTES];
    bool can_send;
    char agent_id[65];
} room_voice_snapshot_t;

typedef struct {
    bool valid;
    bool available;
    bool send_enabled;
    esp_openclaw_node_handle_t operator_node;
    uint64_t recipient_revision;
    uint64_t owner_incarnation;
    char recipient[65];
    char agent_id[65];
    char session_key[ESP_OPENCLAW_CHAT_KEY_BYTES];
} room_voice_context_t;

/* Display callbacks record bounded intent without waiting for audio or the
 * room owner. Generations increase across gestures; cancel is sticky. */
void room_voice_draft_request(room_voice_action_t action, uint32_t generation,
                             uint64_t recipient_revision, uint64_t owner_incarnation);
bool room_voice_draft_needs_poll(void);
/* Poll and event forwarding share the room state lock. The operator node is
 * process-lived, including while accepted cleanup callbacks drain. */
void room_voice_draft_event(esp_openclaw_node_handle_t source, const char *event,
                           const char *payload, uint64_t revision, uint64_t owner);
bool room_voice_draft_busy(void);
/* Reconnecting would discard an editable draft or interrupt a submitted chat. */
bool room_voice_draft_connection_busy(void);
/* The supervisor owns capture, transcription, explicit Send and cleanup.
 * Transcription alone never submits chat. Dismissing a sent view never aborts. */
bool room_voice_draft_poll(const room_voice_context_t *context, room_voice_snapshot_t *result);
