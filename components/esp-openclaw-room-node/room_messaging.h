#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_openclaw_node.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef enum {
    ROOM_MESSAGING_REQUIRED, ROOM_MESSAGING_REQUESTING, ROOM_MESSAGING_WAITING,
    ROOM_MESSAGING_CONNECTING, ROOM_MESSAGING_READY, ROOM_MESSAGING_REJECTED,
    ROOM_MESSAGING_EXPIRED, ROOM_MESSAGING_ERROR,
} room_messaging_state_t;

typedef struct {
    room_messaging_state_t state;
    unsigned revision;
    bool connected, can_request, busy;
    char request_id[129];
} room_messaging_snapshot_t;

void room_messaging_init(SemaphoreHandle_t lock, void (*wake)(void));
/* These operations share the room's state lock. CONNECTED follows SDK token
 * persistence; a request receipt alone never enables messaging. */
void room_messaging_set_operator_locked(esp_openclaw_node_handle_t client,
    uint64_t incarnation, uint32_t connection, bool ready);
void room_messaging_invalidate_locked(void);
bool room_messaging_busy_locked(void);
bool room_messaging_ready_locked(void);
void room_messaging_copy_locked(room_messaging_snapshot_t *snapshot, bool idle);
esp_err_t room_messaging_request_locked(unsigned revision, bool idle);
/* The existing supervisor is the sole RPC and reconnect execution owner. */
void room_messaging_poll(void);
