#pragma once

#include <stdbool.h>
#include "esp_openclaw_directory.h"

typedef enum {
    DIAL_REPLY_IDLE, DIAL_REPLY_PREPARING, DIAL_REPLY_REVIEW, DIAL_REPLY_SENDING,
    DIAL_REPLY_ACCEPTED, DIAL_REPLY_SENT, DIAL_REPLY_UNKNOWN, DIAL_REPLY_FAILED,
} dial_reply_state_t;

typedef enum {
    DIAL_REPLY_CONTINUE, DIAL_REPLY_SUMMARIZE, DIAL_REPLY_EXPLAIN, DIAL_REPLY_PRESET_COUNT,
} dial_reply_preset_t;

typedef struct {
    dial_reply_state_t state;
    unsigned revision, preview_revision;
    esp_openclaw_agent_t agent;
    char title[97], text[160], detail[128];
    bool can_send, can_read;
} dial_reply_snapshot_t;
