#pragma once

#include "room_watch_navigation.h"

/* Native 1280 x 720 landscape renderer. The controller owns input and state. */
void room_tab5_ui_create(lv_obj_t *parent, lv_event_cb_t navigation);
bool room_tab5_ui_supports(room_watch_page_t page);
bool room_tab5_settings_page(room_watch_page_t page);
void room_tab5_ui_render(room_watch_page_t page, room_watch_page_t parent, const room_ui_facts_t *facts,
    const room_ui_talk_target_t *target, room_ui_state_t state, const char *detail, bool hidden);
