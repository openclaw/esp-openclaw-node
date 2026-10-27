#include <assert.h>
#include <stddef.h>
#include <string.h>
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
    assert(!dial_action_complete(&state, old, DIAL_ACTION_ACCEPTED));
    assert(!dial_action_begin(&state, &current));
    assert(dial_action_connect(&state, 2, "ws://first.example"));
    assert(dial_action_begin(&state, &current));
    assert(!dial_action_complete(&state, old, DIAL_ACTION_ACCEPTED));
    assert(dial_action_matches(&state, current));
    assert(dial_action_complete(&state, current, DIAL_ACTION_ACCEPTED));
    assert(!dial_action_complete(&state, current, DIAL_ACTION_ACCEPTED));

    assert(dial_action_begin(&state, &old));
    assert(dial_action_connect(&state, 3, "ws://second.example"));
    assert(state.configured_uri == NULL);
    assert(!dial_action_begin(&state, &current));
    assert(!dial_action_complete(&state, old, DIAL_ACTION_ACCEPTED));
    assert(dial_action_configure(&state, true));
    assert(dial_action_begin(&state, &current));
    assert(!dial_action_matches(&state, old));
    assert(dial_action_complete(&state, current, DIAL_ACTION_ACCEPTED));
    assert(dial_action_configure(&state, false));
    assert(!dial_action_begin(&state, &current));
    assert(dial_action_connect(&state, 0, NULL));

    /* Application completion may beat the transport acknowledgment. Neither
     * the late acknowledgment nor a duplicate result may replace it. */
    assert(dial_action_connect(&state, 4, "ws://first.example"));
    assert(dial_action_configure(&state, true));
    assert(dial_action_begin(&state, &current));
    assert(dial_action_resolve(&state, current, true, "Done"));
    assert(!dial_action_complete(&state, current, DIAL_ACTION_ACCEPTED));
    assert(!dial_action_resolve(&state, current, true, "Done"));

    assert(dial_action_begin(&state, &old));
    assert(dial_action_complete(&state, old, DIAL_ACTION_ACCEPTED));
    assert(dial_action_begin(&state, &current));
    assert(!dial_action_resolve(&state, old, true, "Done"));
    assert(dial_action_complete(&state, current, DIAL_ACTION_ACCEPTED));
    assert(dial_action_resolve(&state, current, true, "Done"));

    /* Replacing a control and reconnecting each invalidate old outcomes. */
    assert(dial_action_begin(&state, &old));
    assert(dial_action_complete(&state, old, DIAL_ACTION_ACCEPTED));
    assert(dial_action_configure(&state, true));
    assert(!dial_action_resolve(&state, old, true, "Done"));
    assert(dial_action_begin(&state, &old));
    assert(dial_action_complete(&state, old, DIAL_ACTION_ACCEPTED));
    assert(dial_action_connect(&state, 0, NULL));
    assert(dial_action_connect(&state, 5, "ws://first.example"));
    assert(!dial_action_resolve(&state, old, true, "Done"));
    assert(dial_action_begin(&state, &current));
    assert(!dial_action_resolve(&state, (dial_action_token_t){4, current.attempt}, true, "Done"));
    assert(dial_action_complete(&state, current, DIAL_ACTION_ACCEPTED));
    assert(dial_action_resolve(&state, current, true, "Done"));
    assert(dial_action_configure(&state, false));
    assert(dial_action_connect(&state, 0, NULL));
    /* The record owns the submitted display value, even when drafts change. */
    assert(dial_action_connect(&state, 6, "ws://first.example"));
    assert(dial_action_configure(&state, true));
    assert(dial_action_begin(&state, &current));
    char label[] = "Desk light", unit[] = "%";
    dial_action_submitted(&state, current, label, 45, unit);
    memset(label, 'x', strlen(label));
    unit[0] = 'x';
    assert(state.activity.stage == DIAL_ACTIVITY_SENDING);
    assert(strcmp(state.activity.label, "Desk light") == 0);
    assert(strcmp(state.activity.unit, "%") == 0 && state.activity.value == 45);
    unsigned revision = state.activity.revision;
    assert(!dial_action_configure(&state, true));
    assert(state.activity.revision == revision);
    assert(dial_action_complete(&state, current, DIAL_ACTION_ACCEPTED));
    assert(state.activity.stage == DIAL_ACTIVITY_WAITING);
    assert(dial_action_resolve(&state, current, false, "Device refused"));
    assert(state.activity.stage == DIAL_ACTIVITY_FAILED);
    assert(strcmp(state.activity.message, "Device refused") == 0);
    revision = state.activity.revision;
    assert(!dial_action_complete(&state, current, DIAL_ACTION_ACCEPTED));
    assert(!dial_action_resolve(&state, current, true, "Late duplicate"));
    assert(dial_action_connect(&state, 0, NULL));
    assert(state.activity.revision == revision);

    /* An ambiguous send can still receive a matching, authoritative result. */
    assert(dial_action_connect(&state, 7, "ws://first.example"));
    assert(dial_action_begin(&state, &current));
    dial_action_submitted(&state, current, "Desk light", 60, "%");
    assert(dial_action_complete(&state, current, DIAL_ACTION_UNCERTAIN));
    assert(state.activity.stage == DIAL_ACTIVITY_UNKNOWN);
    assert(state.result_attempt == current.attempt);
    assert(dial_action_resolve(&state, current, true, "Light set to 60%"));
    assert(state.activity.stage == DIAL_ACTIVITY_COMPLETED);
    revision = state.activity.revision;
    assert(dial_action_configure(&state, true));
    assert(state.activity.revision == revision);

    /* Rebinding and reconnecting invalidate unfinished outcomes, never values. */
    assert(dial_action_begin(&state, &old));
    dial_action_submitted(&state, old, "Heater", 21, "C");
    assert(dial_action_complete(&state, old, DIAL_ACTION_ACCEPTED));
    assert(dial_action_configure(&state, true));
    assert(state.activity.stage == DIAL_ACTIVITY_UNKNOWN && state.activity.value == 21);
    assert(!dial_action_resolve(&state, old, true, "Old binding"));
    assert(dial_action_begin(&state, &current));
    dial_action_submitted(&state, current, "Fan", 2, "");
    assert(dial_action_connect(&state, 0, NULL));
    assert(state.activity.stage == DIAL_ACTIVITY_UNKNOWN && state.activity.value == 2);
    assert(dial_action_connect(&state, 8, "ws://first.example"));
    assert(!dial_action_resolve(&state, current, true, "Old connection"));

    /* A failed local admission preserves the previous record. Once queued,
     * a request that cannot enter the SDK is definitively Not sent. */
    assert(dial_action_begin(&state, &current));
    revision = state.activity.revision;
    assert(dial_action_complete(&state, current, DIAL_ACTION_NOT_SENT));
    assert(state.activity.revision == revision && state.activity.value == 2);
    assert(dial_action_begin(&state, &current));
    dial_action_submitted(&state, current, "Fan", 3, "");
    assert(dial_action_complete(&state, current, DIAL_ACTION_NOT_SENT));
    assert(state.activity.stage == DIAL_ACTIVITY_NOT_SENT);
    assert(!dial_action_resolve(&state, current, true, "Never sent"));
    assert(dial_action_connect(&state, 0, NULL));
    assert(dial_action_configure(&state, false));
    return 0;
}
