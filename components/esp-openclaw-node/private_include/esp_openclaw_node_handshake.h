#pragma once

#include <stddef.h>
#include <stdbool.h>

#include "cJSON.h"

/* Validate the complete handshake frame, retaining only fields consumed before
 * the connection is ready. */
cJSON *esp_openclaw_node_parse_handshake(const char *json, size_t length);

typedef enum {
    ESP_OPENCLAW_RESPONSE_NOT_APPLICABLE,
    ESP_OPENCLAW_RESPONSE_PARSED,
    ESP_OPENCLAW_RESPONSE_INVALID,
    ESP_OPENCLAW_RESPONSE_NO_MEMORY,
} esp_openclaw_response_parse_t;

/* The caller owns *root, including a Raw payload. On payload allocation failure,
 * metadata may still be returned so the pending callback can receive no payload. */
esp_openclaw_response_parse_t esp_openclaw_node_parse_response(
    const char *json, size_t length, cJSON **root);

/* Reject strings that cannot survive the SDK's C-string callback contract. */
bool esp_openclaw_node_json_strings_valid(const char *json, size_t length);
