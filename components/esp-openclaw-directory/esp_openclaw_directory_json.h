#pragma once

#include "cJSON.h"
#include "esp_openclaw_directory.h"

/* Shared bounds and string validation before cJSON decodes identifiers. */
cJSON *esp_openclaw_directory_parse_payload(const char *json, size_t length,
    esp_openclaw_directory_result_t *result);
