#include "room_agent_directory.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"

#define DIRECTORY_TIMEOUT_US (15 * 1000 * 1000)

typedef struct {
    esp_openclaw_node_handle_t client;
    uint64_t incarnation;
    uint32_t connection;
    unsigned revision;
    size_t offset;
    int64_t deadline;
    room_agents_state_t state;
    char *json;
} directory_request_t;

static struct {
    SemaphoreHandle_t lock;
    void (*wake)(void);
    esp_timer_handle_t timer;
    esp_openclaw_node_handle_t client;
    uint64_t incarnation;
    uint32_t connection;
    bool ready, interested, pending;
    size_t offset;
    unsigned revision;
    room_agents_state_t state;
    esp_openclaw_agent_page_t *page;
    directory_request_t *inflight;
} directory;
static _Atomic(directory_request_t *) completed;

static void deadline_fired(void *arg)
{
    (void)arg;
    directory.wake();
}

esp_err_t room_agent_directory_init(SemaphoreHandle_t owner_lock, void (*wake)(void))
{
    directory.lock = owner_lock;
    directory.wake = wake;
    const esp_timer_create_args_t args = {.callback = deadline_fired, .name = "agent_directory"};
    return esp_timer_create(&args, &directory.timer);
}

static void invalidate(void)
{
    ++directory.revision;
    free(directory.page);
    directory.page = NULL;
    directory.pending = directory.interested;
    directory.state = !directory.interested ? ROOM_AGENTS_IDLE
        : directory.ready ? ROOM_AGENTS_LOADING : ROOM_AGENTS_OFFLINE;
}

void room_agent_directory_set_owner_locked(esp_openclaw_node_handle_t client,
    uint64_t incarnation, uint32_t connection, bool ready)
{
    ready = ready && client != NULL && connection != 0;
    if (directory.client == client && directory.incarnation == incarnation &&
        directory.connection == connection && directory.ready == ready) return;
    directory.client = client;
    directory.incarnation = incarnation;
    directory.connection = connection;
    directory.ready = ready;
    invalidate();
}

esp_err_t room_agent_directory_refresh(size_t offset)
{
    if (directory.lock == NULL || directory.timer == NULL) return ESP_ERR_INVALID_STATE;
    if (offset > SIZE_MAX - ESP_OPENCLAW_DIRECTORY_PAGE_SIZE) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(directory.lock, portMAX_DELAY);
    directory.interested = true;
    directory.offset = offset;
    invalidate();
    xSemaphoreGive(directory.lock);
    directory.wake();
    return ESP_OK;
}

void room_agent_directory_cancel(void)
{
    if (directory.lock == NULL) return;
    xSemaphoreTake(directory.lock, portMAX_DELAY);
    directory.interested = false;
    invalidate();
    xSemaphoreGive(directory.lock);
    directory.wake();
}

void room_agent_directory_copy(room_agents_snapshot_t *snapshot)
{
    *snapshot = (room_agents_snapshot_t){0};
    if (directory.lock == NULL) return;
    xSemaphoreTake(directory.lock, portMAX_DELAY);
    snapshot->state = directory.state;
    snapshot->revision = directory.revision;
    if (directory.page != NULL) snapshot->page = *directory.page;
    xSemaphoreGive(directory.lock);
}

static bool current(const directory_request_t *request)
{
    return directory.ready && directory.interested &&
        request->client == directory.client && request->incarnation == directory.incarnation &&
        request->connection == directory.connection && request->revision == directory.revision;
}

esp_err_t room_agent_directory_resolve_locked(unsigned revision, const char *id,
    room_agent_target_t *target)
{
    if (id == NULL || target == NULL) return ESP_ERR_INVALID_ARG;
    if (!directory.ready || directory.state != ROOM_AGENTS_READY ||
        revision != directory.revision || directory.page == NULL ||
        directory.page->main_key[0] == '\0') return ESP_ERR_INVALID_STATE;
    for (size_t i = 0; i < directory.page->count; ++i) {
        const esp_openclaw_agent_t *agent = &directory.page->agents[i];
        if (strcmp(agent->id, id) != 0) continue;
        target->agent = *agent;
        memcpy(target->main_key, directory.page->main_key, sizeof(target->main_key));
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

static void response(esp_openclaw_node_handle_t client,
    const esp_openclaw_node_gateway_result_t *result, void *context)
{
    (void)client;
    directory_request_t *request = context;
    request->state = ROOM_AGENTS_ERROR;
    if (result->ok && result->payload_json != NULL) {
        size_t length = strnlen(result->payload_json, ESP_OPENCLAW_DIRECTORY_MAX_JSON_BYTES + 1);
        if (length > ESP_OPENCLAW_DIRECTORY_MAX_JSON_BYTES) request->state = ROOM_AGENTS_TOO_LARGE;
        else {
            request->json = malloc(length + 1);
            if (request->json != NULL) memcpy(request->json, result->payload_json, length + 1);
        }
    }
    /* One accepted request owns the mailbox until the supervisor consumes it. */
    atomic_store(&completed, request);
    directory.wake();
}

void room_agent_directory_poll(bool defer_request)
{
    if (directory.lock == NULL || directory.timer == NULL) return;
    directory_request_t *done = atomic_exchange(&completed, NULL);
    if (done != NULL) {
        esp_openclaw_agent_page_t *page = NULL;
        if (done->json != NULL) {
            page = malloc(sizeof(*page));
            if (page != NULL) {
                esp_openclaw_directory_result_t result = esp_openclaw_directory_parse(
                    done->json, strlen(done->json), done->offset, page);
                done->state = result == ESP_OPENCLAW_DIRECTORY_OK
                    ? page->count == 0 ? ROOM_AGENTS_EMPTY : ROOM_AGENTS_READY
                    : result == ESP_OPENCLAW_DIRECTORY_TOO_LARGE ? ROOM_AGENTS_TOO_LARGE : ROOM_AGENTS_ERROR;
            }
        }
        esp_timer_stop(directory.timer);
        xSemaphoreTake(directory.lock, portMAX_DELAY);
        directory.inflight = NULL;
        if (current(done)) {
            bool expired = esp_timer_get_time() >= done->deadline;
            directory.state = expired ? ROOM_AGENTS_TIMEOUT : done->state;
            if (!expired && (done->state == ROOM_AGENTS_READY || done->state == ROOM_AGENTS_EMPTY)) {
                directory.page = page;
                page = NULL;
            }
        }
        xSemaphoreGive(directory.lock);
        free(page);
        free(done->json);
        free(done);
    }
    xSemaphoreTake(directory.lock, portMAX_DELAY);
    if (directory.inflight != NULL && current(directory.inflight) &&
        esp_timer_get_time() >= directory.inflight->deadline) {
        ++directory.revision;
        directory.state = ROOM_AGENTS_TIMEOUT;
        /* Keep the callback context. This shared socket also owns Talk and Canvas;
         * a timeout must neither disconnect it nor accumulate replacement RPCs. */
    }
    if (defer_request || !directory.pending || !directory.ready || directory.inflight != NULL) {
        xSemaphoreGive(directory.lock);
        return;
    }
    directory_request_t *request = calloc(1, sizeof(*request));
    directory.pending = false;
    if (request == NULL) {
        directory.state = ROOM_AGENTS_ERROR;
        xSemaphoreGive(directory.lock);
        return;
    }
    *request = (directory_request_t){.client = directory.client,
        .incarnation = directory.incarnation, .connection = directory.connection,
        .revision = directory.revision, .offset = directory.offset,
        .deadline = esp_timer_get_time() + DIRECTORY_TIMEOUT_US, .state = ROOM_AGENTS_ERROR};
    directory.inflight = request;
    xSemaphoreGive(directory.lock);
    esp_err_t err = esp_timer_start_once(directory.timer, DIRECTORY_TIMEOUT_US);
    if (err == ESP_OK) err = esp_openclaw_node_gateway_request_on_connection(request->client,
        request->connection, "agents.list", "{}", response, request);
    if (err != ESP_OK) {
        atomic_store(&completed, request);
        directory.wake();
    }
}
