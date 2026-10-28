#pragma once

#include "lvgl.h"
#include "room_ui_controller.h"

typedef enum {
    ROOM_WATCH_HOME, ROOM_WATCH_MENU, ROOM_WATCH_CONNECTION,
    ROOM_WATCH_CANVAS, ROOM_WATCH_DEVICE_STATUS, ROOM_WATCH_AGENTS,
    ROOM_WATCH_SETTINGS, ROOM_WATCH_DISPLAY, ROOM_WATCH_BRIGHTNESS, ROOM_WATCH_READING, ROOM_WATCH_DENSITY,
    ROOM_WATCH_TEXT_STANDARD, ROOM_WATCH_TEXT_LARGE, ROOM_WATCH_COMPACT, ROOM_WATCH_COMFORTABLE, ROOM_WATCH_MESSAGING, ROOM_WATCH_DIMMER, ROOM_WATCH_BRIGHTER,
    ROOM_WATCH_CONVERSATIONS, ROOM_WATCH_DEVICES, ROOM_WATCH_LOCAL_DEVICE, ROOM_WATCH_BACK,
} room_watch_page_t;

void room_watch_navigation_set_awake(bool awake);
void room_watch_navigation_create(lv_obj_t *parent, lv_event_cb_t handler);
void room_watch_navigation_render(room_watch_page_t page, const room_ui_facts_t *facts,
                                  bool hidden, bool resume_failed, room_watch_page_t parent, int content_bottom);
