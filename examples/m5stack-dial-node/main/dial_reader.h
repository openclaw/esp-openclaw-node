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

/* IDs and row indices are admitted against the current copied directory revision.
 * Browsing never changes the session assigned by dial.configure. */
esp_err_t dial_reader_conversations_open(const char *agent_id, unsigned revision);
esp_err_t dial_reader_conversations_refresh(size_t offset);
esp_err_t dial_reader_preview_open(size_t index, unsigned revision);
void dial_reader_conversations_close(void);
void dial_reader_conversations_copy(dial_conversations_snapshot_t *snapshot);

/* Never allocates or copies transcript pages. An unavailable connection clears
 * the Home context immediately, before the reader worker repaints its state. */
void dial_reader_home_copy(dial_home_context_t *context);
