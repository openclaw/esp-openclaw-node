#pragma once

#include "lvgl.h"
#include "room_ui_controller.h"

/* All calls are owned by the room UI's display lock. */
void room_watch_home_create(lv_obj_t *parent, lv_event_cb_t menu_handler);
void room_watch_home_render(const room_ui_facts_t *facts, room_ui_state_t state,
                            const char *detail, bool hidden);
