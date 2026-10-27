#pragma once

#include "cJSON.h"
#include "esp_openclaw_node.h"

/* The node remains usable when optional operator access is unavailable. */
esp_err_t dial_reader_start(esp_openclaw_node_handle_t node);
void dial_reader_node_event(esp_openclaw_node_event_t event);
cJSON *dial_reader_status(void);
