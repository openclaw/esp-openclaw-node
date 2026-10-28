#pragma once

#include "cJSON.h"
#include "esp_openclaw_node.h"
#include "esp_openclaw_devices.h"
#include "dial_directory.h"
#include "dial_messaging.h"
#include "dial_reply.h"
#include "dial_creation.h"

/* The node remains usable when optional operator access is unavailable. */
esp_err_t dial_reader_start(esp_openclaw_node_handle_t node);
void dial_reader_node_event(esp_openclaw_node_event_t event);
cJSON *dial_reader_status(void);
void dial_reader_messaging_copy(dial_messaging_snapshot_t *snapshot);
esp_err_t dial_reader_messaging_request(unsigned revision);
esp_err_t dial_reader_messaging_cancel(unsigned revision);
/* Read-only inventory shares the reader connection, never the assigned control
 * session. Copy clears stale devices immediately when either connection is lost. */
esp_err_t dial_reader_devices_refresh(void);
void dial_reader_devices_close(void);
void dial_reader_devices_copy(esp_openclaw_devices_snapshot_t *snapshot);
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

/* A draft is bound to the visible preview; Send is an explicit revision-fenced intent. */
esp_err_t dial_reader_reply_begin(dial_reply_preset_t preset, unsigned preview_revision);
esp_err_t dial_reader_reply_send(unsigned revision);
void dial_reader_reply_close(unsigned revision);
void dial_reader_reply_copy(dial_reply_snapshot_t *snapshot);
esp_err_t dial_reader_reply_read(unsigned revision);

/* Creating an empty conversation and sending a message are separate intents. */
esp_err_t dial_reader_creation_begin(unsigned conversation_revision);
esp_err_t dial_reader_creation_submit(unsigned revision);
esp_err_t dial_reader_creation_open(unsigned revision);
void dial_reader_creation_close(unsigned revision);
void dial_reader_creation_copy(dial_creation_snapshot_t *snapshot);
