#include "openclaw_density.h"
#include "openclaw_preferences.h"

static openclaw_density_t density;
static bool loaded;

bool openclaw_density_set(openclaw_density_t value)
{
    if (value != OPENCLAW_DENSITY_COMPACT && value != OPENCLAW_DENSITY_COMFORTABLE) return false;
    (void)openclaw_density();
    if (density == value && openclaw_preferences_saved(OPENCLAW_PREFERENCE_DENSITY)) return false;
    if (openclaw_preferences_save(OPENCLAW_PREFERENCE_DENSITY, value) != ESP_OK) return false;
    density = value;
    return true;
}

openclaw_density_t openclaw_density(void)
{
    if (!loaded) {
        density = (openclaw_density_t)openclaw_preferences_load(OPENCLAW_PREFERENCE_DENSITY, OPENCLAW_DENSITY_COMPACT);
        loaded = true;
    }
    return density;
}

int openclaw_density_row_height(bool secondary)
{
    return (secondary ? 60 : 44) + (openclaw_density() == OPENCLAW_DENSITY_COMFORTABLE ? 16 : 0);
}

int openclaw_density_row_gap(void)
{
    return openclaw_density() == OPENCLAW_DENSITY_COMFORTABLE ? 8 : 4;
}
