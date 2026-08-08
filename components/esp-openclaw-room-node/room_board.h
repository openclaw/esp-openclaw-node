#pragma once

#include "esp_openclaw_room_node.h"

esp_err_t room_board_bind(const esp_openclaw_room_node_config_t *config);
const esp_openclaw_room_node_config_t *room_board_config(void);
lv_display_t *room_board_display_start(void);
bool room_board_display_lock(uint32_t timeout_ms);
void room_board_display_unlock(void);
esp_err_t room_board_display_brightness_set(int percent);
