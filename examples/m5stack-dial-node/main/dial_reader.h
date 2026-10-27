#pragma once

#include "cJSON.h"
#include "esp_openclaw_node.h"
#include "esp_openclaw_directory.h"

typedef enum {
    DIAL_AGENTS_IDLE, DIAL_AGENTS_LOADING, DIAL_AGENTS_READY, DIAL_AGENTS_EMPTY,
    DIAL_AGENTS_OFFLINE, DIAL_AGENTS_CONNECTING, DIAL_AGENTS_ACCESS_NEEDED,
    DIAL_AGENTS_ERROR, DIAL_AGENTS_TOO_LARGE,
} dial_agents_state_t;

typedef struct {
    dial_agents_state_t state;
    unsigned revision;
    esp_openclaw_agent_page_t page;
} dial_agents_snapshot_t;

/* The node remains usable when optional operator access is unavailable. */
esp_err_t dial_reader_start(esp_openclaw_node_handle_t node);
void dial_reader_node_event(esp_openclaw_node_event_t event);
cJSON *dial_reader_status(void);
esp_err_t dial_reader_agents_refresh(size_t offset);
void dial_reader_agents_copy(dial_agents_snapshot_t *snapshot);
