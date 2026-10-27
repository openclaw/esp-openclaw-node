#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "room_agent_directory.h"

typedef enum {
    ROOM_UI_IDLE = 0,
    ROOM_UI_LISTENING,
    ROOM_UI_CONNECTING,
    ROOM_UI_SPEAKING,
    ROOM_UI_ERROR,
    ROOM_UI_SETUP,
} room_ui_state_t;

typedef enum {
    ROOM_UI_WIFI_STARTING = 0,
    ROOM_UI_WIFI_UNCONFIGURED,
    ROOM_UI_WIFI_CONNECTING,
    ROOM_UI_WIFI_CONNECTED,
    ROOM_UI_WIFI_OFFLINE,
    ROOM_UI_WIFI_UNAVAILABLE,
} room_ui_wifi_state_t;

typedef enum {
    ROOM_UI_GATEWAY_STARTING = 0,
    ROOM_UI_GATEWAY_CONNECTING,
    ROOM_UI_GATEWAY_CONNECTED,
    ROOM_UI_GATEWAY_OFFLINE,
    ROOM_UI_GATEWAY_NO_SESSION,
} room_ui_gateway_state_t;

typedef enum {
    ROOM_UI_TALK_STARTING = 0,
    ROOM_UI_TALK_UNAVAILABLE,
    ROOM_UI_TALK_WAITING,
    ROOM_UI_TALK_NO_SESSION,
    ROOM_UI_TALK_READY,
    ROOM_UI_TALK_CONNECTING,
    ROOM_UI_TALK_ACTIVE,
    ROOM_UI_TALK_STOPPING,
} room_ui_talk_state_t;

typedef struct {
    room_ui_wifi_state_t wifi;
    room_ui_gateway_state_t gateway;
    room_ui_talk_state_t talk;
} room_ui_facts_t;

typedef struct {
    bool selected, expired;
    char id[65], name[65];
} room_ui_talk_target_t;

typedef struct {
    room_ui_state_t state;
    bool diagnostics_open;
    char detail[48];
    char gateway[64];
} room_ui_diagnostics_snapshot_t;

void room_ui_init(void);
/** Register a short, non-blocking local Talk request handler before init. */
void room_ui_set_talk_action_handler(void (*handler)(bool start));
/** Records directory intent without waiting for the Gateway or room owner lock. */
void room_ui_set_agents_action_handler(void (*handler)(bool open, size_t offset));
/** Copy a directory result outside the room owner lock; repaint retries on contention. */
void room_ui_store_agents(const room_agents_snapshot_t *snapshot);
/** Selection intent is copied by the owner; this callback must not wait for it.
 * An empty ID explicitly restores Gateway-default routing for the next call. */
void room_ui_set_agent_selection_handler(void (*handler)(const char *id, unsigned revision));
void room_ui_store_talk_target(const room_ui_talk_target_t *target);
/** Acknowledge only after the owner has accepted or rejected the selection. */
void room_ui_agent_selection_complete(esp_err_t result);
typedef enum { ROOM_CONVERSATIONS_OPEN, ROOM_CONVERSATIONS_LOAD,
    ROOM_CONVERSATIONS_READ, ROOM_CONVERSATIONS_CLOSE,
    ROOM_CONVERSATIONS_SUSPEND, ROOM_CONVERSATIONS_RESUME } room_ui_conversation_action_t;
void room_ui_set_conversation_handler(void (*handler)(room_ui_conversation_action_t,
    const char *agent_id, unsigned revision, size_t value, unsigned generation));
bool room_ui_wants_conversations(void);
void room_ui_store_conversation_context(const room_conversation_context_t *context);
void room_ui_store_conversations(const room_conversations_snapshot_t *snapshot, unsigned generation);
void room_ui_conversations_failed(unsigned generation);
/* Show only an existing reader selection that belongs to the displayed roster. */
esp_err_t room_ui_watch_reader(const room_conversations_snapshot_t *snapshot, bool preview, unsigned generation);
bool room_ui_wants_conversation_resume(unsigned generation);
/* NULL snapshots mean there is no retained reader; show Agents instead. */
esp_err_t room_ui_watch_resume(const room_agents_snapshot_t *agents,
    const room_conversations_snapshot_t *reader, unsigned epoch, unsigned generation);
void room_ui_set(room_ui_state_t state, const char *detail);
/** Store one coherent snapshot without taking the display lock. Refresh after releasing owner locks. */
void room_ui_store_facts(const room_ui_facts_t *facts);
/** Repaint the most recently set state, e.g. after leaving canvas mode. */
void room_ui_refresh(void);
/** Open a local watch page (menu, agents, conversations, settings, display, connection, canvas, home) without changing Talk. */
esp_err_t room_ui_watch_menu(const char *page);
/** Runtime Watch preference; sleep and camera visibility remain controller-owned. */
esp_err_t room_ui_watch_brightness(int percent);
/** Apply the requested UI state through the Watch preference and camera floor. */
esp_err_t room_ui_apply_display_brightness(int percent);
/** Show the idle face for `show_ms` (tap wake-up or agent face.set outside a call). */
void room_ui_show_face_hint(uint32_t show_ms);
/** Record the connected gateway (host:port); shown under the face. NULL clears. */
void room_ui_set_gateway(const char *gateway_host);
/** True while the current Talk state renders as the face. */
bool room_ui_talk_face_active(void);
esp_err_t room_ui_camera_indicator_begin(void);
void room_ui_camera_indicator_end(void);
void room_ui_get_diagnostics(room_ui_diagnostics_snapshot_t *snapshot);
const char *room_ui_state_name(room_ui_state_t state);
/** Controller-owned brightness and privacy-indicator policy for the modal. */
void room_ui_set_diagnostics_open(bool open);
void room_ui_raise_camera_indicator(void);
bool room_ui_is_initialized(void);
