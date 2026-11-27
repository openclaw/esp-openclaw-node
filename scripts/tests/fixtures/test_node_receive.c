// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_OPENCLAW_NODE_MAX_INBOUND_MESSAGE_SIZE 64
#define ESP_OPENCLAW_NODE_TAG "test"
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
enum { ESP_OPENCLAW_NODE_INTERNAL_CONNECTING, ESP_OPENCLAW_NODE_INTERNAL_READY,
       ESP_OPENCLAW_NODE_INTERNAL_DESTROYING, ESP_OPENCLAW_NODE_INTERNAL_CLOSED };
typedef int esp_openclaw_node_internal_state_t;
typedef struct node {
    esp_openclaw_node_internal_state_t state;
    uint32_t active_transport_id, fatal_transport_id, failed_receive_transport_id;
    esp_err_t fatal_transport_err;
    char *rx_buffer;
    size_t rx_buffer_len, rx_frame_len, rx_frame_received;
    int rx_frame_opcode;
    bool rx_frame_fin, rx_message_started;
} *esp_openclaw_node_handle_t;
typedef struct { esp_openclaw_node_handle_t node; uint32_t transport_id; } esp_openclaw_node_transport_event_ctx_t;
typedef const void *esp_event_base_t;
enum { WEBSOCKET_EVENT_CONNECTED, WEBSOCKET_EVENT_DISCONNECTED, WEBSOCKET_EVENT_CLOSED,
       WEBSOCKET_EVENT_ERROR, WEBSOCKET_EVENT_DATA };
typedef struct {
    const char *data_ptr;
    int data_len, payload_len, payload_offset, op_code;
    bool fin;
    struct { int esp_tls_last_esp_err, esp_transport_sock_errno; } error_handle;
} esp_websocket_event_data_t;
enum { ESP_OPENCLAW_NODE_WORK_MSG_WS_CONNECTED, ESP_OPENCLAW_NODE_WORK_MSG_WS_DISCONNECTED,
       ESP_OPENCLAW_NODE_WORK_MSG_WS_ERROR, ESP_OPENCLAW_NODE_WORK_MSG_WS_FATAL, ESP_OPENCLAW_NODE_WORK_MSG_DATA };
typedef struct { int type; uint32_t transport_id; esp_err_t local_err; char *text; } esp_openclaw_node_work_message_t;
typedef struct { size_t total_free_bytes, largest_free_block; } multi_heap_info_t;
static esp_openclaw_node_work_message_t messages[16];
static size_t count, allocation_calls;
static bool fail_allocation;
static int locked;
static char last_log[1024];

void esp_openclaw_node_lock_state(esp_openclaw_node_handle_t node) { (void)node; assert(!locked); locked = 1; }
void esp_openclaw_node_unlock_state(esp_openclaw_node_handle_t node) { (void)node; assert(locked); locked = 0; }
bool esp_openclaw_node_state_is_connecting(int state) { return state == ESP_OPENCLAW_NODE_INTERNAL_CONNECTING; }
const char *esp_openclaw_node_internal_state_name(int state) { (void)state; return "connecting"; }
bool esp_openclaw_node_should_accept_callback_transport_id_locked(esp_openclaw_node_handle_t, uint32_t);
void esp_openclaw_node_enqueue_work_message_from_callback(esp_openclaw_node_handle_t node, esp_openclaw_node_work_message_t *message) {
    (void)node; assert(!locked && count < 16); messages[count++] = *message;
}
void heap_caps_get_info(multi_heap_info_t *info, unsigned caps) {
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    *info = (multi_heap_info_t){1024, 512};
}
static void log_message(const char *format, ...) {
    va_list args; va_start(args, format); vsnprintf(last_log, sizeof(last_log), format, args); va_end(args);
}
#define ESP_LOGE(tag, ...) log_message(__VA_ARGS__)
#define ESP_LOGW(tag, ...) log_message(__VA_ARGS__)
#define ESP_LOGD(tag, ...) ((void)0)
static void *receive_realloc(void *memory, size_t size) {
    ++allocation_calls;
    return fail_allocation ? NULL : realloc(memory, size);
}
#include "transport_clear.inc"
#include "transport_helpers.inc"
#define realloc receive_realloc
#include "transport_receive.inc"
#undef realloc

static void emit(esp_openclaw_node_handle_t node, int opcode, const char *data,
                 int bytes, int payload, int offset, bool fin) {
    esp_openclaw_node_transport_event_ctx_t context = {node, node->active_transport_id};
    esp_websocket_event_data_t event = {.data_ptr = data, .data_len = bytes, .payload_len = payload,
        .payload_offset = offset, .op_code = opcode, .fin = fin};
    websocket_event_handler(&context, NULL, WEBSOCKET_EVENT_DATA, &event);
    assert(!locked);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    struct node node = {.state = ESP_OPENCLAW_NODE_INTERNAL_CONNECTING, .active_transport_id = 7};
    if (strcmp(argv[1], "allocation") == 0 || strcmp(argv[1], "allocation-continuation") == 0 ||
        strcmp(argv[1], "fatal-handoff") == 0) {
        int opcode = 1;
        size_t before = 0;
        if (strcmp(argv[1], "allocation-continuation") == 0) {
            emit(&node, 1, "xy", 2, 2, 0, false);
            opcode = 0;
            before = allocation_calls;
        }
        fail_allocation = true;
        emit(&node, opcode, "ab", 2, 4, 0, true);
        assert(count == 1 && messages[0].type == ESP_OPENCLAW_NODE_WORK_MSG_WS_FATAL);
        assert(messages[0].local_err == ESP_ERR_NO_MEM && node.fatal_transport_err == ESP_ERR_NO_MEM);
        if (strcmp(argv[1], "fatal-handoff") == 0) {
            /* The worker takes its pending fatal event before transport teardown. */
            node.fatal_transport_id = 0;
            node.fatal_transport_err = ESP_OK;
        }
        emit(&node, opcode, "cd", 2, 4, 2, true);
        assert(count == 1 && allocation_calls == before + 1 && node.rx_buffer == NULL);
        assert(node.failed_receive_transport_id == 7);
    } else if (strcmp(argv[1], "oversize") == 0) {
        emit(&node, 1, "a", 1, 65, 0, true);
        assert(count == 1 && messages[0].local_err == ESP_ERR_INVALID_SIZE && allocation_calls == 0);
    } else if (strcmp(argv[1], "malformed") == 0) {
        emit(&node, 0, "a", 1, 1, 0, true);
        assert(count == 1 && messages[0].local_err == ESP_ERR_INVALID_SIZE);
    } else if (strcmp(argv[1], "empty-frames") == 0) {
        emit(&node, 1, NULL, 0, 0, 0, false);
        emit(&node, 0, NULL, 0, 2, 0, true);
        emit(&node, 0, "ab", 2, 2, 0, true);
        assert(count == 1 && messages[0].type == ESP_OPENCLAW_NODE_WORK_MSG_DATA);
        assert(strcmp(messages[0].text, "ab") == 0);
        emit(&node, 1, NULL, 0, 0, 0, true);
        assert(count == 2 && messages[1].type == ESP_OPENCLAW_NODE_WORK_MSG_DATA && messages[1].text[0] == '\0');
    } else if (strcmp(argv[1], "exact-limit") == 0) {
        char text[65]; memset(text, 'x', 64); text[64] = '\0';
        emit(&node, 1, text, 32, 64, 0, true);
        emit(&node, 1, text + 32, 32, 64, 32, true);
        assert(count == 1 && messages[0].type == ESP_OPENCLAW_NODE_WORK_MSG_DATA);
        assert(strcmp(messages[0].text, text) == 0);
    } else if (strcmp(argv[1], "recovery") == 0) {
        fail_allocation = true;
        emit(&node, 1, "ab", 2, 2, 0, true);
        assert(count == 1 && messages[0].local_err == ESP_ERR_NO_MEM);
        fail_allocation = false;
        node.active_transport_id = 8;
        node.fatal_transport_id = 0;
        node.fatal_transport_err = ESP_OK;
        node.failed_receive_transport_id = 0;
        emit(&node, 1, "cd", 2, 2, 0, true);
        assert(count == 2 && messages[1].type == ESP_OPENCLAW_NODE_WORK_MSG_DATA);
        assert(strcmp(messages[1].text, "cd") == 0);
    } else if (strcmp(argv[1], "bad-offset") == 0 || strcmp(argv[1], "changed-empty-header") == 0) {
        bool empty_header = strcmp(argv[1], "changed-empty-header") == 0;
        emit(&node, 1, empty_header ? NULL : "ab", empty_header ? 0 : 2, 4, 0, true);
        emit(&node, 1, "cd", 2, empty_header ? 5 : 4, 0, true);
        assert(count == 1 && messages[0].type == ESP_OPENCLAW_NODE_WORK_MSG_WS_FATAL);
        assert(messages[0].local_err == ESP_ERR_INVALID_SIZE && node.rx_buffer == NULL);
    } else {
        if (strcmp(argv[1], "zero-first") == 0) {
            emit(&node, 1, NULL, 0, 4, 0, true);
            emit(&node, 1, NULL, 0, 4, 0, true);
        }
        emit(&node, 1, "ab", 2, 4, 0, strcmp(argv[1], "valid") != 0);
        if (strcmp(argv[1], "zero-middle") == 0) emit(&node, 1, NULL, 0, 4, 2, true);
        emit(&node, 1, "cd", 2, 4, 2, strcmp(argv[1], "valid") != 0);
        if (strcmp(argv[1], "valid") == 0) emit(&node, 0, "ef", 2, 2, 0, true);
        assert(count == 1 && messages[0].type == ESP_OPENCLAW_NODE_WORK_MSG_DATA);
        assert(strcmp(messages[0].text, strcmp(argv[1], "valid") == 0 ? "abcdef" : "abcd") == 0);
        assert(node.rx_buffer == NULL && !node.rx_message_started);
    }
    for (size_t i = 0; i < count; ++i) free(messages[i].text);
    esp_openclaw_node_clear_data_buffer_locked(&node);
    printf("receive scenario %s passed\n", argv[1]);
}
