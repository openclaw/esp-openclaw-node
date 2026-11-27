// SPDX-License-Identifier: Apache-2.0
#include "compact_input.h"

enum { DEBOUNCE_MS = 25, HOLD_MS = 650 };

static void button(compact_button_t *key, bool pressed, uint32_t now,
                   uint32_t revision, bool primary, compact_input_event_t *event)
{
    if (pressed != key->raw) {
        key->raw = pressed;
        key->changed_at = now;
        if (pressed && !key->pending) {
            key->pending = true;
            key->revision = revision;
            key->consumed = false;
        }
    }
    if (now - key->changed_at < DEBOUNCE_MS) return;
    if (pressed != key->pressed) {
        key->pressed = pressed;
        if (pressed) {
            key->pressed_at = now;
        } else {
            if (key->armed && key->pending && !key->consumed && key->revision == revision) {
                if (primary) event->select = true;
                else event->move = 1;
            }
            key->armed = true;
        }
    }
    if (!pressed) { key->armed = true; key->pending = false; }
    if (pressed && key->armed && !key->consumed && key->revision == revision &&
        now - key->pressed_at >= HOLD_MS) {
        key->consumed = true;
        if (primary) event->home = true;
        else event->move = -1;
    }
}

compact_input_event_t compact_input_sample(compact_input_t *input, uint32_t now,
    uint32_t revision, bool primary, bool secondary, bool encoder, uint8_t quadrature)
{
    compact_input_event_t event = {0};
    quadrature &= 3;
    if (!input->initialized) {
        *input = (compact_input_t){.initialized = true, .encoder = encoder, .quadrature = quadrature,
            .primary = {.raw = primary, .pressed = primary, .changed_at = now},
            .secondary = {.raw = secondary, .pressed = secondary, .changed_at = now}};
        return event;
    }
    button(&input->primary, primary, now, revision, true, &event);
    button(&input->secondary, secondary, now, revision, false, &event);
    if (encoder && input->encoder && quadrature != input->quadrature) {
        if (input->primary.pending || event.select) {
            input->primary.consumed = true;
            event.select = false;
        }
        static const int8_t transitions[16] = {
            0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0
        };
        int delta = transitions[(input->quadrature << 2) | quadrature];
        input->steps = delta == 0 ? 0 : input->steps + delta;
        if (input->steps >= 4 || input->steps <= -4) {
            event.move = input->steps > 0 ? 1 : -1;
            input->steps = 0;
            if (input->primary.pressed) input->primary.consumed = true;
        }
    }
    input->encoder = encoder;
    input->quadrature = quadrature;
    if (event.move) {
        event.select = false;
        if (input->primary.pending) input->primary.consumed = true;
    }
    return event;
}
