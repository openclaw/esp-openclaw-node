/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "esp_openclaw_node.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_OPENCLAW_CHAT_TEXT_BYTES 2048U
#define ESP_OPENCLAW_CHAT_KEY_BYTES 513U
#define ESP_OPENCLAW_CHAT_ID_BYTES 129U

typedef struct esp_openclaw_chat *esp_openclaw_chat_handle_t;

typedef enum {
    ESP_OPENCLAW_CHAT_PREPARING,
    ESP_OPENCLAW_CHAT_READY,
    ESP_OPENCLAW_CHAT_SENDING,
    ESP_OPENCLAW_CHAT_RUNNING,
    ESP_OPENCLAW_CHAT_COMPLETE,
    ESP_OPENCLAW_CHAT_HANDED_OFF,
    ESP_OPENCLAW_CHAT_ABORTED,
    ESP_OPENCLAW_CHAT_FAILED,
    ESP_OPENCLAW_CHAT_OUTCOME_UNKNOWN,
} esp_openclaw_chat_state_t;

typedef enum {
    ESP_OPENCLAW_CHAT_ERROR_NONE,
    ESP_OPENCLAW_CHAT_ERROR_CONNECTION,
    ESP_OPENCLAW_CHAT_ERROR_TIMEOUT,
    ESP_OPENCLAW_CHAT_ERROR_GATEWAY,
    ESP_OPENCLAW_CHAT_ERROR_FORMAT,
} esp_openclaw_chat_error_t;

typedef struct {
    esp_openclaw_chat_state_t state;
    esp_openclaw_chat_error_t error;
    char session_key[ESP_OPENCLAW_CHAT_KEY_BYTES];
    char agent_id[65];
    char session_id[ESP_OPENCLAW_CHAT_ID_BYTES];
    char run_id[ESP_OPENCLAW_CHAT_ID_BYTES];
} esp_openclaw_chat_snapshot_t;

/* Resolve an existing selected conversation using chat.history. Does not create
 * a conversation or send a message. Node must outlive this handle AND all its
 * accepted RPC callbacks. The caller serializes publication, event forwarding,
 * copy/poll/send and destruction; transport callbacks synchronize internally. */
esp_err_t esp_openclaw_chat_prepare(esp_openclaw_node_handle_t node,
    const char *session_key, const char *agent_id, esp_openclaw_chat_handle_t *out);

/* Explicit, single-use submission of a complete UTF-8 draft. Supply a fresh
 * unique idempotency key for this user intent. Queue rejection returns an error
 * and leaves READY; ESP_OK means queued locally, not accepted by the Gateway.
 * Never automatically resend an OUTCOME_UNKNOWN draft with a new key. */
esp_err_t esp_openclaw_chat_send(esp_openclaw_chat_handle_t handle,
    const char *text, const char *idempotency_key);

/* Forward the shared Node event callback, without replacing other consumers.
 * Completion describes this run only. Read replies through chat.history;
 * queued followups and handed-off work may continue under another run. */
void esp_openclaw_chat_event(esp_openclaw_chat_handle_t handle,
    esp_openclaw_node_handle_t node, const char *event, const char *payload_json);
void esp_openclaw_chat_poll(esp_openclaw_chat_handle_t handle);
void esp_openclaw_chat_copy(esp_openclaw_chat_handle_t handle,
    esp_openclaw_chat_snapshot_t *out);

/* Release the view; never aborts an already submitted agent turn. Accepted
 * callbacks retain their own reference, so late replies remain safe. */
void esp_openclaw_chat_destroy(esp_openclaw_chat_handle_t handle);

#ifdef __cplusplus
}
#endif
