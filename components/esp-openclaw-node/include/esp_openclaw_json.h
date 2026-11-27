#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef bool (*esp_openclaw_json_value_fn)(const char *json, size_t length, void *context);

/* Validate the complete object before visiting a top-level array by printable
 * ASCII key. Spans borrow the caller's unchanged input; returning false stops
 * the walk. The first matching key wins, including escaped keys. Nesting is
 * limited to 64 containers. No JSON objects are allocated. */
bool esp_openclaw_json_visit_array(const char *json, size_t length, const char *key,
                                 esp_openclaw_json_value_fn visit, void *context);
