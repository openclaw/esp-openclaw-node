#pragma once

#include <stdbool.h>

typedef enum { OPENCLAW_DENSITY_COMPACT, OPENCLAW_DENSITY_COMFORTABLE } openclaw_density_t;

/* Runtime-only list spacing, owned by the board's LVGL task/display lock. */
bool openclaw_density_set(openclaw_density_t value);
openclaw_density_t openclaw_density(void);
int openclaw_density_row_height(bool secondary);
int openclaw_density_row_gap(void);
