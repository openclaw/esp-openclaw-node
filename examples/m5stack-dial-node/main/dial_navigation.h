#pragma once

#include "dial_action.h"
#include "dial_directory.h"
#include "esp_openclaw_device_types.h"
#include "lvgl.h"

typedef enum { DIAL_NAV_MENU, DIAL_NAV_AGENTS, DIAL_NAV_DETAIL, DIAL_NAV_SESSIONS, DIAL_NAV_PREVIEW, DIAL_NAV_SETTINGS, DIAL_NAV_DISPLAY, DIAL_NAV_CONNECTION, DIAL_NAV_ACTIVITY, DIAL_NAV_DEVICES, DIAL_NAV_DEVICE_DETAIL, DIAL_NAV_LOCAL_DEVICE } dial_navigation_view_t;
typedef enum { DIAL_NAV_HOME, DIAL_NAV_CONTROL, DIAL_NAV_REFRESH, DIAL_NAV_CHANGED, DIAL_NAV_CONVERSATIONS, DIAL_NAV_SESSIONS_REFRESH, DIAL_NAV_READ, DIAL_NAV_RESUME, DIAL_NAV_BRIGHTNESS, DIAL_NAV_DEVICES_REFRESH } dial_navigation_event_t;
/* CHANGED asks the input owner to cancel active gestures before replacing rows. */
typedef void (*dial_navigation_handler_t)(dial_navigation_event_t event, size_t offset, void *context);

typedef struct {
    lv_obj_t *root, *heading, *back, *rows[3], *icons[3], *names[3], *hint, *description;
    lv_obj_t *identity, *preview_body, *preview_text;
    lv_obj_t *avatars[3], *detail_avatar, *detail_initials;
    dial_navigation_view_t view;
    unsigned focus, first, count;
    unsigned revision, conversation_revision, preview_revision;
    unsigned agent_focus, session_focus;
    unsigned device_focus, device_revision;
    esp_openclaw_devices_state_t device_state;
    dial_navigation_view_t settings_parent;
    dial_agents_state_t conversation_state, preview_state;
    dial_agents_state_t state;
    lv_point_t press_point;
    bool moved;
    int brightness;
    bool brightness_failed;
    bool connection_known, wifi_connected, gateway_connected;
    dial_activity_t activity;
    /* Owned by the application and refreshed only on the LVGL task. */
    const dial_agents_snapshot_t *snapshot;
    const dial_conversations_snapshot_t *conversations;
    const esp_openclaw_devices_snapshot_t *devices;
    dial_navigation_handler_t handler;
    void *context;
} dial_navigation_t;

void dial_navigation_create(dial_navigation_t *nav, lv_obj_t *parent,
                            dial_navigation_handler_t handler, void *context);
void dial_navigation_open(dial_navigation_t *nav, bool agents);
void dial_navigation_update(dial_navigation_t *nav, const dial_agents_snapshot_t *snapshot);
void dial_navigation_conversations_update(dial_navigation_t *nav, const dial_conversations_snapshot_t *snapshot);
bool dial_navigation_has_devices(const dial_navigation_t *nav);
void dial_navigation_open_devices(dial_navigation_t *nav);
void dial_navigation_devices_update(dial_navigation_t *nav, const esp_openclaw_devices_snapshot_t *snapshot);
bool dial_navigation_show_reader(dial_navigation_t *nav, const dial_agents_snapshot_t *agents,
                                 const dial_conversations_snapshot_t *view, bool preview);
void dial_navigation_move(dial_navigation_t *nav, int direction);
void dial_navigation_activate(dial_navigation_t *nav);

/* Opens SETTINGS, DISPLAY or CONNECTION without changing the configured control. */
void dial_navigation_open_settings(dial_navigation_t *nav, dial_navigation_view_t page);
void dial_navigation_set_brightness(dial_navigation_t *nav, int percent, bool failed);
void dial_navigation_set_connection(dial_navigation_t *nav, bool wifi, bool gateway);
void dial_navigation_open_activity(dial_navigation_t *nav);
void dial_navigation_activity_update(dial_navigation_t *nav, const dial_activity_t *activity);
