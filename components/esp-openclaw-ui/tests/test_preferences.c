#include <assert.h>
#include <string.h>
#include "nvs.h"
#include "openclaw_preferences.h"
#include "openclaw_reading.h"
#include "openclaw_density.h"

int main(int argc, char **argv)
{
    assert(argc == 3);
    fake_nvs_path = argv[2];
    if (strcmp(argv[1], "write") == 0) {
        assert(openclaw_reading_size() == OPENCLAW_READING_STANDARD);
        assert(openclaw_density() == OPENCLAW_DENSITY_COMPACT);
        assert(openclaw_preferences_load(OPENCLAW_PREFERENCE_BRIGHTNESS, 65) == 65);
        assert(openclaw_preferences_load(OPENCLAW_PREFERENCE_BRIGHTNESS, -1) == -1);
        assert(openclaw_reading_set_size(OPENCLAW_READING_LARGE));
        assert(openclaw_density_set(OPENCLAW_DENSITY_COMFORTABLE));
        assert(openclaw_preferences_save(OPENCLAW_PREFERENCE_BRIGHTNESS, 35) == ESP_OK);
    } else if (strcmp(argv[1], "reboot") == 0) {
        assert(openclaw_reading_size() == OPENCLAW_READING_LARGE);
        assert(openclaw_density() == OPENCLAW_DENSITY_COMFORTABLE);
        assert(openclaw_preferences_load(OPENCLAW_PREFERENCE_BRIGHTNESS, 65) == 35);
        fake_nvs_commit_error = 99;
        assert(!openclaw_reading_set_size(OPENCLAW_READING_STANDARD));
        assert(openclaw_reading_size() == OPENCLAW_READING_LARGE);
        assert(!openclaw_preferences_saved(OPENCLAW_PREFERENCE_READING));
        fake_nvs_commit_error = 0;
        assert(openclaw_reading_set_size(OPENCLAW_READING_STANDARD));
        assert(openclaw_preferences_saved(OPENCLAW_PREFERENCE_READING));
        fake_nvs_write_error = 98;
        assert(!openclaw_density_set(OPENCLAW_DENSITY_COMPACT));
        assert(openclaw_density() == OPENCLAW_DENSITY_COMFORTABLE);
        fake_nvs_write_error = 0;
        assert(openclaw_density_set(OPENCLAW_DENSITY_COMPACT));
    } else {
        /* The raw NVS boundary can contain corrupt or obsolete values. */
        nvs_handle_t handle;
        assert(nvs_open("openclaw_ui", NVS_READWRITE, &handle) == ESP_OK);
        assert(nvs_set_u8(handle, "reading", 255) == ESP_OK);
        assert(nvs_set_u8(handle, "density", 255) == ESP_OK);
        assert(nvs_set_u8(handle, "brightness", 0) == ESP_OK);
        assert(nvs_commit(handle) == ESP_OK); nvs_close(handle);
        assert(openclaw_reading_size() == OPENCLAW_READING_STANDARD);
        assert(openclaw_density() == OPENCLAW_DENSITY_COMPACT);
        assert(openclaw_preferences_load(OPENCLAW_PREFERENCE_BRIGHTNESS, 65) == 65);
        assert(!openclaw_preferences_saved(OPENCLAW_PREFERENCE_BRIGHTNESS));
        assert(openclaw_preferences_save(OPENCLAW_PREFERENCE_BRIGHTNESS, 9) == ESP_ERR_INVALID_ARG);
        assert(openclaw_preferences_save(OPENCLAW_PREFERENCE_BRIGHTNESS, 101) == ESP_ERR_INVALID_ARG);
        assert(openclaw_reading_set_size(OPENCLAW_READING_STANDARD));
        fake_nvs_open_error = 97;
        assert(openclaw_preferences_save(OPENCLAW_PREFERENCE_BRIGHTNESS, 40) == 97);
        assert(!openclaw_preferences_saved(OPENCLAW_PREFERENCE_BRIGHTNESS));
    }
    return 0;
}
