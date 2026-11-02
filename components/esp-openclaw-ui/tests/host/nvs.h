#pragma once
#include <stdint.h>
#include "esp_err.h"
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define ESP_ERR_NVS_NOT_FOUND 0x1102
typedef unsigned nvs_handle_t;
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_u8(nvs_handle_t, const char *, uint8_t *);
esp_err_t nvs_set_u8(nvs_handle_t, const char *, uint8_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
extern const char *fake_nvs_path;
extern esp_err_t fake_nvs_open_error, fake_nvs_write_error, fake_nvs_commit_error;
