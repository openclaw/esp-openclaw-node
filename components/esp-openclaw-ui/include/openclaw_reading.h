#pragma once

#include <stdbool.h>
#include "lvgl.h"

typedef enum { OPENCLAW_READING_STANDARD, OPENCLAW_READING_LARGE } openclaw_reading_size_t;

/* Runtime-only preference, owned by the board's LVGL task/display lock. */
bool openclaw_reading_set_size(openclaw_reading_size_t size);
openclaw_reading_size_t openclaw_reading_size(void);
const lv_font_t *openclaw_reading_font(void);
