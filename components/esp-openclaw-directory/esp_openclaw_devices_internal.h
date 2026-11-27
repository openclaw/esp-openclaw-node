#pragma once

#include "esp_openclaw_devices.h"

/* Requires the directory's owner lock. Only the two qualified compact display
 * adapters share the Identify command contract. Advertisement is not authority. */
bool esp_openclaw_devices_identify_target_locked(const char *node_id,
    unsigned revision, esp_openclaw_device_t *target);
