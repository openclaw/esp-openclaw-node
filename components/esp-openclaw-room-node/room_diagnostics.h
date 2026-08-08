#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** Open/close the shared top-layer modal from the LVGL task. */
esp_err_t room_diagnostics_open(void);
esp_err_t room_diagnostics_close(void);

/** Thread-safe external-task entry points; the operation executes on taskLVGL. */
esp_err_t room_diagnostics_request_open(void);
esp_err_t room_diagnostics_request_close(void);

bool room_diagnostics_is_open(void);
