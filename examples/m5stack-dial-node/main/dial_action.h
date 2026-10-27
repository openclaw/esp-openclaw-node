#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t connection_id;
    uint64_t attempt;
} dial_action_token_t;

typedef enum {
    DIAL_ACTIVITY_EMPTY, DIAL_ACTIVITY_SENDING, DIAL_ACTIVITY_WAITING,
    DIAL_ACTIVITY_COMPLETED, DIAL_ACTIVITY_FAILED, DIAL_ACTIVITY_NOT_SENT, DIAL_ACTIVITY_UNKNOWN,
} dial_activity_stage_t;

typedef struct {
    unsigned revision;
    dial_action_token_t token;
    dial_activity_stage_t stage;
    int value;
    char label[65], unit[17], message[129];
} dial_activity_t;

typedef enum {
    DIAL_ACTION_ACCEPTED, DIAL_ACTION_NOT_SENT, DIAL_ACTION_UNCERTAIN,
} dial_action_delivery_t;

/* The caller serializes access. URI strings belong to this state. */
typedef struct {
    char *gateway_uri, *configured_uri;
    uint32_t connection_id;
    uint64_t next_attempt, pending_attempt, result_attempt;
    dial_activity_t activity;
} dial_action_state_t;

bool dial_action_connect(dial_action_state_t *state, uint32_t id, const char *uri);
bool dial_action_configure(dial_action_state_t *state, bool enabled);
bool dial_action_begin(dial_action_state_t *state, dial_action_token_t *token);
bool dial_action_matches(const dial_action_state_t *state, dial_action_token_t token);
/* Record only after the request enters the worker queue. */
void dial_action_submitted(dial_action_state_t *state, dial_action_token_t token,
                           const char *label, int value, const char *unit);
bool dial_action_complete(dial_action_state_t *state, dial_action_token_t token,
                          dial_action_delivery_t delivery);
/** Accept one application result for the latest press on this connection. */
bool dial_action_resolve(dial_action_state_t *state, dial_action_token_t token,
                         bool success, const char *message);
const char *dial_activity_title(dial_activity_stage_t stage);
