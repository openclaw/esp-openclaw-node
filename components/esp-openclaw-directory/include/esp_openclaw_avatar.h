#pragma once

#include <stdint.h>

#include "esp_openclaw_directory.h"

#define ESP_OPENCLAW_AVATAR_SIZE 64

typedef enum {
    ESP_OPENCLAW_AVATAR_TEXT,
    ESP_OPENCLAW_AVATAR_FOX,
    ESP_OPENCLAW_AVATAR_LOBSTER,
} esp_openclaw_avatar_kind_t;

typedef struct {
    esp_openclaw_avatar_kind_t kind;
    char text[3];
} esp_openclaw_avatar_t;

/* Configured identity wins over the display name. Unsupported emoji use ASCII
 * initials so embedded fonts never receive an unsupported avatar glyph. */
esp_openclaw_avatar_t esp_openclaw_agent_avatar(const esp_openclaw_agent_t *agent);

/* Immutable 64x64 straight-alpha BGRA pixels, or NULL for a text avatar. */
const uint8_t *esp_openclaw_avatar_pixels(esp_openclaw_avatar_kind_t kind);
