#pragma once

#include "lvgl.h"
#include "esp_openclaw_devices.h"

enum { ROOM_DEVICE_NEXT = -5, ROOM_DEVICE_BACK, ROOM_DEVICE_LOCAL,
       ROOM_DEVICE_REFRESH, ROOM_DEVICE_PREVIOUS };
typedef enum { ROOM_DEVICE_STAY, ROOM_DEVICE_LEAVE, ROOM_DEVICE_OPEN_LOCAL,
               ROOM_DEVICE_RELOAD } room_device_action_t;

void room_watch_devices_create(lv_obj_t *parent, lv_event_cb_t handler);
void room_watch_devices_reset(void);
/* The controller owns the snapshot until the next render. NULL shows a retry. */
void room_watch_devices_render(const esp_openclaw_devices_snapshot_t *snapshot, bool hidden);
room_device_action_t room_watch_devices_activate(int action);
