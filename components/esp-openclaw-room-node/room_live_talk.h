#pragma once

#include "esp_openclaw_talk_snapshot.h"
#include "lvgl.h"

void room_live_talk_create(lv_obj_t *parent, bool wide);
void room_live_talk_render(const esp_openclaw_talk_snapshot_t *snapshot, bool hidden);
