#include "dial_action.h"

#include <stdlib.h>
#include <string.h>

static char *copy_uri(const char *uri)
{
    char *copy = malloc(strlen(uri) + 1);
    if (copy != NULL) strcpy(copy, uri);
    return copy;
}

bool dial_action_connect(dial_action_state_t *state, uint32_t id, const char *uri)
{
    char *copy = id != 0 && uri != NULL ? copy_uri(uri) : NULL;
    free(state->gateway_uri);
    state->gateway_uri = copy;
    state->connection_id = copy != NULL ? id : 0;
    state->pending_attempt = 0;
    state->result_attempt = 0;
    if (copy != NULL && state->configured_uri != NULL &&
        strcmp(copy, state->configured_uri) != 0) {
        free(state->configured_uri);
        state->configured_uri = NULL;
    }
    return id == 0 || copy != NULL;
}

bool dial_action_configure(dial_action_state_t *state, bool enabled)
{
    if (state->pending_attempt != 0) return false;
    if (enabled && state->gateway_uri == NULL) return false;
    char *copy = enabled ? copy_uri(state->gateway_uri) : NULL;
    if (enabled && copy == NULL) return false;
    free(state->configured_uri);
    state->configured_uri = copy;
    state->result_attempt = 0;
    return true;
}

bool dial_action_begin(dial_action_state_t *state, dial_action_token_t *token)
{
    if (state->connection_id == 0 || state->configured_uri == NULL ||
        state->pending_attempt != 0) return false;
    if (++state->next_attempt == 0) ++state->next_attempt;
    state->pending_attempt = state->next_attempt;
    state->result_attempt = state->next_attempt;
    *token = (dial_action_token_t){state->connection_id, state->pending_attempt};
    return true;
}

bool dial_action_matches(const dial_action_state_t *state, dial_action_token_t token)
{
    return token.connection_id != 0 && token.connection_id == state->connection_id &&
        token.attempt != 0 && token.attempt == state->pending_attempt;
}

bool dial_action_complete(dial_action_state_t *state, dial_action_token_t token)
{
    if (!dial_action_matches(state, token)) return false;
    state->pending_attempt = 0;
    return true;
}

bool dial_action_resolve(dial_action_state_t *state, dial_action_token_t token)
{
    if (token.connection_id == 0 || token.connection_id != state->connection_id ||
        token.attempt == 0 || token.attempt != state->result_attempt) return false;
    state->result_attempt = 0;
    if (state->pending_attempt == token.attempt) state->pending_attempt = 0;
    return true;
}
