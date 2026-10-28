/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <string.h>
#include "unity.h"

#define esp_openclaw_node_gateway_request_on_connection test_chat_request
#define esp_openclaw_node_get_connection_id test_chat_connection
#define esp_timer_get_time test_chat_time
#include "../src/esp_openclaw_chat.c"
#undef esp_timer_get_time
#undef esp_openclaw_node_get_connection_id
#undef esp_openclaw_node_gateway_request_on_connection

static struct {
    esp_openclaw_node_gateway_request_cb_t callback;
    void *ctx;
    uint32_t connection;
    unsigned requests;
    int64_t now;
    esp_err_t submit_error;
    char method[32], params[16384];
} transport;
static char node_storage, other_node;
#define NODE ((esp_openclaw_node_handle_t)&node_storage)
#define OTHER ((esp_openclaw_node_handle_t)&other_node)

uint32_t test_chat_connection(esp_openclaw_node_handle_t node)
{
    TEST_ASSERT_EQUAL_PTR(NODE, node);
    return transport.connection;
}
int64_t test_chat_time(void) { return transport.now; }
esp_err_t test_chat_request(esp_openclaw_node_handle_t node, uint32_t connection,
    const char *method, const char *params, esp_openclaw_node_gateway_request_cb_t callback, void *ctx)
{
    TEST_ASSERT_EQUAL_PTR(NODE, node);
    if (connection != transport.connection) return ESP_ERR_INVALID_STATE;
    if (transport.submit_error != ESP_OK) return transport.submit_error;
    TEST_ASSERT_NULL(transport.callback);
    TEST_ASSERT_LESS_THAN(sizeof(transport.params), strlen(params));
    strcpy(transport.method, method);
    strcpy(transport.params, params);
    transport.callback = callback;
    transport.ctx = ctx;
    ++transport.requests;
    return ESP_OK;
}
static void reply(bool ok, const char *json)
{
    esp_openclaw_node_gateway_request_cb_t callback = transport.callback;
    void *ctx = transport.ctx;
    TEST_ASSERT_NOT_NULL(callback);
    transport.callback = NULL;
    transport.ctx = NULL;
    const esp_openclaw_node_gateway_result_t result = {.ok = ok, .payload_json = json};
    callback(NODE, &result, ctx);
}
static const char history[] = "{\"sessionKey\":\"main\",\"sessionId\":\"conversation-1\","
    "\"sessionInfo\":{\"key\":\"agent:fox:main\",\"agentId\":\"fox\","
    "\"sessionId\":\"conversation-1\",\"activeLeafEntryId\":\"leaf-1\"}}";
static const char started[] = "{\"runId\":\"intent-1\",\"status\":\"started\"}";
static esp_openclaw_chat_handle_t prepare(void)
{
    memset(&transport, 0, sizeof(transport));
    transport.connection = 7;
    esp_openclaw_chat_handle_t h = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_chat_prepare(NODE, "main", "fox", &h));
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL_STRING("chat.history", transport.method);
    return h;
}
static esp_openclaw_chat_snapshot_t snapshot(esp_openclaw_chat_handle_t h)
{
    esp_openclaw_chat_snapshot_t value;
    esp_openclaw_chat_copy(h, &value);
    return value;
}
static esp_openclaw_chat_handle_t ready(void)
{
    esp_openclaw_chat_handle_t h = prepare();
    reply(true, history);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_READY, snapshot(h).state);
    return h;
}
static esp_openclaw_chat_handle_t sending(void)
{
    esp_openclaw_chat_handle_t h = ready();
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_chat_send(h, "hello", "intent-1"));
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_SENDING, snapshot(h).state);
    return h;
}
static void event(esp_openclaw_chat_handle_t h, unsigned seq, const char *state, const char *extra)
{
    char json[512];
    snprintf(json, sizeof(json), "{\"runId\":\"intent-1\",\"sessionKey\":\"agent:fox:main\","
        "\"agentId\":\"fox\",\"seq\":%u,\"state\":\"%s\"%s}", seq, state, extra);
    esp_openclaw_chat_event(h, NODE, "chat", json);
}
static void dispose(esp_openclaw_chat_handle_t h)
{
    esp_openclaw_chat_destroy(h);
    if (transport.callback != NULL) reply(true, started);
}

TEST_CASE("chat resolves canonical destination without sending a draft", "[chat]")
{
    esp_openclaw_chat_handle_t h = prepare();
    TEST_ASSERT_EQUAL(1, transport.requests);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, esp_openclaw_chat_send(h, "hello", "intent-1"));
    cJSON *params = cJSON_Parse(transport.params);
    TEST_ASSERT_EQUAL_STRING("main", cJSON_GetObjectItemCaseSensitive(params, "sessionKey")->valuestring);
    TEST_ASSERT_EQUAL(1, cJSON_GetObjectItemCaseSensitive(params, "limit")->valueint);
    TEST_ASSERT_EQUAL(4096, cJSON_GetObjectItemCaseSensitive(params, "maxBytes")->valueint);
    cJSON_Delete(params);
    reply(true, history);
    TEST_ASSERT_EQUAL_STRING("agent:fox:main", snapshot(h).session_key);
    TEST_ASSERT_EQUAL_STRING("fox", snapshot(h).agent_id);
    TEST_ASSERT_EQUAL_STRING("conversation-1", snapshot(h).session_id);
    TEST_ASSERT_EQUAL(1, transport.requests);
    dispose(h);
}

TEST_CASE("chat sends explicit escaped text once with captured routing and branch guard", "[chat]")
{
    esp_openclaw_chat_handle_t h = ready();
    const char *text = "hello \"fox\"\n\\ 🦊";
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_chat_send(h, text, "intent-1"));
    TEST_ASSERT_EQUAL_STRING("chat.send", transport.method);
    cJSON *params = cJSON_Parse(transport.params);
    TEST_ASSERT_EQUAL_STRING(text, cJSON_GetObjectItemCaseSensitive(params, "message")->valuestring);
    TEST_ASSERT_EQUAL_STRING("agent:fox:main", cJSON_GetObjectItemCaseSensitive(params, "sessionKey")->valuestring);
    TEST_ASSERT_EQUAL_STRING("fox", cJSON_GetObjectItemCaseSensitive(params, "agentId")->valuestring);
    TEST_ASSERT_EQUAL_STRING("conversation-1", cJSON_GetObjectItemCaseSensitive(params, "sessionId")->valuestring);
    TEST_ASSERT_EQUAL_STRING("leaf-1", cJSON_GetObjectItemCaseSensitive(params, "expectedLeafEntryId")->valuestring);
    TEST_ASSERT_EQUAL_STRING("intent-1", cJSON_GetObjectItemCaseSensitive(params, "idempotencyKey")->valuestring);
    TEST_ASSERT_EQUAL_STRING("followup", cJSON_GetObjectItemCaseSensitive(params, "queueMode")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(params, "deliver")));
    cJSON_Delete(params);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, esp_openclaw_chat_send(h, "double tap", "intent-2"));
    reply(true, started);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_RUNNING, snapshot(h).state);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, esp_openclaw_chat_send(h, "retry", "intent-2"));
    TEST_ASSERT_EQUAL(2, transport.requests);
    dispose(h);
}

TEST_CASE("chat preserves an authoritative empty branch as explicit null", "[chat]")
{
    esp_openclaw_chat_handle_t h = prepare();
    reply(true, "{\"sessionId\":\"empty\",\"sessionInfo\":{\"key\":\"agent:fox:main\","
        "\"agentId\":\"fox\",\"sessionId\":\"empty\",\"activeLeafEntryId\":null}}");
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_chat_send(h, "first message", "intent-1"));
    cJSON *params = cJSON_Parse(transport.params);
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(params, "expectedLeafEntryId")));
    cJSON_Delete(params);
    dispose(h);
}

TEST_CASE("chat rejects incomplete ambiguous or foreign conversation metadata", "[chat]")
{
    const char *invalid[] = {
        "{}",
        "{\"sessionInfo\":{\"key\":\"agent:fox:main\",\"agentId\":\"fox\",\"activeLeafEntryId\":null}}",
        "{\"sessionId\":\"s\",\"sessionInfo\":{\"key\":\"agent:fox:main\",\"agentId\":\"fox\",\"sessionId\":\"s\"}}",
        "{\"sessionId\":\"s\",\"sessionInfo\":{\"key\":\"agent:fox:main\",\"agentId\":\"other\",\"sessionId\":\"s\",\"activeLeafEntryId\":null}}",
        "{\"sessionId\":\"s\",\"sessionInfo\":{\"key\":\"agent:fox:main\",\"agentId\":\"fox\",\"sessionId\":\"replacement\",\"activeLeafEntryId\":null}}",
        "{\"sessionId\":\"s\",\"sessionInfo\":{\"key\":\"main\\u0000hidden\",\"agentId\":\"fox\",\"sessionId\":\"s\",\"activeLeafEntryId\":null}}",
        "{\"sessionId\":\"s\",\"sessionId\":\"other\",\"sessionInfo\":{\"key\":\"agent:fox:main\",\"agentId\":\"fox\",\"sessionId\":\"s\",\"activeLeafEntryId\":null}}",
        "{\"sessionId\":\"s\",\"sessionInfo\":{\"key\":\"agent:fox:main\",\"agentId\":\"fox\",\"sessionId\":\"s\",\"activeLeafEntryId\":3}}",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        esp_openclaw_chat_handle_t h = prepare();
        reply(true, invalid[i]);
        TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_FAILED, snapshot(h).state);
        TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_ERROR_FORMAT, snapshot(h).error);
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, esp_openclaw_chat_send(h, "hello", "intent-1"));
        dispose(h);
    }
}

TEST_CASE("chat bounds routing identities instead of truncating them", "[chat]")
{
    char key[ESP_OPENCLAW_CHAT_KEY_BYTES + 1];
    memset(key, 'k', sizeof(key) - 1); key[sizeof(key) - 1] = '\0';
    esp_openclaw_chat_handle_t h = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esp_openclaw_chat_prepare(NODE, key, "fox", &h));
    TEST_ASSERT_NULL(h);
    h = prepare();
    cJSON *root = cJSON_Parse(history);
    cJSON_ReplaceItemInObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "sessionInfo"), "key", cJSON_CreateString(key));
    char *json = cJSON_PrintUnformatted(root);
    reply(true, json);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_FAILED, snapshot(h).state);
    free(json); cJSON_Delete(root);
    dispose(h);
}

TEST_CASE("chat rejects empty oversized and broken UTF8 drafts without consuming Send", "[chat]")
{
    esp_openclaw_chat_handle_t h = ready();
    char text[ESP_OPENCLAW_CHAT_TEXT_BYTES + 1];
    memset(text, 'x', sizeof(text) - 1); text[sizeof(text) - 1] = '\0';
    const char *invalid[] = {"", " \r\n\t", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", text};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esp_openclaw_chat_send(h, invalid[i], "intent-1"));
        TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_READY, snapshot(h).state);
    }
    TEST_ASSERT_EQUAL(1, transport.requests);
    text[ESP_OPENCLAW_CHAT_TEXT_BYTES - 1] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_chat_send(h, text, "intent-1"));
    dispose(h);
}

TEST_CASE("chat queue rejection sends nothing and allows an explicit retry", "[chat]")
{
    esp_openclaw_chat_handle_t h = ready();
    transport.submit_error = ESP_ERR_NO_MEM;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, esp_openclaw_chat_send(h, "hello", "intent-1"));
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_READY, snapshot(h).state);
    TEST_ASSERT_EQUAL_STRING("", snapshot(h).run_id);
    TEST_ASSERT_EQUAL(1, transport.requests);
    transport.submit_error = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_chat_send(h, "hello", "intent-1"));
    dispose(h);
}

TEST_CASE("chat accepts sequence zero and ignores duplicate and older events", "[chat]")
{
    esp_openclaw_chat_handle_t h = sending();
    event(h, 0, "status", "");
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_RUNNING, snapshot(h).state);
    event(h, 0, "error", "");
    event(h, 2, "delta", ",\"deltaText\":\"hello\"");
    event(h, 1, "error", "");
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_RUNNING, snapshot(h).state);
    reply(true, started);
    event(h, 3, "final", "");
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_COMPLETE, snapshot(h).state);
    event(h, 4, "error", "");
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_COMPLETE, snapshot(h).state);
    dispose(h);
}

TEST_CASE("chat never confuses acknowledgement with terminal evidence", "[chat]")
{
    esp_openclaw_chat_handle_t h = sending();
    reply(true, "{\"runId\":\"intent-1\",\"status\":\"in_flight\"}");
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_RUNNING, snapshot(h).state);
    event(h, 0, "final", ",\"yielded\":true");
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_HANDED_OFF, snapshot(h).state);
    dispose(h);
}

TEST_CASE("chat terminal events before acknowledgements cannot be overwritten", "[chat]")
{
    const char *events[] = {"final", "aborted", "error"};
    esp_openclaw_chat_state_t states[] = {ESP_OPENCLAW_CHAT_COMPLETE, ESP_OPENCLAW_CHAT_ABORTED, ESP_OPENCLAW_CHAT_FAILED};
    for (size_t i = 0; i < 3; ++i) {
        esp_openclaw_chat_handle_t h = sending();
        event(h, 0, events[i], "");
        reply(true, started);
        TEST_ASSERT_EQUAL(states[i], snapshot(h).state);
        dispose(h);
    }
}

TEST_CASE("chat uses cached run outcomes without fabricating reply text", "[chat]")
{
    const char *statuses[] = {"ok", "aborted", "error", "timeout"};
    esp_openclaw_chat_state_t states[] = {ESP_OPENCLAW_CHAT_COMPLETE, ESP_OPENCLAW_CHAT_ABORTED, ESP_OPENCLAW_CHAT_FAILED, ESP_OPENCLAW_CHAT_FAILED};
    for (size_t i = 0; i < 4; ++i) {
        esp_openclaw_chat_handle_t h = sending();
        char json[100];
        snprintf(json, sizeof(json), "{\"runId\":\"intent-1\",\"status\":\"%s\"}", statuses[i]);
        reply(i == 0, json);
        TEST_ASSERT_EQUAL(states[i], snapshot(h).state);
        dispose(h);
    }
}

TEST_CASE("chat ignores foreign malformed and duplicate-field events", "[chat]")
{
    const char *invalid[] = {
        "{\"runId\":\"other\",\"sessionKey\":\"agent:fox:main\",\"seq\":0,\"state\":\"final\"}",
        "{\"runId\":\"intent-1\",\"sessionKey\":\"other\",\"seq\":0,\"state\":\"final\"}",
        "{\"runId\":\"intent-1\",\"sessionKey\":\"agent:fox:main\",\"agentId\":\"other\",\"seq\":0,\"state\":\"final\"}",
        "{\"runId\":\"intent-1\",\"sessionKey\":\"agent:fox:main\",\"seq\":-1,\"state\":\"final\"}",
        "{\"runId\":\"intent-1\",\"sessionKey\":\"agent:fox:main\",\"seq\":0.5,\"state\":\"final\"}",
        "{\"runId\":\"intent-1\",\"sessionKey\":\"agent:fox:main\",\"seq\":4294967296,\"state\":\"final\"}",
        "{\"runId\":\"intent-1\",\"sessionKey\":\"agent:fox:main\",\"seq\":0,\"state\":\"unknown\"}",
        "{\"runId\":\"intent-1\",\"sessionKey\":\"agent:fox:main\",\"seq\":0,\"agentId\":\"fox\",\"agentId\":\"other\",\"state\":\"final\"}",
        "{\"runId\":\"intent-1\\u0000tail\",\"sessionKey\":\"agent:fox:main\",\"seq\":0,\"state\":\"final\"}",
    };
    esp_openclaw_chat_handle_t h = sending();
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        esp_openclaw_chat_event(h, NODE, "chat", invalid[i]);
        TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_SENDING, snapshot(h).state);
    }
    const char *valid = "{\"runId\":\"intent-1\",\"sessionKey\":\"agent:fox:main\",\"seq\":0,\"state\":\"final\"}";
    esp_openclaw_chat_event(h, OTHER, "chat", valid);
    esp_openclaw_chat_event(h, NODE, "talk.event", valid);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_SENDING, snapshot(h).state);
    esp_openclaw_chat_event(h, NODE, "chat", valid);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_COMPLETE, snapshot(h).state);
    dispose(h);
}

TEST_CASE("chat disconnect after submission remains unknown without replay", "[chat]")
{
    esp_openclaw_chat_handle_t h = sending();
    transport.connection = 8;
    esp_openclaw_chat_poll(h);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_OUTCOME_UNKNOWN, snapshot(h).state);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_ERROR_CONNECTION, snapshot(h).error);
    event(h, 0, "final", "");
    reply(true, started);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_OUTCOME_UNKNOWN, snapshot(h).state);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, esp_openclaw_chat_send(h, "retry", "intent-2"));
    TEST_ASSERT_EQUAL(2, transport.requests);
    dispose(h);
}

TEST_CASE("chat timeout can reconcile a late terminal event on the same connection", "[chat]")
{
    esp_openclaw_chat_handle_t h = sending();
    transport.now = 30000000;
    esp_openclaw_chat_poll(h);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_OUTCOME_UNKNOWN, snapshot(h).state);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_ERROR_TIMEOUT, snapshot(h).error);
    event(h, 0, "final", "");
    reply(false, NULL);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_COMPLETE, snapshot(h).state);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_ERROR_NONE, snapshot(h).error);
    dispose(h);
}

TEST_CASE("chat running idle timeout cannot silently retry an accepted run", "[chat]")
{
    esp_openclaw_chat_handle_t h = sending();
    reply(true, started);
    transport.now = 299999999;
    esp_openclaw_chat_poll(h);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_RUNNING, snapshot(h).state);
    transport.now = 300000000;
    esp_openclaw_chat_poll(h);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_OUTCOME_UNKNOWN, snapshot(h).state);
    TEST_ASSERT_EQUAL(2, transport.requests);
    dispose(h);
}

TEST_CASE("chat failed or malformed send replies never manufacture success", "[chat]")
{
    const char *invalid[] = {NULL, "{}", "{\"runId\":\"wrong\",\"status\":\"started\"}",
        "{\"runId\":\"intent-1\",\"status\":\"invented\"}"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        esp_openclaw_chat_handle_t h = sending();
        reply(i != 0, invalid[i]);
        TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_OUTCOME_UNKNOWN, snapshot(h).state);
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, esp_openclaw_chat_send(h, "retry", "intent-2"));
        dispose(h);
    }
}

TEST_CASE("chat preparing connection loss timeout and Gateway denial send nothing", "[chat]")
{
    for (int i = 0; i < 3; ++i) {
        esp_openclaw_chat_handle_t h = prepare();
        if (i == 0) transport.connection = 0;
        if (i == 1) transport.now = 30000000;
        if (i == 2) reply(false, NULL);
        esp_openclaw_chat_poll(h);
        TEST_ASSERT_EQUAL(ESP_OPENCLAW_CHAT_FAILED, snapshot(h).state);
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, esp_openclaw_chat_send(h, "hello", "intent-1"));
        TEST_ASSERT_EQUAL(1, transport.requests);
        dispose(h);
    }
}

TEST_CASE("chat destruction before late callbacks releases only the local view", "[chat]")
{
    esp_openclaw_chat_handle_t h = prepare();
    esp_openclaw_chat_destroy(h);
    reply(true, history);
    TEST_ASSERT_EQUAL(1, transport.requests);
    h = sending();
    esp_openclaw_chat_destroy(h);
    reply(true, started);
    TEST_ASSERT_EQUAL(2, transport.requests);
}
