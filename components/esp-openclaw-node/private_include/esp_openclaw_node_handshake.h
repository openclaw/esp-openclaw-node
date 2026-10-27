#pragma once

#include <stddef.h>

#include "cJSON.h"

/* Validate the complete handshake frame, retaining only fields consumed before
 * the connection is ready. Runtime RPC/event payloads use the normal parser. */
cJSON *esp_openclaw_node_parse_handshake(const char *json, size_t length);
