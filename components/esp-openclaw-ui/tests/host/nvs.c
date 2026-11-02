#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

const char *fake_nvs_path;
esp_err_t fake_nvs_open_error, fake_nvs_write_error, fake_nvs_commit_error;
static uint8_t saved[6], pending[6];
static unsigned index_for(const char *key)
{
    const char *keys[] = {"reading", "density", "brightness"};
    for (unsigned i = 0; i < 3; ++i) if (strcmp(keys[i], key) == 0) return i;
    assert(!"unexpected preference key");
    return 0;
}
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    assert(strcmp(name, "openclaw_ui") == 0);
    if (fake_nvs_open_error) return fake_nvs_open_error;
    if (fake_nvs_path) {
        FILE *file = fopen(fake_nvs_path, "rb");
        if (file) { assert(fread(saved, 1, sizeof(saved), file) == sizeof(saved)); fclose(file); }
    }
    memcpy(pending, saved, sizeof(saved));
    *handle = (unsigned)mode + 1;
    return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    assert(handle == 1 || handle == 2);
    unsigned i = index_for(key);
    if (!saved[i + 3]) return ESP_ERR_NVS_NOT_FOUND;
    *value = saved[i];
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    assert(handle == 2);
    if (fake_nvs_write_error) return fake_nvs_write_error;
    unsigned i = index_for(key);
    pending[i] = value; pending[i + 3] = 1;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 2);
    if (fake_nvs_commit_error) return fake_nvs_commit_error;
    memcpy(saved, pending, sizeof(saved));
    if (fake_nvs_path) {
        FILE *file = fopen(fake_nvs_path, "wb"); assert(file);
        assert(fwrite(saved, 1, sizeof(saved), file) == sizeof(saved)); fclose(file);
    }
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handle == 1 || handle == 2); }
