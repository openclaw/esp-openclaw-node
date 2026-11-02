#pragma once

#include "lvgl.h"
#include "room_voice_draft.h"

/* The editor owns only unsent text. The shared draft owner submits and tracks it. */
void room_text_composer_create(lv_obj_t *parent, bool (*current)(void),
    void (*action)(bool send, const char *text));
void room_text_composer_open(uint32_t generation, const char *text);
void room_text_composer_render(const room_voice_snapshot_t *draft, bool hidden, int bottom);
