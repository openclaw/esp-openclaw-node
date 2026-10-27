#pragma once

#include "esp_openclaw_directory.h"
#include "esp_openclaw_node.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef enum {
    ROOM_AGENTS_IDLE, ROOM_AGENTS_OFFLINE, ROOM_AGENTS_LOADING, ROOM_AGENTS_READY,
    ROOM_AGENTS_EMPTY, ROOM_AGENTS_ERROR, ROOM_AGENTS_TOO_LARGE, ROOM_AGENTS_TIMEOUT,
} room_agents_state_t;

typedef struct {
    room_agents_state_t state;
    unsigned revision;
    esp_openclaw_agent_page_t page;
} room_agents_snapshot_t;

esp_err_t room_agent_directory_init(SemaphoreHandle_t owner_lock, void (*wake)(void));
/* Called with the room owner lock held, before releasing/replacing authority. */
void room_agent_directory_set_owner_locked(esp_openclaw_node_handle_t client,
    uint64_t incarnation, uint32_t connection, bool ready);
esp_err_t room_agent_directory_refresh(size_t offset);
void room_agent_directory_cancel(void);
void room_agent_directory_copy(room_agents_snapshot_t *snapshot);
/* The existing room supervisor calls this after urgent Talk work. */
void room_agent_directory_poll(bool defer_request);
