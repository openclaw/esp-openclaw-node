#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_openclaw_node.h"

esp_err_t room_canvas_init(void);
void room_canvas_set_node(esp_openclaw_node_handle_t node);
void room_canvas_set_gateway_http_base(const char *base_url);
bool room_canvas_is_active(void);

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
