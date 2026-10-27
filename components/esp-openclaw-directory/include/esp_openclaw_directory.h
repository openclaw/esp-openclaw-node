#pragma once

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
