#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
typedef int esp_openclaw_node_connect_failure_reason_t;
typedef int esp_openclaw_node_disconnected_reason_t;
typedef enum {
    ESP_OPENCLAW_NODE_INTERNAL_IDLE,
    ESP_OPENCLAW_NODE_INTERNAL_READY,
    ESP_OPENCLAW_NODE_INTERNAL_DESTROYING,
    ESP_OPENCLAW_NODE_INTERNAL_CLOSED,
} esp_openclaw_node_internal_state_t;
typedef struct {
    esp_openclaw_node_internal_state_t state;
    struct { const char *role; } config;
} test_node_t;
typedef test_node_t *esp_openclaw_node_handle_t;

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t cleanup_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cleanup_condition = PTHREAD_COND_INITIALIZER;
static bool block_cleanup, cleanup_entered, cleanup_released, connect_failure;
static unsigned cleanup_calls, clear_calls, pending_failures, events;

static void esp_openclaw_node_lock_state(esp_openclaw_node_handle_t node)
{
    (void)node;
    assert(pthread_mutex_lock(&state_mutex) == 0);
}

static void esp_openclaw_node_unlock_state(esp_openclaw_node_handle_t node)
{
    (void)node;
    assert(pthread_mutex_unlock(&state_mutex) == 0);
}

static void esp_openclaw_node_cleanup_transport_instance(
    esp_openclaw_node_handle_t node, bool stop_client)
{
    (void)node;
    assert(stop_client);
    cleanup_calls++;
    assert(pthread_mutex_lock(&cleanup_mutex) == 0);
    cleanup_entered = true;
    assert(pthread_cond_broadcast(&cleanup_condition) == 0);
    while (block_cleanup && !cleanup_released) {
        assert(pthread_cond_wait(&cleanup_condition, &cleanup_mutex) == 0);
    }
    assert(pthread_mutex_unlock(&cleanup_mutex) == 0);
}

static void esp_openclaw_node_clear_pending_control_locked(esp_openclaw_node_handle_t node)
{
    assert(node->state == ESP_OPENCLAW_NODE_INTERNAL_IDLE);
    clear_calls++;
}

static void esp_openclaw_node_fail_pending_requests(
    esp_openclaw_node_handle_t node, const char *code, const char *message)
{
    assert(node->state == ESP_OPENCLAW_NODE_INTERNAL_IDLE);
    assert(strcmp(code, "DISCONNECTED") == 0);
    assert(strcmp(message, "Gateway connection closed") == 0);
    pending_failures++;
}

static void esp_openclaw_node_emit_connect_failed(
    esp_openclaw_node_handle_t node, int reason, esp_err_t error, const char *detail)
{
    assert(node->state == ESP_OPENCLAW_NODE_INTERNAL_IDLE);
    assert(reason == 42 && error == -7 && strcmp(detail, "detail") == 0);
    events++;
}

static void esp_openclaw_node_emit_disconnected(
    esp_openclaw_node_handle_t node, int reason, esp_err_t error)
{
    assert(node->state == ESP_OPENCLAW_NODE_INTERNAL_IDLE);
    assert(reason == 42 && error == -7);
    events++;
}

static const char *esp_err_to_name(esp_err_t error) { (void)error; return "test"; }
#define ESP_OPENCLAW_NODE_TAG "test"
#define ESP_LOGW(tag, ...) do { (void)(tag); printf(__VA_ARGS__); } while (0)

#include "completion_under_test.inc"

static void *complete(void *argument)
{
    esp_openclaw_node_handle_t node = argument;
    if (connect_failure) {
        esp_openclaw_node_complete_connect_failed(node, 42, -7, "detail", true);
    } else {
        esp_openclaw_node_complete_disconnected(node, 42, -7, true);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    connect_failure = strcmp(argv[1], "connect-failure") == 0;
    bool normal = strcmp(argv[2], "normal") == 0;
    block_cleanup = strcmp(argv[2], "during-cleanup") == 0;
    esp_openclaw_node_internal_state_t initial = ESP_OPENCLAW_NODE_INTERNAL_READY;
    if (strcmp(argv[2], "destroying") == 0) initial = ESP_OPENCLAW_NODE_INTERNAL_DESTROYING;
    if (strcmp(argv[2], "closed") == 0) initial = ESP_OPENCLAW_NODE_INTERNAL_CLOSED;
    test_node_t storage = {.state = initial, .config = {.role = "node"}};
    esp_openclaw_node_handle_t node = &storage;
    pthread_t worker;
    assert(pthread_create(&worker, NULL, complete, node) == 0);
    if (block_cleanup) {
        assert(pthread_mutex_lock(&cleanup_mutex) == 0);
        while (!cleanup_entered) {
            assert(pthread_cond_wait(&cleanup_condition, &cleanup_mutex) == 0);
        }
        esp_openclaw_node_lock_state(node);
        node->state = ESP_OPENCLAW_NODE_INTERNAL_DESTROYING;
        esp_openclaw_node_unlock_state(node);
        cleanup_released = true;
        assert(pthread_cond_broadcast(&cleanup_condition) == 0);
        assert(pthread_mutex_unlock(&cleanup_mutex) == 0);
    }
    assert(pthread_join(worker, NULL) == 0);
    assert(node->state == (normal ? ESP_OPENCLAW_NODE_INTERNAL_IDLE :
        block_cleanup ? ESP_OPENCLAW_NODE_INTERNAL_DESTROYING : initial));
    assert(cleanup_calls == (unsigned)(normal || block_cleanup));
    assert(clear_calls == (unsigned)normal);
    assert(events == (unsigned)normal);
    assert(pending_failures == (unsigned)(normal && !connect_failure));
    puts("completion state and cleanup ownership preserved");
    return 0;
}
