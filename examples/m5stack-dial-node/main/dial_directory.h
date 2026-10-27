#pragma once

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
