#pragma once

#include "lvgl.h"

void room_watch_settings_create(lv_obj_t *parent, lv_event_cb_t handler);
void room_watch_settings_render(bool display, int brightness, bool failed, bool camera_minimum, bool hidden);
