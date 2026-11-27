// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_openclaw_node.h"
#include "sdkconfig.h"
#include "compact_input.h"

#if CONFIG_EXAMPLE_BOARD_M5STICKC || CONFIG_EXAMPLE_BOARD_T_EMBED
esp_err_t compact_board_start(void);
esp_err_t compact_board_register_commands(esp_openclaw_node_handle_t node);
void compact_board_set_connected(bool connected);
esp_err_t compact_board_console_input(compact_input_event_t event, uint32_t revision, uint32_t *request_id);
esp_err_t compact_board_console_status(char **out_json);
#else
static inline esp_err_t compact_board_start(void) { return ESP_OK; }
static inline esp_err_t compact_board_register_commands(esp_openclaw_node_handle_t node) { (void)node; return ESP_OK; }
static inline void compact_board_set_connected(bool connected) { (void)connected; }
#endif
