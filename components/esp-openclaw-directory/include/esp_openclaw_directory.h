#pragma once

#include <stdbool.h>
#include <stddef.h>

#define ESP_OPENCLAW_DIRECTORY_PAGE_SIZE 8
#define ESP_OPENCLAW_DIRECTORY_MAX_JSON_BYTES 32768

typedef struct {
    char id[65];
    char name[65];
    char emoji[33];
} esp_openclaw_agent_t;

typedef struct {
    size_t offset, count, total;
    /* Empty if absent or too long; routing metadata must never be shortened. */
    char main_key[129];
    esp_openclaw_agent_t agents[ESP_OPENCLAW_DIRECTORY_PAGE_SIZE];
} esp_openclaw_agent_page_t;

typedef enum {
    ESP_OPENCLAW_DIRECTORY_OK,
    ESP_OPENCLAW_DIRECTORY_INVALID,
    ESP_OPENCLAW_DIRECTORY_TOO_LARGE,
} esp_openclaw_directory_result_t;

/* Parse one local page of agents.list. The Gateway currently returns the whole
 * roster. IDs are copied exactly; display text may be shortened. */
esp_openclaw_directory_result_t esp_openclaw_directory_parse(
    const char *json, size_t length, size_t offset, esp_openclaw_agent_page_t *page);

#define ESP_OPENCLAW_SESSION_KEY_BYTES 513
#define ESP_OPENCLAW_PREVIEW_ITEMS 4

typedef struct {
    /* Routing identity is exact; only presentation text may be shortened. */
    char key[ESP_OPENCLAW_SESSION_KEY_BYTES];
    char title[97];
    char last_message[161];
} esp_openclaw_session_t;

typedef struct {
    size_t offset, count, next_offset;
    bool has_more;
    esp_openclaw_session_t sessions[ESP_OPENCLAW_DIRECTORY_PAGE_SIZE];
} esp_openclaw_session_page_t;

typedef enum {
    ESP_OPENCLAW_PREVIEW_OK, ESP_OPENCLAW_PREVIEW_EMPTY,
    ESP_OPENCLAW_PREVIEW_MISSING, ESP_OPENCLAW_PREVIEW_ERROR,
} esp_openclaw_preview_status_t;

typedef enum {
    ESP_OPENCLAW_PREVIEW_USER, ESP_OPENCLAW_PREVIEW_ASSISTANT,
    ESP_OPENCLAW_PREVIEW_TOOL, ESP_OPENCLAW_PREVIEW_SYSTEM, ESP_OPENCLAW_PREVIEW_OTHER,
} esp_openclaw_preview_role_t;

typedef struct {
    esp_openclaw_preview_role_t role;
    char text[241];
} esp_openclaw_preview_item_t;

typedef struct {
    esp_openclaw_preview_status_t status;
    size_t count;
    esp_openclaw_preview_item_t items[ESP_OPENCLAW_PREVIEW_ITEMS];
} esp_openclaw_session_preview_t;

/* sessions.list must be requested with agentId, limit=8 and includeGlobal=false.
 * Offset belongs to the server page, not a local slice. No key is synthesized. */
esp_openclaw_directory_result_t esp_openclaw_sessions_parse(
    const char *json, size_t length, const char *agent_id, size_t offset,
    esp_openclaw_session_page_t *page);

/* Parse the result of sessions.preview for one exact key with limit=4. */
esp_openclaw_directory_result_t esp_openclaw_preview_parse(
    const char *json, size_t length, const char *key,
    esp_openclaw_session_preview_t *preview);

/* A confirmed empty sessions.create result. Neither routing value is shortened. */
typedef struct {
    char key[ESP_OPENCLAW_SESSION_KEY_BYTES];
    char session_id[129];
} esp_openclaw_session_created_t;

esp_openclaw_directory_result_t esp_openclaw_session_created_parse(
    const char *json, size_t length, const char *agent_id,
    esp_openclaw_session_created_t *created);
