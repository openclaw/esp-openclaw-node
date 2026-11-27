// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool raw, pressed, armed, consumed;
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
