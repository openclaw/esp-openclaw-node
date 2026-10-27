#pragma once

#include <stdbool.h>
#include <stdint.h>

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

typedef struct {
    room_agents_state_t state, preview_state;
    unsigned revision, preview_revision;
    esp_openclaw_agent_t agent;
    esp_openclaw_session_page_t page;
    esp_openclaw_session_preview_t preview;
    size_t selected;
} room_conversations_snapshot_t;

esp_err_t room_agent_directory_init(SemaphoreHandle_t owner_lock, void (*wake)(void));
/* Called with the room owner lock held, before releasing/replacing authority. */
void room_agent_directory_set_owner_locked(esp_openclaw_node_handle_t client,
    uint64_t incarnation, uint32_t connection, bool ready);
esp_err_t room_agent_directory_refresh(size_t offset);
void room_agent_directory_cancel(void);
/* Hide the browser without dropping its reader; explicit cancel still clears it. */
void room_agent_directory_suspend(void);
void room_agent_directory_copy(room_agents_snapshot_t *snapshot);
/* Browsing never changes the current or next Talk target. */
esp_err_t room_conversations_open(const char *agent_id, unsigned agents_revision);
esp_err_t room_conversations_refresh(size_t offset);
esp_err_t room_conversations_preview(size_t index, unsigned revision);
void room_conversations_close(void);
void room_conversations_copy(room_conversations_snapshot_t *snapshot);
/* Copy one live roster/reader pair under the owner lock. The display checks the
 * epoch again after acquiring its own lock, without reversing lock order. */
esp_err_t room_conversations_copy_resume(room_agents_snapshot_t *agents,
    room_conversations_snapshot_t *reader, unsigned *epoch);
typedef struct {
    unsigned epoch;
    char agent_name[65];
} room_conversation_context_t;
/* Bounded Home projection; no messages, allocation, or directory request.
 * Revalidate its epoch before admitting a later Continue action. */
esp_err_t room_conversations_copy_context(room_conversation_context_t *context);
bool room_conversations_resume_current(unsigned epoch);
typedef struct {
    esp_openclaw_agent_t agent;
    char main_key[129];
} room_agent_target_t;
/* Caller holds the room owner lock; resolve only a row from this live revision. */
esp_err_t room_agent_directory_resolve_locked(unsigned revision, const char *id,
    room_agent_target_t *target);
/* The existing room supervisor calls this after urgent Talk work. */
void room_agent_directory_poll(bool defer_request);
