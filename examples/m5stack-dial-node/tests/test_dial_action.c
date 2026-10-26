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

    /* Application completion may beat the transport acknowledgment. Neither
     * the late acknowledgment nor a duplicate result may replace it. */
    assert(dial_action_connect(&state, 4, "ws://first.example"));
    assert(dial_action_configure(&state, true));
    assert(dial_action_begin(&state, &current));
    assert(dial_action_resolve(&state, current));
    assert(!dial_action_complete(&state, current));
    assert(!dial_action_resolve(&state, current));

    assert(dial_action_begin(&state, &old));
    assert(dial_action_complete(&state, old));
    assert(dial_action_begin(&state, &current));
    assert(!dial_action_resolve(&state, old));
    assert(dial_action_complete(&state, current));
    assert(dial_action_resolve(&state, current));

    /* Replacing a control and reconnecting each invalidate old outcomes. */
    assert(dial_action_begin(&state, &old));
    assert(dial_action_complete(&state, old));
    assert(dial_action_configure(&state, true));
    assert(!dial_action_resolve(&state, old));
    assert(dial_action_begin(&state, &old));
    assert(dial_action_complete(&state, old));
    assert(dial_action_connect(&state, 0, NULL));
    assert(dial_action_connect(&state, 5, "ws://first.example"));
    assert(!dial_action_resolve(&state, old));
    assert(dial_action_begin(&state, &current));
    assert(!dial_action_resolve(&state, (dial_action_token_t){4, current.attempt}));
    assert(dial_action_complete(&state, current));
    assert(dial_action_resolve(&state, current));
    assert(dial_action_connect(&state, 0, NULL));
    return 0;
}
