#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_openclaw_node.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define ROOM_DEVICES_MAX 16
#define ROOM_DEVICES_MAX_JSON_BYTES 32768

typedef enum {
    ROOM_DEVICES_IDLE, ROOM_DEVICES_OFFLINE, ROOM_DEVICES_LOADING,
    ROOM_DEVICES_READY, ROOM_DEVICES_EMPTY, ROOM_DEVICES_ERROR,
    ROOM_DEVICES_TOO_LARGE, ROOM_DEVICES_TIMEOUT,
} room_devices_state_t;

typedef struct {
    char id[129], name[65], model[65];
    bool connected, connected_known, paired, paired_known;
} room_device_t;

typedef struct {
    room_devices_state_t state;
    unsigned revision;
    size_t count;
    room_device_t devices[ROOM_DEVICES_MAX];
} room_devices_snapshot_t;

esp_err_t room_device_directory_init(SemaphoreHandle_t lock, void (*wake)(void));
/* Called with the room owner lock held before replacing connection authority. */
void room_device_directory_set_owner_locked(esp_openclaw_node_handle_t client,
    uint64_t incarnation, uint32_t connection, bool ready);
esp_err_t room_device_directory_refresh(void);
void room_device_directory_cancel(void);
void room_device_directory_copy(room_devices_snapshot_t *snapshot);
/* The room supervisor parses replies after urgent Talk work. */
void room_device_directory_poll(bool defer_request);
