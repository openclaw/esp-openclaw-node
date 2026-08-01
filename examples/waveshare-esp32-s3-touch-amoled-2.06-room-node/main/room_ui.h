#pragma once

typedef enum {
    ROOM_UI_IDLE = 0,
    ROOM_UI_LISTENING,
    ROOM_UI_CONNECTING,
    ROOM_UI_SPEAKING,
    ROOM_UI_ERROR,
    ROOM_UI_SETUP,
} room_ui_state_t;

void room_ui_init(void);
void room_ui_set(room_ui_state_t state, const char *detail);
/** Repaint the most recently set state, e.g. after leaving canvas mode. */
void room_ui_refresh(void);
