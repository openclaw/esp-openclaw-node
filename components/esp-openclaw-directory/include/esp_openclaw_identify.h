#pragma once

#include "esp_openclaw_devices.h"

typedef enum {
    ESP_OPENCLAW_IDENTIFY_IDLE, ESP_OPENCLAW_IDENTIFY_REVIEW,
    ESP_OPENCLAW_IDENTIFY_SENDING, ESP_OPENCLAW_IDENTIFY_ACCEPTED,
    ESP_OPENCLAW_IDENTIFY_RENDERED, ESP_OPENCLAW_IDENTIFY_UNKNOWN,
    ESP_OPENCLAW_IDENTIFY_FAILED,
} esp_openclaw_identify_state_t;

typedef struct {
    esp_openclaw_identify_state_t state;
    unsigned revision;
    esp_openclaw_device_t target;
    char text[23];
    bool can_send;
} esp_openclaw_identify_snapshot_t;

esp_err_t esp_openclaw_identify_init(SemaphoreHandle_t lock, void (*wake)(void));
/* The shared reader owner holds lock for authority updates. */
void esp_openclaw_identify_set_owner_locked(esp_openclaw_node_handle_t client,
    uint64_t incarnation, uint32_t connection, bool ready, bool writable);
esp_err_t esp_openclaw_identify_begin(const char *node_id, unsigned inventory_revision);
esp_err_t esp_openclaw_identify_send(unsigned revision);
void esp_openclaw_identify_copy(esp_openclaw_identify_snapshot_t *snapshot);
void esp_openclaw_identify_poll(bool defer_request);
