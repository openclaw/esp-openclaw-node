#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_openclaw_node.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define ESP_OPENCLAW_DEVICES_MAX 16
#define ESP_OPENCLAW_DEVICES_MAX_JSON_BYTES 32768

typedef enum {
    ESP_OPENCLAW_DEVICES_IDLE, ESP_OPENCLAW_DEVICES_OFFLINE, ESP_OPENCLAW_DEVICES_LOADING,
    ESP_OPENCLAW_DEVICES_READY, ESP_OPENCLAW_DEVICES_EMPTY, ESP_OPENCLAW_DEVICES_ERROR,
    ESP_OPENCLAW_DEVICES_TOO_LARGE, ESP_OPENCLAW_DEVICES_TIMEOUT,
} esp_openclaw_devices_state_t;

typedef struct {
    char id[129], name[65], model[65];
    bool connected, connected_known, paired, paired_known;
} esp_openclaw_device_t;

typedef struct {
    esp_openclaw_devices_state_t state;
    unsigned revision;
    size_t count;
    esp_openclaw_device_t devices[ESP_OPENCLAW_DEVICES_MAX];
} esp_openclaw_devices_snapshot_t;

esp_err_t esp_openclaw_devices_init(SemaphoreHandle_t lock, void (*wake)(void));
/* Called with the caller's owner lock held before replacing connection authority. */
void esp_openclaw_devices_set_owner_locked(esp_openclaw_node_handle_t client,
    uint64_t incarnation, uint32_t connection, bool ready);
esp_err_t esp_openclaw_devices_refresh(void);
void esp_openclaw_devices_cancel(void);
void esp_openclaw_devices_copy(esp_openclaw_devices_snapshot_t *snapshot);
/* Call from one worker; defer requests during higher-priority work. */
void esp_openclaw_devices_poll(bool defer_request);
