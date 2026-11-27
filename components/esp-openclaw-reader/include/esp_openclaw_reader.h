#pragma once

#include "cJSON.h"
#include "esp_openclaw_node.h"
#include "esp_openclaw_devices.h"
#include "esp_openclaw_reader_types.h"

typedef struct {
    const char *display_name;
    const char *model_identifier;
    const char *tls_server_name;
} esp_openclaw_reader_config_t;

/* One reader per application. Configuration strings are copied during start.
 * The node remains usable when optional operator access is unavailable. */
esp_err_t esp_openclaw_reader_start(esp_openclaw_node_handle_t node,
                                   const esp_openclaw_reader_config_t *config);
void esp_openclaw_reader_node_event(esp_openclaw_node_event_t event);
cJSON *esp_openclaw_reader_status(void);
/* Queue one local bootstrap attempt for the current primary Gateway. Requires
 * an idle, unavailable reader with no work in flight. Never replays the code;
 * after manual pairing approval, explicitly submit again if the first attempt
 * was rejected. The existing node session is not replaced. */
esp_err_t esp_openclaw_reader_setup_code(const char *code);
void esp_openclaw_reader_messaging_copy(esp_openclaw_reader_messaging_snapshot_t *snapshot);
esp_err_t esp_openclaw_reader_messaging_request(unsigned revision);
esp_err_t esp_openclaw_reader_messaging_cancel(unsigned revision);
/* Read-only inventory shares the reader connection, never the assigned control
 * session. Copy clears stale devices immediately when either connection is lost. */
esp_err_t esp_openclaw_reader_devices_refresh(void);
void esp_openclaw_reader_devices_close(void);
void esp_openclaw_reader_devices_copy(esp_openclaw_devices_snapshot_t *snapshot);
esp_err_t esp_openclaw_reader_agents_refresh(size_t offset);
void esp_openclaw_reader_agents_copy(esp_openclaw_reader_agents_t *snapshot);

/* IDs and row indices are admitted against the current copied directory revision.
 * Browsing never changes an independently configured control session. */
esp_err_t esp_openclaw_reader_conversations_open(const char *agent_id, unsigned revision);
esp_err_t esp_openclaw_reader_conversations_refresh(size_t offset);
esp_err_t esp_openclaw_reader_preview_open(size_t index, unsigned revision);
void esp_openclaw_reader_conversations_close(void);
void esp_openclaw_reader_conversations_copy(esp_openclaw_reader_conversations_t *snapshot);

/* Never allocates or copies transcript pages. An unavailable connection clears
 * the Home context immediately, before the reader worker repaints its state. */
void esp_openclaw_reader_home_copy(esp_openclaw_reader_home_t *context);

/* A draft is bound to the visible preview; Send is an explicit revision-fenced intent. */
esp_err_t esp_openclaw_reader_reply_begin(esp_openclaw_reader_reply_preset_t preset, unsigned preview_revision);
esp_err_t esp_openclaw_reader_reply_send(unsigned revision);
void esp_openclaw_reader_reply_close(unsigned revision);
void esp_openclaw_reader_reply_copy(esp_openclaw_reader_reply_snapshot_t *snapshot);
esp_err_t esp_openclaw_reader_reply_read(unsigned revision);

/* Creating an empty conversation and sending a message are separate intents. */
esp_err_t esp_openclaw_reader_creation_begin(unsigned conversation_revision);
esp_err_t esp_openclaw_reader_creation_submit(unsigned revision);
esp_err_t esp_openclaw_reader_creation_open(unsigned revision);
void esp_openclaw_reader_creation_close(unsigned revision);
void esp_openclaw_reader_creation_copy(esp_openclaw_reader_creation_snapshot_t *snapshot);
