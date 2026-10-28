#include "openclaw_density.h"

static openclaw_density_t density;

bool openclaw_density_set(openclaw_density_t value)
{
    if (value != OPENCLAW_DENSITY_COMPACT && value != OPENCLAW_DENSITY_COMFORTABLE) return false;
    if (density == value) return false;
    density = value;
    return true;
}

openclaw_density_t openclaw_density(void) { return density; }

int openclaw_density_row_height(bool secondary)
{
    return (secondary ? 60 : 44) + (density == OPENCLAW_DENSITY_COMFORTABLE ? 16 : 0);
}

int openclaw_density_row_gap(void)
{
    return density == OPENCLAW_DENSITY_COMFORTABLE ? 8 : 4;
}
