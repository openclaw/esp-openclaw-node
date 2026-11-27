#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t generation, rendered_generation;
    bool rendered;
} esp_openclaw_display_receipt_t;

/* A compact display receipt must match the requested target and exact content.
 * A valid receipt records acceptance; rendered is a separate observation. */
bool esp_openclaw_display_receipt_parse(const char *json, size_t length,
    const char *node_id, const char *command, const char *heading, const char *text,
    esp_openclaw_display_receipt_t *receipt);
