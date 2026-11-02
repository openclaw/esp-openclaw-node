#include "openclaw_reading.h"
#include "openclaw_preferences.h"

static openclaw_reading_size_t size;
static bool loaded;

bool openclaw_reading_set_size(openclaw_reading_size_t value)
{
    if (value != OPENCLAW_READING_STANDARD && value != OPENCLAW_READING_LARGE) return false;
    (void)openclaw_reading_size();
    if (size == value && openclaw_preferences_saved(OPENCLAW_PREFERENCE_READING)) return false;
    if (openclaw_preferences_save(OPENCLAW_PREFERENCE_READING, value) != ESP_OK) return false;
    size = value;
    return true;
}

openclaw_reading_size_t openclaw_reading_size(void)
{
    if (!loaded) {
        size = (openclaw_reading_size_t)openclaw_preferences_load(OPENCLAW_PREFERENCE_READING, OPENCLAW_READING_STANDARD);
        loaded = true;
    }
    return size;
}

const lv_font_t *openclaw_reading_font(void)
{
#if LV_FONT_MONTSERRAT_20
    if (openclaw_reading_size() == OPENCLAW_READING_LARGE) return &lv_font_montserrat_20;
#endif
    return &lv_font_montserrat_14;
}
