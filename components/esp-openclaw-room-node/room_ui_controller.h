#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    ROOM_UI_IDLE = 0,
    ROOM_UI_LISTENING,
    ROOM_UI_CONNECTING,
    ROOM_UI_SPEAKING,
    ROOM_UI_ERROR,
    ROOM_UI_SETUP,
} room_ui_state_t;

typedef struct {
    room_ui_state_t state;
    bool diagnostics_open;
    char detail[48];
    char gateway[64];
} room_ui_diagnostics_snapshot_t;

void room_ui_init(void);
void room_ui_set(room_ui_state_t state, const char *detail);
/** Repaint the most recently set state, e.g. after leaving canvas mode. */
void room_ui_refresh(void);
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
