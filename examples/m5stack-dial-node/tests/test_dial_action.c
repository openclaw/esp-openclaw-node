#include <assert.h>
#include <stddef.h>
#include "dial_action.h"

int main(void)
{
    dial_action_state_t state = {0};
    dial_action_token_t old, current;
    assert(!dial_action_begin(&state, &old));
    assert(!dial_action_configure(&state, true));
    assert(dial_action_connect(&state, 1, "ws://first.example"));
    assert(dial_action_configure(&state, true));
    assert(dial_action_begin(&state, &old));
    assert(!dial_action_begin(&state, &current));
    assert(!dial_action_configure(&state, false));

    assert(dial_action_connect(&state, 0, NULL));
    assert(state.configured_uri != NULL);
    assert(!dial_action_complete(&state, old));
    assert(!dial_action_begin(&state, &current));
    assert(dial_action_connect(&state, 2, "ws://first.example"));
    assert(dial_action_begin(&state, &current));
    assert(!dial_action_complete(&state, old));
    assert(dial_action_matches(&state, current));
    assert(dial_action_complete(&state, current));
    assert(!dial_action_complete(&state, current));

    assert(dial_action_begin(&state, &old));
    assert(dial_action_connect(&state, 3, "ws://second.example"));
    assert(state.configured_uri == NULL);
    assert(!dial_action_begin(&state, &current));
    assert(!dial_action_complete(&state, old));
    assert(dial_action_configure(&state, true));
    assert(dial_action_begin(&state, &current));
    assert(!dial_action_matches(&state, old));
    assert(dial_action_complete(&state, current));
    assert(dial_action_configure(&state, false));
    assert(!dial_action_begin(&state, &current));
    assert(dial_action_connect(&state, 0, NULL));
    return 0;
}
