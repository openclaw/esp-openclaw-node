#pragma once

#include "room_watch_navigation.h"

void room_watch_settings_create(lv_obj_t *parent, lv_event_cb_t handler);
void room_watch_settings_render(room_watch_page_t page, room_watch_page_t parent, int brightness, bool failed, bool camera_minimum, bool hidden);
