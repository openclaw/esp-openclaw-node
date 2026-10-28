#pragma once

#include "lvgl.h"
#include "room_ui_controller.h"

typedef enum { ROOM_HOME_NONE, ROOM_HOME_CONTINUE, ROOM_HOME_AGENTS, ROOM_HOME_CONNECTION } room_home_action_t;

/* All calls are owned by the room UI's display lock. */
void room_watch_home_create(lv_obj_t *parent, lv_event_cb_t menu_handler, lv_event_cb_t agent_handler, lv_event_cb_t continue_handler,
                            lv_event_cb_t voice_handler, lv_event_cb_t discard_handler, lv_event_cb_t send_handler);
/* Review owns the content area; the controller hides the mascot and hints. */
bool room_watch_home_review_visible(void);
void room_watch_home_render(const room_ui_facts_t *facts, const room_ui_talk_target_t *target, room_ui_state_t state,
                           const char *detail, const char *reader_agent, const room_voice_snapshot_t *voice,
                           bool voice_enabled, bool holding, room_home_action_t action, bool hidden);
