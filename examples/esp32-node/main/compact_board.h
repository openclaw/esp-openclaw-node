// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "sdkconfig.h"

#if CONFIG_EXAMPLE_BOARD_M5STICKC || CONFIG_EXAMPLE_BOARD_T_EMBED
esp_err_t compact_board_start(void);
void compact_board_set_connected(bool connected);
#else
static inline esp_err_t compact_board_start(void) { return ESP_OK; }
static inline void compact_board_set_connected(bool connected) { (void)connected; }
#endif
