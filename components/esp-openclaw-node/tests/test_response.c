#include "esp_openclaw_node_handshake.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t allocated, peak, budget;
typedef union { max_align_t alignment; size_t size; } allocation_t;
static void *limited_malloc(size_t size)
{
    if (size > budget - allocated) return NULL;
    allocation_t *allocation = malloc(sizeof(*allocation) + size);
    assert(allocation != NULL);
    allocation->size = size;
    allocated += size;
    if (allocated > peak) peak = allocated;
    return allocation + 1;
}
static void limited_free(void *pointer)
{
    if (pointer == NULL) return;
    allocation_t *allocation = (allocation_t *)pointer - 1;
    allocated -= allocation->size;
    free(allocation);
}

typedef struct test_node *esp_openclaw_node_handle_t;
typedef struct { bool ok; const char *payload_json, *error_code, *error_message; } esp_openclaw_node_gateway_result_t;
typedef void (*esp_openclaw_node_gateway_request_cb_t)(esp_openclaw_node_handle_t,
    const esp_openclaw_node_gateway_result_t *, void *);
typedef struct {
    bool in_use;
    char request_id[40];
    esp_openclaw_node_gateway_request_cb_t callback;
    void *user_ctx;
} esp_openclaw_node_pending_request_t;
struct test_node { esp_openclaw_node_pending_request_t pending_requests[2]; };
#define ESP_OPENCLAW_NODE_MAX_PENDING_REQUESTS 2
#define ESP_OPENCLAW_NODE_TAG "test"
#define ESP_ERR_NO_MEM 257
#define ESP_LOGW(tag, ...) ((void)(tag), (void)printf(__VA_ARGS__), (void)putchar('\n'))
static unsigned locked, callbacks;
static bool expected_ok = true;
static const char *expected_error;
static char *retained;
static const char *expected;
static const char *connect_diagnostic_role(esp_openclaw_node_handle_t node) { (void)node; return "operator"; }
static void esp_openclaw_node_lock_state(esp_openclaw_node_handle_t node) { (void)node; assert(!locked++); }
static void esp_openclaw_node_unlock_state(esp_openclaw_node_handle_t node) { (void)node; assert(locked-- == 1); }
#include "response.inc"

static void capture(esp_openclaw_node_handle_t node, const esp_openclaw_node_gateway_result_t *result, void *context)
{
    (void)node; (void)context;
    assert(!locked && result->ok == expected_ok);
    if (expected_error != NULL) assert(strcmp(result->error_code, expected_error) == 0);
    ++callbacks;
    if (expected == NULL) { assert(result->payload_json == NULL); return; }
    assert(result->payload_json != NULL && strcmp(result->payload_json, expected) == 0);
    size_t size = strlen(result->payload_json) + 1;
    retained = cJSON_malloc(size);
    assert(retained != NULL);
    memcpy(retained, result->payload_json, size);
}

static void deliver(cJSON *root, const char *payload)
{
    struct test_node node = {0};
    node.pending_requests[0] = (esp_openclaw_node_pending_request_t){.in_use = true, .callback = capture};
    strcpy(node.pending_requests[0].request_id, "rpc-1");
    expected = payload;
    unsigned before = callbacks;
    assert(!handle_gateway_response(&node, "unknown", root));
    assert(callbacks == before && node.pending_requests[0].in_use);
    assert(handle_gateway_response(&node, "rpc-1", root));
    assert(callbacks == before + 1 && !node.pending_requests[0].in_use);
    assert(!handle_gateway_response(&node, "rpc-1", root) && callbacks == before + 1);
}

int main(void)
{
    cJSON_Hooks hooks = {.malloc_fn = limited_malloc, .free_fn = limited_free};
    cJSON_InitHooks(&hooks);
    budget = 65536;
    const char *values[] = {"{ \"x\": [1, true, null] }", "[1,2]", "\"hello\\nworld\"", "null", "false", "1e2"};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        char json[160];
        snprintf(json, sizeof(json), "{\"type\":\"res\",\"id\":\"rpc-1\",\"ok\":true,\"payload\":%s}", values[i]);
        cJSON *root = NULL;
        assert(esp_openclaw_node_parse_response(json, strlen(json), &root) == ESP_OPENCLAW_RESPONSE_PARSED);
        assert(cJSON_IsRaw(cJSON_GetObjectItemCaseSensitive(root, "payload")));
        deliver(root, values[i]);
        cJSON_Delete(root);
        assert(strcmp(retained, values[i]) == 0);
        cJSON_free(retained); retained = NULL;
        assert(allocated == 0);
    }
    const char *missing = "{\"type\":\"res\",\"id\":\"rpc-1\",\"ok\":true}";
    cJSON *root = NULL;
    assert(esp_openclaw_node_parse_response(missing, strlen(missing), &root) == ESP_OPENCLAW_RESPONSE_PARSED);
    deliver(root, NULL); cJSON_Delete(root); assert(allocated == 0);
    const char *denied = "{\"type\":\"res\",\"id\":\"rpc-1\",\"ok\":false,\"error\":{\"code\":\"DENIED\",\"message\":\"No access\"}}";
    assert(esp_openclaw_node_parse_response(denied, strlen(denied), &root) == ESP_OPENCLAW_RESPONSE_PARSED);
    expected_ok = false; expected_error = "DENIED";
    deliver(root, NULL); cJSON_Delete(root); assert(allocated == 0);
    expected_ok = true; expected_error = NULL;

    const char *escaped = "{\"ty\\u0070e\":\"r\\u0065s\",\"type\":\"event\",\"pay\\u006coad\":{\"first\":true},\"payload\":false}";
    assert(esp_openclaw_node_parse_response(escaped, strlen(escaped), &root) == ESP_OPENCLAW_RESPONSE_PARSED);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(root, "payload")->valuestring, "{\"first\":true}") == 0);
    cJSON_Delete(root); assert(allocated == 0);
    const char *event = "{\"type\":\"event\",\"payload\":{\"x\":true}}";
    assert(esp_openclaw_node_parse_response(event, strlen(event), &root) == ESP_OPENCLAW_RESPONSE_NOT_APPLICABLE && root == NULL);
    const char *invalid[] = {
        "{\"type\":\"res\",\"payload\":[1,]}", "{\"type\":\"res\"} trailing",
        "{\"type\":\"res\",\"payload\":\"\\u0000\"}", "{\"type\":\"res\",\"payload\":\"\xc0\xaf\"}",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(esp_openclaw_node_parse_response(invalid[i], strlen(invalid[i]), &root) == ESP_OPENCLAW_RESPONSE_INVALID && root == NULL);
    char deep[180] = "{\"type\":\"res\",\"payload\":";
    size_t offset = strlen(deep);
    for (unsigned i = 0; i < 65; ++i) deep[offset++] = '[';
    deep[offset++] = '0';
    for (unsigned i = 0; i < 65; ++i) deep[offset++] = ']';
    deep[offset++] = '}'; deep[offset] = '\0';
    assert(esp_openclaw_node_parse_response(deep, offset, &root) == ESP_OPENCLAW_RESPONSE_INVALID && root == NULL);
    strcpy(deep, "{\"payload\":"); offset = strlen(deep);
    for (unsigned i = 0; i < 65; ++i) deep[offset++] = '[';
    deep[offset++] = '0';
    for (unsigned i = 0; i < 65; ++i) deep[offset++] = ']';
    strcpy(deep + offset, ",\"type\":\"res\"}");
    assert(esp_openclaw_node_parse_response(deep, strlen(deep), &root) == ESP_OPENCLAW_RESPONSE_INVALID && root == NULL);

    char json[20000], padding[1101];
    memset(padding, 'x', sizeof(padding) - 1); padding[sizeof(padding) - 1] = '\0';
    size_t used = (size_t)snprintf(json, sizeof(json), "{\"type\":\"res\",\"id\":\"rpc-1\",\"ok\":true,\"payload\":{\"nodes\":[");
    for (unsigned i = 0; i < 10; ++i) {
        int size = snprintf(json + used, sizeof(json) - used,
            "%s{\"nodeId\":\"node-%u\",\"displayName\":\"Board %u\",\"connected\":true,\"paired\":true,"
            "\"caps\":[\"device\",\"wifi\",\"display\"],\"commands\":[\"device.info\",\"device.status\",\"wifi.status\",\"display.show\",\"display.status\"],\"ignored\":\"%s\"}",
            i ? "," : "", i, i, padding);
        assert(size > 0 && (size_t)size < sizeof(json) - used); used += (size_t)size;
    }
    assert(used + 4 < sizeof(json)); strcpy(json + used, "]}}"); used += 3;
    budget = 40000;
    root = cJSON_Parse(json); assert(root != NULL);
    char *printed = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(root, "payload"));
    assert(printed == NULL); cJSON_Delete(root); assert(allocated == 0);
    peak = 0;
    assert(esp_openclaw_node_parse_response(json, used, &root) == ESP_OPENCLAW_RESPONSE_PARSED);
    deliver(root, cJSON_GetObjectItemCaseSensitive(root, "payload")->valuestring);
    cJSON_Delete(root);
    root = cJSON_Parse(retained); assert(root != NULL);
    cJSON *nodes = cJSON_GetObjectItemCaseSensitive(root, "nodes");
    assert(cJSON_GetArraySize(nodes) == 10);
    for (unsigned i = 0; i < 10; ++i) {
        cJSON *row = cJSON_GetArrayItem(nodes, (int)i);
        char id[20]; snprintf(id, sizeof(id), "node-%u", i);
        assert(strcmp(cJSON_GetObjectItemCaseSensitive(row, "nodeId")->valuestring, id) == 0);
        assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(row, "caps")) == 3);
        assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(row, "commands")) == 5);
    }
    cJSON_Delete(root); cJSON_free(retained); retained = NULL; assert(allocated == 0);
    printf("RPC response: %zu-byte frame and callback copy fit a %zu-byte heap (peak %zu)\n", used, budget, peak);

    const char *small = "{\"type\":\"res\",\"id\":\"rpc-1\",\"ok\":true,\"payload\":{\"value\":true}}";
    for (budget = 0; budget < 1024; budget += 16) {
        esp_openclaw_response_parse_t status = esp_openclaw_node_parse_response(small, strlen(small), &root);
        assert(status == ESP_OPENCLAW_RESPONSE_PARSED || status == ESP_OPENCLAW_RESPONSE_NO_MEMORY);
        if (status == ESP_OPENCLAW_RESPONSE_NO_MEMORY && root != NULL) {
            assert(cJSON_GetObjectItemCaseSensitive(root, "payload") == NULL);
            deliver(root, NULL);
        }
        cJSON_Delete(root); assert(allocated == 0);
    }
    cJSON_InitHooks(NULL);
    puts("RPC response: raw values, ownership, callback admission and allocation failures passed");
    return 0;
}
