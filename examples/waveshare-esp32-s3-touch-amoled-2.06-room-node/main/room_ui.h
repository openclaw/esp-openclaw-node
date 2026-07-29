#pragma once

typedef enum {
    ROOM_UI_IDLE = 0,
    ROOM_UI_LISTENING,
    ROOM_UI_CONNECTING,
    ROOM_UI_SPEAKING,
    ROOM_UI_ERROR,
} room_ui_state_t;

void room_ui_init(void);
void room_ui_set(room_ui_state_t state, const char *detail);
