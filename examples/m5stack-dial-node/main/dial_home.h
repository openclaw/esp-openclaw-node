#pragma once

#include <stdbool.h>

#include "lvgl.h"
#include "dial_directory.h"

typedef enum { DIAL_HOME_MENU, DIAL_HOME_AGENTS, DIAL_HOME_READER, DIAL_HOME_PREVIEW } dial_home_destination_t;

typedef struct {
    lv_obj_t *root, *wifi, *link, *title, *headline, *action;
    dial_home_destination_t destination;
} dial_home_t;

void dial_home_create(dial_home_t *home, lv_obj_t *parent);
void dial_home_render(dial_home_t *home, bool wifi, bool connected, bool assigned,
                      const char *activity, const dial_home_context_t *context);
