#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ROOM_VOICE_UNAVAILABLE = 0,
    ROOM_VOICE_READY,
    ROOM_VOICE_STARTING,
    ROOM_VOICE_RECORDING,
    ROOM_VOICE_FINISHING,
    ROOM_VOICE_DRAFT,
    ROOM_VOICE_ERROR,
} room_voice_state_t;

typedef enum { ROOM_VOICE_BEGIN, ROOM_VOICE_FINISH, ROOM_VOICE_CANCEL } room_voice_action_t;

typedef struct {
    room_voice_state_t state;
    uint32_t generation;
    uint32_t duration_ms;
    uint64_t recipient_revision;
    uint64_t owner_incarnation;
    char recipient[65];
    char detail[96];
} room_voice_snapshot_t;

typedef struct {
    bool valid;
    bool available;
    uint64_t recipient_revision;
    uint64_t owner_incarnation;
    char recipient[65];
} room_voice_context_t;

/* Display callbacks record bounded intent without waiting for audio or the
 * room owner. Generations increase across gestures; cancel is sticky. */
void room_voice_draft_request(room_voice_action_t action, uint32_t generation,
                             uint64_t recipient_revision, uint64_t owner_incarnation);
bool room_voice_draft_needs_poll(void);
/* The room supervisor is the sole caller and owns the unsent audio allocation.
 * Returns true when the copied UI snapshot changed. No network submission. */
bool room_voice_draft_poll(const room_voice_context_t *context, room_voice_snapshot_t *result);
