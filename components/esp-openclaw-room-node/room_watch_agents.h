#pragma once

#include "room_agent_directory.h"
#include "lvgl.h"

enum {
    ROOM_AGENT_CHOOSE = -5, ROOM_AGENT_BACK, ROOM_AGENT_PREVIOUS, ROOM_AGENT_NEXT, ROOM_AGENT_REFRESH,
};

typedef enum { ROOM_AGENT_STAY, ROOM_AGENT_LEAVE, ROOM_AGENT_LOAD, ROOM_AGENT_SELECT_TARGET } room_agent_action_t;

void room_watch_agents_create(lv_obj_t *parent, lv_event_cb_t handler);
void room_watch_agents_reset(void);
/* The snapshot is owned by the display controller and lives until the next render. */
void room_watch_agents_render(const room_agents_snapshot_t *snapshot, bool hidden,
    const char *target_id, bool expired, bool pending, bool failed);
room_agent_action_t room_watch_agents_activate(int action, size_t *offset);

bool room_watch_agents_target(char id[65], unsigned *revision);
