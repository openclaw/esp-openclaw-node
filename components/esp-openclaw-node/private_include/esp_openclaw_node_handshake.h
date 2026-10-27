#pragma once

#include <stddef.h>
#include <stdbool.h>

#include "cJSON.h"

/* Validate the complete handshake frame, retaining only fields consumed before
 * the connection is ready. Runtime RPC/event payloads use the normal parser. */
cJSON *esp_openclaw_node_parse_handshake(const char *json, size_t length);

/* Reject strings that cannot survive the SDK's C-string callback contract. */
bool esp_openclaw_node_json_strings_valid(const char *json, size_t length);
