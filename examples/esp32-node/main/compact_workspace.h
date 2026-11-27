// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "compact_input.h"
#include "esp_err.h"
#include "esp_openclaw_node_wifi.h"

typedef esp_err_t (*compact_brightness_fn)(int percent);

void compact_workspace_start(int width, int height, bool encoder,
                             compact_brightness_fn brightness);
void compact_workspace_poll(const esp_openclaw_node_wifi_status_t *wifi, bool connected,
                            uint32_t now);
void compact_workspace_input(compact_input_event_t event);
void compact_workspace_message(const char *heading, const char *text, bool large, uint32_t generation);
unsigned compact_workspace_revision(void);
bool compact_workspace_message_visible(void);
const char *compact_workspace_view(void);
/* UI-owner access after poll/render. Never publish this label through node commands. */
const char *compact_workspace_selection(void);
int compact_workspace_message_line_height(void);
