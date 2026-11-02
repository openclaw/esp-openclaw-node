#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    OPENCLAW_PREFERENCE_READING,
    OPENCLAW_PREFERENCE_DENSITY,
    OPENCLAW_PREFERENCE_BRIGHTNESS,
    OPENCLAW_PREFERENCE_COUNT,
} openclaw_preference_t;

/* Called before UI tasks start or under the board's display lock. Missing
 * values use the board default;
 * brightness default -1 preserves the board's automatic awake policy.
 * Malformed values never reach the renderer. */
int openclaw_preferences_load(openclaw_preference_t preference, int fallback);
esp_err_t openclaw_preferences_save(openclaw_preference_t preference, int value);
bool openclaw_preferences_saved(openclaw_preference_t preference);
