#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_openclaw_node.h"
#include "esp_openclaw_device_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

esp_err_t esp_openclaw_devices_init(SemaphoreHandle_t lock, void (*wake)(void));
/* Called with the caller's owner lock held before replacing connection authority. */
void esp_openclaw_devices_set_owner_locked(esp_openclaw_node_handle_t client,
    uint64_t incarnation, uint32_t connection, bool ready);
esp_err_t esp_openclaw_devices_refresh(void);
void esp_openclaw_devices_cancel(void);
void esp_openclaw_devices_copy(esp_openclaw_devices_snapshot_t *snapshot);
/* Call from one worker; defer requests during higher-priority work. */
void esp_openclaw_devices_poll(bool defer_request);
