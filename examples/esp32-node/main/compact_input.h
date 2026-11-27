// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool raw, pressed, armed, consumed, pending;
    uint32_t changed_at, pressed_at, revision;
} compact_button_t;

typedef struct {
    bool initialized, encoder;
    uint8_t quadrature;
    int8_t steps;
    compact_button_t primary, secondary;
} compact_input_t;

typedef struct {
    int move;
    bool select, home;
} compact_input_event_t;

/* Pressed samples are active-high booleans, independent of board wiring.
 * A held startup key must be released before it can produce an action. */
compact_input_event_t compact_input_sample(compact_input_t *input, uint32_t now,
    uint32_t revision, bool primary, bool secondary, bool encoder, uint8_t quadrature);
/* External UI navigation consumes any physical gesture already in progress. */
void compact_input_cancel_pending(compact_input_t *input);

typedef enum { COMPACT_CONSOLE_NONE, COMPACT_CONSOLE_QUEUED, COMPACT_CONSOLE_APPLIED, COMPACT_CONSOLE_STALE } compact_console_outcome_t;
typedef struct {
    compact_input_event_t event;
    uint32_t revision, request_id, applied_count;
    compact_console_outcome_t outcome;
    bool pending;
} compact_console_input_t;

/* The board state mutex serializes these calls. One request owns the mailbox
 * through rendering; completion records input delivery, not remote success. */
bool compact_console_submit(compact_console_input_t *input, compact_input_event_t event,
    uint32_t expected_revision, uint32_t current_revision, uint32_t *request_id);
bool compact_console_complete(compact_console_input_t *input, uint32_t request_id, bool applied);
