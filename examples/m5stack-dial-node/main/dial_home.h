#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "dial_directory.h"
#include "openclaw_status_bar.h"

typedef enum { DIAL_HOME_MENU, DIAL_HOME_AGENTS, DIAL_HOME_READER, DIAL_HOME_PREVIEW } dial_home_destination_t;

typedef struct {
    lv_obj_t *root, *title, *headline, *action;
    openclaw_status_bar_t status;
    lv_obj_t *eyes[2], *mouth;
    dial_home_destination_t destination;
} dial_home_t;

void dial_home_create(dial_home_t *home, lv_obj_t *parent);
void dial_home_animate(dial_home_t *home, const char *mood, uint32_t now);
void dial_home_render(dial_home_t *home, const openclaw_status_facts_t *status, bool assigned,
                      const char *activity, const dial_home_context_t *context);
