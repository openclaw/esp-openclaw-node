#pragma once

#include "dial_directory.h"
#include "lvgl.h"

typedef enum { DIAL_NAV_MENU, DIAL_NAV_AGENTS, DIAL_NAV_DETAIL, DIAL_NAV_SESSIONS, DIAL_NAV_PREVIEW, DIAL_NAV_SETTINGS, DIAL_NAV_DISPLAY, DIAL_NAV_CONNECTION } dial_navigation_view_t;
typedef enum { DIAL_NAV_HOME, DIAL_NAV_CONTROL, DIAL_NAV_REFRESH, DIAL_NAV_CHANGED, DIAL_NAV_CONVERSATIONS, DIAL_NAV_SESSIONS_REFRESH, DIAL_NAV_READ, DIAL_NAV_CLOSE, DIAL_NAV_BRIGHTNESS } dial_navigation_event_t;
/* CHANGED asks the input owner to cancel active gestures before replacing rows. */
typedef void (*dial_navigation_handler_t)(dial_navigation_event_t event, size_t offset, void *context);

typedef struct {
    lv_obj_t *root, *heading, *back, *rows[3], *icons[3], *names[3], *hint, *description;
    lv_obj_t *id_caption, *identity, *preview_body, *preview_text;
    dial_navigation_view_t view;
    unsigned focus, first, count;
    unsigned revision, conversation_revision, preview_revision;
    unsigned agent_focus, session_focus;
    dial_agents_state_t conversation_state, preview_state;
    dial_agents_state_t state;
    lv_point_t press_point;
    bool moved;
    int brightness;
    bool brightness_failed;
    bool connection_known, wifi_connected, gateway_connected;
    /* Owned by the application and refreshed only on the LVGL task. */
    const dial_agents_snapshot_t *snapshot;
    const dial_conversations_snapshot_t *conversations;
    dial_navigation_handler_t handler;
    void *context;
} dial_navigation_t;

void dial_navigation_create(dial_navigation_t *nav, lv_obj_t *parent,
                            dial_navigation_handler_t handler, void *context);
void dial_navigation_open(dial_navigation_t *nav, bool agents);
void dial_navigation_update(dial_navigation_t *nav, const dial_agents_snapshot_t *snapshot);
void dial_navigation_conversations_update(dial_navigation_t *nav, const dial_conversations_snapshot_t *snapshot);
bool dial_navigation_show_reader(dial_navigation_t *nav, const dial_agents_snapshot_t *agents,
                                 const dial_conversations_snapshot_t *view, bool preview);
void dial_navigation_move(dial_navigation_t *nav, int direction);
void dial_navigation_activate(dial_navigation_t *nav);

/* Opens SETTINGS, DISPLAY or CONNECTION without changing the configured control. */
void dial_navigation_open_settings(dial_navigation_t *nav, dial_navigation_view_t page);
void dial_navigation_set_brightness(dial_navigation_t *nav, int percent, bool failed);
void dial_navigation_set_connection(dial_navigation_t *nav, bool wifi, bool gateway);
