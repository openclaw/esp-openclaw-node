#pragma once

#include <stdbool.h>
#include "esp_openclaw_directory.h"

#define ESP_OPENCLAW_READER_TEXT_BYTES 160U

typedef enum {
    ESP_OPENCLAW_READER_IDLE, ESP_OPENCLAW_READER_LOADING, ESP_OPENCLAW_READER_READY, ESP_OPENCLAW_READER_EMPTY,
    ESP_OPENCLAW_READER_OFFLINE, ESP_OPENCLAW_READER_CONNECTING, ESP_OPENCLAW_READER_ACCESS_NEEDED,
    ESP_OPENCLAW_READER_ERROR, ESP_OPENCLAW_READER_TOO_LARGE,
} esp_openclaw_reader_state_t;

typedef struct {
    esp_openclaw_reader_state_t state;
    unsigned revision;
    esp_openclaw_agent_page_t page;
} esp_openclaw_reader_agents_t;

/* Read states match the agent directory; preview status additionally distinguishes
 * an empty transcript from a missing or failed conversation. */
typedef struct {
    esp_openclaw_reader_state_t state, preview_state;
    unsigned revision, preview_revision;
    esp_openclaw_agent_t agent;
    esp_openclaw_session_page_t page;
    esp_openclaw_session_preview_t preview;
    size_t selected;
} esp_openclaw_reader_conversations_t;

/* Small Home projection; selection remains owned by the reader. */
typedef struct {
    bool available, preview;
    unsigned directory_revision, conversation_revision, preview_revision;
    esp_openclaw_agent_t agent;
} esp_openclaw_reader_home_t;

typedef enum {
    ESP_OPENCLAW_READER_MESSAGING_OFF,
    ESP_OPENCLAW_READER_MESSAGING_REQUESTING,
    ESP_OPENCLAW_READER_MESSAGING_WAITING,
    ESP_OPENCLAW_READER_MESSAGING_CONNECTING,
    ESP_OPENCLAW_READER_MESSAGING_READY,
    ESP_OPENCLAW_READER_MESSAGING_REJECTED,
    ESP_OPENCLAW_READER_MESSAGING_EXPIRED,
    ESP_OPENCLAW_READER_MESSAGING_ERROR,
} esp_openclaw_reader_messaging_state_t;

typedef struct {
    esp_openclaw_reader_messaging_state_t state;
    unsigned revision;
    char request_id[129];
} esp_openclaw_reader_messaging_snapshot_t;

typedef enum {
    ESP_OPENCLAW_READER_REPLY_IDLE, ESP_OPENCLAW_READER_REPLY_PREPARING, ESP_OPENCLAW_READER_REPLY_REVIEW, ESP_OPENCLAW_READER_REPLY_SENDING,
    ESP_OPENCLAW_READER_REPLY_ACCEPTED, ESP_OPENCLAW_READER_REPLY_SENT, ESP_OPENCLAW_READER_REPLY_UNKNOWN, ESP_OPENCLAW_READER_REPLY_FAILED,
} esp_openclaw_reader_reply_state_t;

typedef enum {
    ESP_OPENCLAW_READER_REPLY_CONTINUE, ESP_OPENCLAW_READER_REPLY_SUMMARIZE, ESP_OPENCLAW_READER_REPLY_EXPLAIN, ESP_OPENCLAW_READER_REPLY_PRESET_COUNT,
} esp_openclaw_reader_reply_preset_t;

typedef struct {
    esp_openclaw_reader_reply_state_t state;
    unsigned revision, preview_revision;
    esp_openclaw_agent_t agent;
    char title[97], text[ESP_OPENCLAW_READER_TEXT_BYTES], detail[128];
    bool can_send, can_read;
} esp_openclaw_reader_reply_snapshot_t;

typedef enum {
    ESP_OPENCLAW_READER_CREATION_IDLE, ESP_OPENCLAW_READER_CREATION_REVIEW, ESP_OPENCLAW_READER_CREATION_CREATING,
    ESP_OPENCLAW_READER_CREATION_CREATED, ESP_OPENCLAW_READER_CREATION_UNKNOWN, ESP_OPENCLAW_READER_CREATION_FAILED,
} esp_openclaw_reader_creation_state_t;

typedef struct {
    esp_openclaw_reader_creation_state_t state;
    unsigned revision, conversation_revision;
    esp_openclaw_agent_t agent;
    char detail[128];
    bool can_create, can_open;
} esp_openclaw_reader_creation_snapshot_t;
