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

/* Read states match the agent directory; preview status additionally distinguishes
 * an empty transcript from a missing or failed conversation. */
typedef struct {
    dial_agents_state_t state, preview_state;
    unsigned revision, preview_revision;
    esp_openclaw_agent_t agent;
    esp_openclaw_session_page_t page;
    esp_openclaw_session_preview_t preview;
    size_t selected;
} dial_conversations_snapshot_t;

/* Small Home projection; selection remains owned by the reader. */
typedef struct {
    bool available, preview;
    unsigned directory_revision, conversation_revision, preview_revision;
    esp_openclaw_agent_t agent;
} dial_home_context_t;
