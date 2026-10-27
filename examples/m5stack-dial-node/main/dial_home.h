#pragma once

#include <stdbool.h>

#include "lvgl.h"

typedef struct {
    lv_obj_t *root, *wifi, *link, *headline, *action;
} dial_home_t;

void dial_home_create(dial_home_t *home, lv_obj_t *parent);
void dial_home_render(dial_home_t *home, bool wifi, bool connected, bool assigned,
                      const char *activity);
