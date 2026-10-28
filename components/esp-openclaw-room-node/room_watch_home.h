#pragma once

#include "lvgl.h"
#include "room_ui_controller.h"

/* All calls are owned by the room UI's display lock. */
void room_watch_home_create(lv_obj_t *parent, lv_event_cb_t menu_handler, lv_event_cb_t continue_handler,
                            lv_event_cb_t voice_handler, lv_event_cb_t discard_handler);
/* The controller calls this only after an accepted display brightness write. */
void room_watch_home_set_awake(bool awake);
/* Review owns the content area; the controller hides the mascot and hints. */
bool room_watch_home_review_visible(void);
/* Returns whether the retained-reader action is visible. */
bool room_watch_home_render(const room_ui_facts_t *facts, const room_ui_talk_target_t *target, room_ui_state_t state,
                           const char *detail, const char *reader_agent, const room_voice_snapshot_t *voice,
                           bool voice_enabled, bool holding, bool hidden);
