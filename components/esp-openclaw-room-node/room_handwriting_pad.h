#pragma once

#include "lvgl.h"

void room_handwriting_pad_create(lv_obj_t *parent, bool (*current)(void),
    bool (*accept)(char), void (*keyboard)(void));
void room_handwriting_pad_show(bool visible, int bottom);
