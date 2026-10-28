#pragma once

#include "lvgl.h"
#include "room_ui_controller.h"

/* All calls are owned by the room UI's display lock. */
void room_watch_status_create(lv_obj_t *parent);
void room_watch_status_render(const room_ui_facts_t *facts, bool hidden);
/* Only an accepted display brightness write changes the sampling state. */
void room_watch_status_set_awake(bool awake);
