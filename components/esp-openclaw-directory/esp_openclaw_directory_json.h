#pragma once

#include "cJSON.h"
#include "esp_openclaw_directory.h"

/* Shared bounds and string validation before cJSON decodes identifiers. */
bool esp_openclaw_directory_payload_bounds(const char *json, size_t length,
    esp_openclaw_directory_result_t *result);
cJSON *esp_openclaw_directory_parse_payload(const char *json, size_t length,
    esp_openclaw_directory_result_t *result);
