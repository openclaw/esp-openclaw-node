#pragma once

#include "cJSON.h"
#include "esp_openclaw_node.h"
#include "dial_directory.h"

/* The node remains usable when optional operator access is unavailable. */
esp_err_t dial_reader_start(esp_openclaw_node_handle_t node);
void dial_reader_node_event(esp_openclaw_node_event_t event);
cJSON *dial_reader_status(void);
esp_err_t dial_reader_agents_refresh(size_t offset);
void dial_reader_agents_copy(dial_agents_snapshot_t *snapshot);
