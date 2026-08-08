#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_openclaw_node.h"

#define ROOM_CANVAS_ACTIVE_BRIGHTNESS 80

typedef enum {
    ROOM_CANVAS_ACTION_RENDER_CHANGED,
    ROOM_CANVAS_ACTION_REQUEST_FACE_HINT,
} room_canvas_action_t;

typedef void (*room_canvas_action_handler_t)(room_canvas_action_t action, uint32_t value);

typedef enum {
    ROOM_CANVAS_RETAINED_NONE = 0,
    ROOM_CANVAS_RETAINED_IMAGE,
    ROOM_CANVAS_RETAINED_A2UI,
} room_canvas_retained_kind_t;

typedef struct {
    bool active;
    room_canvas_retained_kind_t retained_kind;
    uint16_t retained_components;
    uint8_t retained_images;
} room_canvas_diagnostics_snapshot_t;

void room_canvas_set_action_handler(room_canvas_action_handler_t handler);

esp_err_t room_canvas_init(void);
void room_canvas_set_node(esp_openclaw_node_handle_t node);
/** Operator-role client used for plugin.surface.refresh (operator scope). */
void room_canvas_set_refresh_client(esp_openclaw_node_handle_t operator_node);
void room_canvas_set_gateway_http_base(const char *base_url);
bool room_canvas_is_active(void);
void room_canvas_get_diagnostics(room_canvas_diagnostics_snapshot_t *snapshot);
/** Cycle status <-> canvas for tap/BOOT; wakes the face when no content. */
void room_canvas_view_toggle(void);

esp_err_t room_canvas_present(
    const char *url,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error);
esp_err_t room_canvas_hide(
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error);
esp_err_t room_canvas_snapshot(
    int max_width,
    double quality,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error);
esp_err_t room_canvas_a2ui_push_jsonl(
    const char *jsonl,
    size_t jsonl_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error);
esp_err_t room_canvas_a2ui_push_messages(
    const cJSON *messages,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error);
esp_err_t room_canvas_a2ui_reset(
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error);
