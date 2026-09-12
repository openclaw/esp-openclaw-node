#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

typedef int esp_err_t;
typedef unsigned TickType_t;
typedef void *esp_websocket_client_handle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_OPENCLAW_NODE_TAG "esp_openclaw_node"
#define ESP_OPENCLAW_NODE_MAX_PENDING_REQUESTS 8
#define ESP_OPENCLAW_NODE_MAX_SESSION_KEY_LEN 256
#define ESP_OPENCLAW_NODE_INTERNAL_READY 1
#define pdMS_TO_TICKS(value) ((TickType_t)(value))

typedef struct node *esp_openclaw_node_handle_t;
typedef struct {
    bool ok;
    const char *payload_json;
    const char *error_code;
    const char *error_message;
} esp_openclaw_node_gateway_result_t;
typedef void (*esp_openclaw_node_gateway_request_cb_t)(
    esp_openclaw_node_handle_t, const esp_openclaw_node_gateway_result_t *, void *);
typedef struct {
    bool in_use;
    char request_id[40];
    esp_openclaw_node_gateway_request_cb_t callback;
    void *user_ctx;
} esp_openclaw_node_pending_request_t;
typedef struct {
    int (*send_text)(esp_websocket_client_handle_t, const char *, int, TickType_t);
} websocket_ops_t;
struct node {
    int state;
    void *ws;
    const websocket_ops_t *websocket_client_ops;
    esp_openclaw_node_pending_request_t pending_requests[ESP_OPENCLAW_NODE_MAX_PENDING_REQUESTS];
    uint64_t next_request_id;
};
typedef struct {
    const char *session_key;
} esp_openclaw_node_command_invocation_t;

static const char *payload_canary = "{\"value\":\"payload_canary_secret\"}";
static char *handler_payload;
static char *serialized;
static char *sent;
static unsigned payload_frees, serialized_frees, envelope_frees;
static unsigned sends, warnings, callbacks, locks;
static int send_mode, handler_rc;
static bool serialize_failure, missing_payload, block_send, send_entered, send_release;
static atomic_int_fast64_t clock_us = 1000000;
static pthread_mutex_t send_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t send_condition = PTHREAD_COND_INITIALIZER;

typedef struct {
    unsigned command, stage, payload_present, alloc_failed;
    int rc, requested, returned;
    uint64_t bytes, elapsed_ms;
    unsigned payload_frees, serialized_frees, envelope_frees;
} record_t;
static record_t records[9];
static unsigned record_count;

static int64_t esp_timer_get_time(void)
{
    return atomic_fetch_add(&clock_us, 1000);
}

static void test_log(const char *tag, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
static void test_log(const char *tag, const char *format, ...)
{
    assert(locks == 0);
    assert(strcmp(tag, ESP_OPENCLAW_NODE_TAG) == 0);
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    assert(length > 0 && (size_t)length < sizeof(line));
    const char *canaries[] = {
        "request_canary", "node_canary", "session_canary", "params_canary",
        "payload_canary_secret", "error_canary", "message_canary",
    };
    for (size_t i = 0; i < sizeof(canaries) / sizeof(canaries[0]); ++i) {
        assert(strstr(line, canaries[i]) == NULL);
    }
    if (strncmp(line, "invoke_reply_diag ", 18) != 0) {
        assert(strcmp(line, "failed sending invoke result") == 0);
        warnings++;
        return;
    }
    assert(record_count < 9);
    record_t *record = &records[record_count++];
    int consumed = 0;
    assert(sscanf(
        line,
        "invoke_reply_diag command=%u stage=%u rc=%d payload_present=%u "
        "alloc_failed=%u bytes=%" SCNu64 " requested=%d returned=%d elapsed_ms=%" SCNu64 "%n",
        &record->command, &record->stage, &record->rc, &record->payload_present,
        &record->alloc_failed, &record->bytes, &record->requested, &record->returned,
        &record->elapsed_ms, &consumed) == 9);
    assert(consumed == length);
    record->payload_frees = payload_frees;
    record->serialized_frees = serialized_frees;
    record->envelope_frees = envelope_frees;
}
#define ESP_LOGI(tag, ...) do { if (!TEST_LOG_QUIET) test_log(tag, __VA_ARGS__); } while (0)
#define ESP_LOGW(tag, ...) test_log(tag, __VA_ARGS__)

static void esp_openclaw_node_lock_state(esp_openclaw_node_handle_t node)
{
    (void)node;
    assert(locks++ == 0);
}

static void esp_openclaw_node_unlock_state(esp_openclaw_node_handle_t node)
{
    (void)node;
    assert(locks-- == 1);
}

static esp_err_t esp_openclaw_node_dispatch_command(
    esp_openclaw_node_handle_t node, const char *command, const char *params,
    size_t length, const esp_openclaw_node_command_invocation_t *invocation,
    char **payload, const char **error_code, const char **error_message)
{
    (void)node;
    assert(command != NULL);
    assert(length == strlen(params) && strstr(params, "params_canary") != NULL);
    assert(strcmp(invocation->session_key, "session_canary") == 0);
    handler_payload = missing_payload ? NULL : strdup(payload_canary);
    assert(missing_payload || handler_payload != NULL);
    *payload = handler_payload;
    *error_code = "error_canary";
    *error_message = "message_canary";
    atomic_fetch_add(&clock_us, 17000);
    return handler_rc;
}

static void tracked_free(void *pointer)
{
    if (pointer != NULL && pointer == handler_payload) assert(++payload_frees == 1);
    if (pointer != NULL && pointer == serialized) assert(++serialized_frees == 1);
    free(pointer);
}

static void tracked_delete(cJSON *item)
{
    if (cJSON_IsObject(item) && cJSON_GetObjectItemCaseSensitive(item, "method") != NULL) {
        envelope_frees++;
    }
    cJSON_Delete(item);
}

static void *fail_allocation(size_t size)
{
    (void)size;
    return NULL;
}

static char *tracked_print(const cJSON *item)
{
    if (serialize_failure) {
        cJSON_Hooks hooks = {.malloc_fn = fail_allocation, .free_fn = free};
        cJSON_InitHooks(&hooks);
    }
    serialized = cJSON_PrintUnformatted(item);
    cJSON_InitHooks(NULL);
    return serialized;
}

#define free tracked_free
#define cJSON_Delete tracked_delete
#define cJSON_PrintUnformatted tracked_print
#include "reply_under_test.inc"
#undef cJSON_PrintUnformatted
#undef cJSON_Delete
#undef free

static int send_text(void *client, const char *data, int length, TickType_t timeout)
{
    assert(client != NULL && timeout == 5000 && locks == 0);
    assert(length > 1 && (size_t)length == strlen(data));
    sent = strdup(data);
    assert(sent != NULL);
    sends++;
    if (block_send) {
        assert(pthread_mutex_lock(&send_mutex) == 0);
        send_entered = true;
        assert(pthread_cond_broadcast(&send_condition) == 0);
        while (!send_release) assert(pthread_cond_wait(&send_condition, &send_mutex) == 0);
        assert(pthread_mutex_unlock(&send_mutex) == 0);
    }
    atomic_fetch_add(&clock_us, 23000);
    if (send_mode == 1) return -7;
    if (send_mode == 2) return 0;
    if (send_mode == 3) return length - 1;
    return length;
}

static const websocket_ops_t ops = {.send_text = send_text};
static struct node node = {
    .state = ESP_OPENCLAW_NODE_INTERNAL_READY,
    .ws = &node,
    .websocket_client_ops = &ops,
};

static void gateway_callback(
    esp_openclaw_node_handle_t handle, const esp_openclaw_node_gateway_result_t *result,
    void *context)
{
    assert(handle == &node && context == &node && locks == 0);
    assert(!result->ok && strcmp(result->error_code, "TRANSPORT_ERROR") == 0);
    assert(!node.pending_requests[0].in_use);
    callbacks++;
}

static cJSON *request(const char *command)
{
    cJSON *root = cJSON_CreateObject();
    assert(root != NULL);
    assert(cJSON_AddStringToObject(root, "id", "request_canary") != NULL);
    assert(cJSON_AddStringToObject(root, "nodeId", "node_canary") != NULL);
    assert(cJSON_AddStringToObject(root, "command", command) != NULL);
    assert(cJSON_AddStringToObject(root, "paramsJSON", "{\"value\":\"params_canary\"}") != NULL);
    assert(cJSON_AddStringToObject(root, "sessionKey", "session_canary") != NULL);
    return root;
}

static void *invoke_thread(void *payload)
{
    handle_invoke_request(&node, payload);
    return NULL;
}

static void check_envelope(bool gateway)
{
    cJSON *root = cJSON_Parse(sent);
    assert(cJSON_IsObject(root));
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(root, "type")->valuestring, "req") == 0);
    cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    assert(cJSON_IsObject(params));
    const char *method = cJSON_GetObjectItemCaseSensitive(root, "method")->valuestring;
    if (gateway) {
        assert(strcmp(method, "fixture.request") == 0);
        assert(strcmp(cJSON_GetObjectItemCaseSensitive(params, "value")->valuestring,
                      "params_canary") == 0);
    } else {
        assert(strcmp(method, "node.invoke.result") == 0);
        assert(strcmp(cJSON_GetObjectItemCaseSensitive(params, "id")->valuestring,
                      "request_canary") == 0);
        assert(strcmp(cJSON_GetObjectItemCaseSensitive(params, "nodeId")->valuestring,
                      "node_canary") == 0);
        assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params, "ok")) == (handler_rc == ESP_OK));
        cJSON *payload = cJSON_GetObjectItemCaseSensitive(params, "payloadJSON");
        if (handler_rc != ESP_OK) {
            cJSON *error = cJSON_GetObjectItemCaseSensitive(params, "error");
            assert(payload == NULL && cJSON_IsObject(error));
            assert(strcmp(cJSON_GetObjectItemCaseSensitive(error, "code")->valuestring,
                          "error_canary") == 0);
            assert(strcmp(cJSON_GetObjectItemCaseSensitive(error, "message")->valuestring,
                          "message_canary") == 0);
        } else if (missing_payload) {
            assert(payload == NULL);
        } else {
            assert(cJSON_IsString(payload) && strcmp(payload->valuestring, payload_canary) == 0);
        }
    }
    cJSON_Delete(root);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    bool gateway = strncmp(argv[1], "gateway-", 8) == 0;
    const char *scenario = gateway ? argv[1] + 8 : argv[1];
    send_mode = strcmp(scenario, "negative") == 0 ? 1
        : strcmp(scenario, "zero") == 0 ? 2 : strcmp(scenario, "short") == 0 ? 3 : 0;
    serialize_failure = strcmp(scenario, "serialize-failure") == 0;
    handler_rc = strcmp(scenario, "handler-error") == 0 ? ESP_FAIL : ESP_OK;
    missing_payload = strcmp(scenario, "no-payload") == 0;
    block_send = strcmp(scenario, "blocked-send") == 0;
    const char *command = strcmp(scenario, "device-info") == 0 ? "device.info"
        : strcmp(scenario, "other-command") == 0 ? "camera.snap.extra" : "camera.snap";
    if (gateway) {
        esp_openclaw_node_send_gateway_request(
            &node, "fixture.request", "{\"value\":\"params_canary\"}", gateway_callback, &node);
        assert(callbacks == (send_mode != 0 ? 1U : 0U));
        assert(node.pending_requests[0].in_use == (send_mode == 0));
        assert(record_count == 0 && warnings == 0 && payload_frees == 0);
    } else {
        cJSON *payload = request(command);
        if (block_send) {
            pthread_t thread;
            assert(pthread_create(&thread, NULL, invoke_thread, payload) == 0);
            assert(pthread_mutex_lock(&send_mutex) == 0);
            while (!send_entered) assert(pthread_cond_wait(&send_condition, &send_mutex) == 0);
            assert(record_count == 7 && records[6].stage == 7);
            assert(payload_frees == 0 && serialized_frees == 0 && envelope_frees == 0);
            send_release = true;
            assert(pthread_cond_broadcast(&send_condition) == 0);
            assert(pthread_mutex_unlock(&send_mutex) == 0);
            assert(pthread_join(thread, NULL) == 0);
        } else {
            handle_invoke_request(&node, payload);
        }
        cJSON_Delete(payload);
        assert(warnings == (send_mode != 0 || serialize_failure ? 1U : 0U));
        assert(payload_frees == (missing_payload ? 0U : 1U));
        bool selected = strcmp(command, "camera.snap.extra") != 0 && !TEST_LOG_QUIET;
        assert(record_count == (selected ? (serialize_failure ? 7U : 9U) : 0U));
        for (unsigned i = 0; i < record_count; ++i) {
            record_t *record = &records[i];
            unsigned stage = serialize_failure && i == 6 ? 9 : i + 1;
            assert(record->stage == stage);
            assert(record->command == (strcmp(command, "device.info") == 0 ? 2U : 1U));
            if (stage == 1 || stage == 3 || stage == 5 || stage == 7) {
                assert(record->elapsed_ms == 0);
            } else {
                assert(record->elapsed_ms > 0);
            }
            if (stage >= 2) {
                assert(record->rc == handler_rc);
                assert(record->payload_present == !missing_payload);
            }
            if (stage >= 6) {
                assert(record->alloc_failed == serialize_failure);
                assert(record->bytes == (serialize_failure ? 0 : strlen(sent)));
            }
            if (stage == 8) {
                assert(record->requested == (int)strlen(sent));
                int expected = send_mode == 1 ? -7 : send_mode == 2 ? 0
                    : send_mode == 3 ? (int)strlen(sent) - 1 : (int)strlen(sent);
                assert(record->returned == expected);
                assert(record->payload_frees == 0 && record->serialized_frees == 0
                       && record->envelope_frees == 0);
            }
            if (stage == 9) {
                assert(record->payload_frees == (missing_payload ? 0U : 1U));
                assert(record->serialized_frees == (serialize_failure ? 0U : 1U));
                assert(record->envelope_frees == 1);
            }
        }
    }
    assert(envelope_frees == 1);
    assert(serialized_frees == (serialize_failure ? 0U : 1U));
    assert(sends == (serialize_failure ? 0U : 1U));
    if (!serialize_failure) check_envelope(gateway);
    free(sent);
    assert(pthread_cond_destroy(&send_condition) == 0);
    assert(pthread_mutex_destroy(&send_mutex) == 0);
    return 0;
}
