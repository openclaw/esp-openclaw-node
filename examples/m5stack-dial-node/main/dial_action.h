#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t connection_id;
    uint64_t attempt;
} dial_action_token_t;

/* The caller serializes access. URI strings belong to this state. */
typedef struct {
    char *gateway_uri, *configured_uri;
    uint32_t connection_id;
    uint64_t next_attempt, pending_attempt, result_attempt;
} dial_action_state_t;

bool dial_action_connect(dial_action_state_t *state, uint32_t id, const char *uri);
bool dial_action_configure(dial_action_state_t *state, bool enabled);
bool dial_action_begin(dial_action_state_t *state, dial_action_token_t *token);
bool dial_action_matches(const dial_action_state_t *state, dial_action_token_t token);
bool dial_action_complete(dial_action_state_t *state, dial_action_token_t token);
/** Accept one application result for the latest press on this connection. */
bool dial_action_resolve(dial_action_state_t *state, dial_action_token_t token);
