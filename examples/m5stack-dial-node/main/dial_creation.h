#pragma once

#include <stdbool.h>
#include "esp_openclaw_directory.h"

typedef enum {
    DIAL_CREATION_IDLE, DIAL_CREATION_REVIEW, DIAL_CREATION_CREATING,
    DIAL_CREATION_CREATED, DIAL_CREATION_UNKNOWN, DIAL_CREATION_FAILED,
} dial_creation_state_t;

typedef struct {
    dial_creation_state_t state;
    unsigned revision, conversation_revision;
    esp_openclaw_agent_t agent;
    char detail[128];
    bool can_create, can_open;
} dial_creation_snapshot_t;
