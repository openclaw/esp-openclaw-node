#pragma once

#include <stdbool.h>

/* The arc represents discrete positions, keeping touch and rotary input on
 * the same grid and bounding the work needed to render a configured range. */
typedef struct {
    int minimum;
    int maximum;
    int step;
    int position;
} dial_control_t;

bool dial_control_configure(dial_control_t *control, double minimum,
                            double maximum, double step, double value);
int dial_control_value(const dial_control_t *control);
int dial_control_positions(const dial_control_t *control);
bool dial_control_move(dial_control_t *control, int position);
