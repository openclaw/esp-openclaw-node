#include "openclaw_reading.h"

static openclaw_reading_size_t size;

bool openclaw_reading_set_size(openclaw_reading_size_t value)
{
    if (value != OPENCLAW_READING_STANDARD && value != OPENCLAW_READING_LARGE) return false;
    if (size == value) return false;
    size = value;
    return true;
}

openclaw_reading_size_t openclaw_reading_size(void) { return size; }

const lv_font_t *openclaw_reading_font(void)
{
#if LV_FONT_MONTSERRAT_20
    if (size == OPENCLAW_READING_LARGE) return &lv_font_montserrat_20;
#endif
    return &lv_font_montserrat_14;
}
