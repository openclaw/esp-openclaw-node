#pragma once

#include "room_agent_directory.h"
#include "lvgl.h"

enum { ROOM_CHAT_NEW_BUTTON = -6, ROOM_CHAT_CREATION_BUTTON, ROOM_CHAT_BACK,
    ROOM_CHAT_PREVIOUS, ROOM_CHAT_REFRESH, ROOM_CHAT_NEXT };
typedef enum { ROOM_CHAT_STAY, ROOM_CHAT_LEAVE, ROOM_CHAT_LOAD, ROOM_CHAT_READ,
    ROOM_CHAT_NEW, ROOM_CHAT_CREATE, ROOM_CHAT_DISMISS_CREATION, ROOM_CHAT_OPEN_CREATED } room_chat_action_t;

void room_watch_conversations_create(lv_obj_t *parent, lv_event_cb_t handler);
void room_watch_conversations_reset(bool show_preview);
void room_watch_conversations_suspend(void);
void room_watch_conversations_resume(const room_conversations_snapshot_t *snapshot);
void room_watch_conversations_render(const room_conversations_snapshot_t *snapshot, bool hidden);
room_chat_action_t room_watch_conversations_activate(int action, size_t *value);

/* Input must use the mode that is actually visible. */
bool room_watch_conversations_input_current(void);
