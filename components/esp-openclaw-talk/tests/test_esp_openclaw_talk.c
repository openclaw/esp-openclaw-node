#include <string.h>

#include "unity.h"

#define esp_openclaw_node_gateway_request test_talk_gateway_request
#include "../src/esp_openclaw_talk.c"
#undef esp_openclaw_node_gateway_request

typedef struct {
    esp_openclaw_node_gateway_request_cb_t callback;
    void *ctx;
    esp_openclaw_node_handle_t node;
} pending_talk_request_t;

typedef struct {
    pending_talk_request_t config;
    pending_talk_request_t create;
    char config_params[64];
    char create_params[512];
    char close_params[512];
    esp_openclaw_node_handle_t close_node;
    esp_err_t config_submit_error;
    esp_err_t create_submit_error;
    int config_count;
    int create_count;
    int ownership_rejections;
    int close_count;
    int ice_count;
    int connected_count;
    int signaling_close_count;
    int setup_failed_count;
    esp_openclaw_talk_setup_result_t setup_result;
} talk_test_state_t;

static talk_test_state_t s_talk;

static void assert_not_critical(void)
{
#ifdef OPENCLAW_TALK_HOST_TEST
    TEST_ASSERT_EQUAL_MESSAGE(0, talk_host_critical_depth,
        "Gateway submissions and callbacks must not hold a critical section");
#endif
}

static void reset_talk_state(void)
{
    memset(&s_talk, 0, sizeof(s_talk));
#ifdef OPENCLAW_TALK_HOST_TEST
    talk_host_error_count = 0;
#endif
}

static void capture_params(char *destination, size_t capacity, const char *params)
{
    int length = snprintf(destination, capacity, "%s", params);
    TEST_ASSERT_TRUE_MESSAGE(length >= 0 && (size_t)length < capacity,
        "Test transport must not truncate the request under test");
}

esp_err_t test_talk_gateway_request(
    esp_openclaw_node_handle_t node,
    const char *method,
    const char *params_json,
    esp_openclaw_node_gateway_request_cb_t callback,
    void *user_ctx)
{
    assert_not_critical();
    pending_talk_request_t request = {.callback = callback, .ctx = user_ctx, .node = node};
    if (strcmp(method, "talk.config") == 0) {
        ++s_talk.config_count;
        capture_params(s_talk.config_params, sizeof(s_talk.config_params), params_json);
        if (s_talk.config_submit_error != ESP_OK) return s_talk.config_submit_error;
        TEST_ASSERT_NULL(s_talk.config.callback);
        s_talk.config = request;
    } else if (strcmp(method, "talk.client.create") == 0) {
        ++s_talk.create_count;
        capture_params(s_talk.create_params, sizeof(s_talk.create_params), params_json);
        if (s_talk.create_submit_error != ESP_OK) return s_talk.create_submit_error;
        TEST_ASSERT_NULL(s_talk.create.callback);
        s_talk.create = request;
    } else {
        TEST_ASSERT_EQUAL_STRING("talk.client.close", method);
        ++s_talk.close_count;
        s_talk.close_node = node;
        capture_params(s_talk.close_params, sizeof(s_talk.close_params), params_json);
    }
    return ESP_OK;
}

static int record_ice(esp_peer_signaling_ice_info_t *info, void *ctx)
{
    (void)ctx;
    assert_not_critical();
    TEST_ASSERT_TRUE(info->is_initiator);
    ++s_talk.ice_count;
    return 0;
}

static int record_connected(void *ctx)
{
    (void)ctx;
    assert_not_critical();
    ++s_talk.connected_count;
    return 0;
}

static int record_close(void *ctx)
{
    (void)ctx;
    assert_not_critical();
    ++s_talk.signaling_close_count;
    return 0;
}

static void record_setup_failed(esp_openclaw_talk_setup_result_t result, void *ctx)
{
    (void)ctx;
    assert_not_critical();
    ++s_talk.setup_failed_count;
    s_talk.setup_result = result;
}

static int begin_signaling(const char *key, esp_openclaw_node_handle_t node,
    esp_peer_signaling_handle_t *handle)
{
    const esp_openclaw_talk_signaling_config_t extra = {
        .operator_node = node,
        .gateway_http_base_url = "https://gateway.example/",
        .session_key = key,
        .setup_failed_cb = record_setup_failed,
    };
    esp_peer_signaling_cfg_t config = {
        .on_ice_info = record_ice,
        .on_connected = record_connected,
        .on_close = record_close,
        .extra_cfg = (void *)&extra,
    };
    return esp_openclaw_talk_signaling_impl()->start(&config, handle);
}

static esp_peer_signaling_handle_t start_on_node(const char *key, esp_openclaw_node_handle_t node)
{
    reset_talk_state();
    esp_peer_signaling_handle_t handle = NULL;
    TEST_ASSERT_EQUAL(ESP_PEER_ERR_NONE, begin_signaling(key, node, &handle));
    TEST_ASSERT_NOT_NULL(handle);
    return handle;
}

static esp_peer_signaling_handle_t start_signaling_with_key(const char *key)
{
    return start_on_node(key, (esp_openclaw_node_handle_t)1);
}

static esp_peer_signaling_handle_t start_signaling(void)
{
    esp_peer_signaling_handle_t handle = start_signaling_with_key("main");
    TEST_ASSERT_NOT_NULL(s_talk.create.callback);
    return handle;
}

static pending_talk_request_t take_request(pending_talk_request_t *pending)
{
    pending_talk_request_t request = *pending;
    *pending = (pending_talk_request_t){0};
    return request;
}

static void reply_request(pending_talk_request_t request, bool ok, const char *payload, const char *error)
{
    const esp_openclaw_node_gateway_result_t result = {
        .ok = ok, .payload_json = payload, .error_code = error,
    };
    TEST_ASSERT_NOT_NULL(request.callback);
    request.callback(request.node, &result, request.ctx);
}

static void respond_to_config(bool ok, const char *payload, const char *error)
{
    reply_request(take_request(&s_talk.config), ok, payload, error);
}

static void respond_to_create(bool ok, const char *payload, const char *error)
{
    reply_request(take_request(&s_talk.create), ok, payload, error);
}

static void stop_signaling(esp_peer_signaling_handle_t handle)
{
    TEST_ASSERT_EQUAL(ESP_PEER_ERR_NONE, esp_openclaw_talk_signaling_impl()->stop(handle));
}

static void stop_and_drain(esp_peer_signaling_handle_t handle)
{
    stop_signaling(handle);
    if (s_talk.config.callback != NULL) respond_to_config(false, NULL, "UNAVAILABLE");
    if (s_talk.create.callback != NULL) respond_to_create(false, NULL, "UNAVAILABLE");
}

static const char *valid_response(void)
{
    return "{\"transport\":\"webrtc\",\"offerUrl\":\"/talk/offer\","
           "\"clientSecret\":\"broker-token\",\"voiceSessionId\":\"voice-1\","
           "\"offerHeaders\":{\"X-Talk\":\"one\"},"
           "\"clientControl\":{\"owner\":\"gateway\"}}";
}

static char *routing_config(const char *owner, const char *main_key)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON *config = cJSON_AddObjectToObject(payload, "config");
    if (owner != NULL) cJSON_AddStringToObject(cJSON_AddObjectToObject(config, "talk"), "agentId", owner);
    if (main_key != NULL) cJSON_AddStringToObject(cJSON_AddObjectToObject(config, "session"), "mainKey", main_key);
    char *json = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    TEST_ASSERT_NOT_NULL(json);
    return json;
}

static bool request_has_string(const char *params, const char *field, const char *expected)
{
    cJSON *request = cJSON_Parse(params);
    cJSON *value = cJSON_GetObjectItemCaseSensitive(request, field);
    bool matches = cJSON_IsString(value) && strcmp(value->valuestring, expected) == 0;
    cJSON_Delete(request);
    return matches;
}

static void assert_private_config_request(void)
{
    cJSON *request = cJSON_Parse(s_talk.config_params);
    bool valid = cJSON_IsObject(request) && cJSON_GetArraySize(request) == 1 &&
        cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(request, "includeSecrets"));
    cJSON_Delete(request);
    TEST_ASSERT_TRUE_MESSAGE(valid, "Default discovery must explicitly request includeSecrets:false");
}

static void respond_as_configured_gateway(const char *owner_key)
{
    bool owned = request_has_string(s_talk.create_params, "sessionKey", owner_key);
    if (!owned) {
        ++s_talk.ownership_rejections;
        cJSON *request = cJSON_Parse(s_talk.create_params);
        cJSON *key = cJSON_GetObjectItemCaseSensitive(request, "sessionKey");
        printf("fake Gateway: INVALID_REQUEST, rejected sessionKey=%s; required=%s\n",
            cJSON_IsString(key) ? key->valuestring : "<missing>", owner_key);
        cJSON_Delete(request);
    }
    respond_to_create(owned, owned ? valid_response() : NULL, owned ? NULL : "INVALID_REQUEST");
}

static void assert_setup_failure(void)
{
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_TALK_SETUP_FAILED, s_talk.setup_result);
    TEST_ASSERT_EQUAL(1, s_talk.setup_failed_count);
    TEST_ASSERT_EQUAL(1, s_talk.signaling_close_count);
    TEST_ASSERT_EQUAL(0, s_talk.connected_count);
    TEST_ASSERT_EQUAL(0, s_talk.ice_count);
#ifdef OPENCLAW_TALK_HOST_TEST
    TEST_ASSERT_EQUAL(1, talk_host_error_count);
#endif
}

TEST_CASE("default Talk uses the configured Gateway owner for create and close", "[esp_openclaw_talk]")
{
    esp_peer_signaling_handle_t handle = start_signaling_with_key(NULL);
    /* Old production reaches the fake Gateway and is rejected for its actual
     * ownerless key, not for failing a scripted discovery RPC sequence. */
    if (s_talk.config.callback != NULL) respond_to_config(true, "{\"config\":{\"talk\":{\"agentId\":\"guide\"}}}", NULL);
    respond_as_configured_gateway("agent:guide:main");
    stop_signaling(handle);
    TEST_ASSERT_EQUAL_MESSAGE(0, s_talk.ownership_rejections,
        "Gateway rejected ownerless create; the configured Talk owner must be used");
    TEST_ASSERT_EQUAL(1, s_talk.connected_count);
    TEST_ASSERT_EQUAL(0, s_talk.setup_failed_count);
    TEST_ASSERT_EQUAL(1, s_talk.close_count);
    TEST_ASSERT_TRUE(request_has_string(s_talk.close_params, "sessionKey", "agent:guide:main"));
    TEST_ASSERT_TRUE(request_has_string(s_talk.close_params, "voiceSessionId", "voice-1"));
}

TEST_CASE("default discovery observes owner and main key on every Talk attempt", "[esp_openclaw_talk]")
{
    const struct { const char *key, *owner, *main_key, *expected; } cases[] = {
        {"", "guide", NULL, "agent:guide:main"},
        {NULL, "\tGuide_2-Room\r\n", " \tDESK\n", "agent:guide_2-room:desk"},
        {NULL, "\xc2\xa0" "GuIdE" "\xef\xbb\xbf", "\xe3\x80\x80" "CAF\xc3\x89" "\xe2\x80\xaf", "agent:guide:caf\xc3\x89"},
        {NULL, "second", "\xc2\xa0\xe2\x80\x89\xef\xbb\xbf", "agent:second:main"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        esp_peer_signaling_handle_t handle = start_signaling_with_key(cases[i].key);
        char *config = routing_config(cases[i].owner, cases[i].main_key);
        if (s_talk.config.callback != NULL) respond_to_config(true, config, NULL);
        free(config);
        respond_as_configured_gateway(cases[i].expected);
        stop_signaling(handle);
        TEST_ASSERT_EQUAL(0, s_talk.ownership_rejections);
        TEST_ASSERT_EQUAL(1, s_talk.config_count);
        assert_private_config_request();
        TEST_ASSERT_EQUAL(1, s_talk.create_count);
        TEST_ASSERT_EQUAL(1, s_talk.connected_count);
        TEST_ASSERT_EQUAL(1, s_talk.close_count);
        TEST_ASSERT_TRUE(request_has_string(s_talk.close_params, "sessionKey", cases[i].expected));
    }
}

TEST_CASE("explicit Talk keys bypass discovery and remain unchanged on close", "[esp_openclaw_talk]")
{
    const char *keys[] = {"agent:manual:MixedCase", "main", "  explicit-key  "};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        esp_peer_signaling_handle_t handle = start_signaling_with_key(keys[i]);
        respond_as_configured_gateway(keys[i]);
        stop_signaling(handle);
        TEST_ASSERT_EQUAL(0, s_talk.config_count);
        TEST_ASSERT_EQUAL(0, s_talk.ownership_rejections);
        TEST_ASSERT_EQUAL(1, s_talk.connected_count);
        TEST_ASSERT_TRUE(request_has_string(s_talk.close_params, "sessionKey", keys[i]));
    }
}

TEST_CASE("valid missing Talk owner retains explicit main and Gateway ambiguity rejection", "[esp_openclaw_talk]")
{
    const char *configs[] = {
        "{\"config\":{}}",
        "{\"config\":{\"talk\":{\"agentId\":\"\"},\"session\":{\"mainKey\":\"desk\"}}}",
        "{\"config\":{\"talk\":{\"agentId\":\" \\u00a0 \"}}}",
        "{\"config\":{\"talk\":{},\"agents\":{\"list\":[{\"id\":\"wrong-default\"}]},\"systemAgent\":{\"agentId\":\"wrong-default\"}}}",
    };
    for (size_t i = 0; i < sizeof(configs) / sizeof(configs[0]); ++i) {
        esp_peer_signaling_handle_t handle = start_signaling_with_key(NULL);
        respond_to_config(true, configs[i], NULL);
        TEST_ASSERT_TRUE(request_has_string(s_talk.create_params, "sessionKey", "main"));
        respond_to_create(false, NULL, "INVALID_REQUEST");
        stop_signaling(handle);
        assert_setup_failure();
        TEST_ASSERT_EQUAL(1, s_talk.config_count);
        TEST_ASSERT_EQUAL(1, s_talk.create_count);
        TEST_ASSERT_EQUAL(0, s_talk.close_count);
    }
}

TEST_CASE("malformed Talk routing fails without a fallback create", "[esp_openclaw_talk]")
{
    const char *configs[] = {
        NULL, "not-json", "{}", "{\"config\":null}", "{\"config\":[]}",
        "{\"config\":{\"talk\":[]}}", "{\"config\":{\"session\":false}}",
        "{\"config\":{\"talk\":{\"agentId\":7}}}", "{\"config\":{\"talk\":{\"agentId\":null}}}",
        "{\"config\":{\"session\":{\"mainKey\":7}}}",
        "{\"config\":{\"talk\":{\"agentId\":\"guide\"},\"session\":{\"mainKey\":null}}}",
        "{\"config\":{\"talk\":{\"agentId\":\"bad:owner\"}}}",
        "{\"config\":{\"talk\":{\"agentId\":\"bad owner\"}}}",
        "{\"config\":{\"talk\":{\"agentId\":\"-owner\"}}}",
        "{\"config\":{\"talk\":{\"agentId\":\"\\u00e9\"}}}",
    };
    for (size_t i = 0; i < sizeof(configs) / sizeof(configs[0]); ++i) {
        esp_peer_signaling_handle_t handle = start_signaling_with_key(NULL);
        respond_to_config(true, configs[i], NULL);
        stop_and_drain(handle);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_talk.create_count, configs[i] != NULL ? configs[i] : "missing payload");
        assert_setup_failure();
        TEST_ASSERT_EQUAL(0, s_talk.close_count);
    }
}

TEST_CASE("routing input and composed keys reject oversize without truncation", "[esp_openclaw_talk]")
{
    char owner64[65], owner65[66], oversized[258], main185[186], main186[187];
    memset(owner64, 'a', sizeof(owner64) - 1); owner64[64] = '\0';
    memset(owner65, 'a', sizeof(owner65) - 1); owner65[65] = '\0';
    memset(oversized, 'x', sizeof(oversized) - 1); oversized[257] = '\0';
    memset(main185, 'm', sizeof(main185) - 1); main185[185] = '\0';
    memset(main186, 'm', sizeof(main186) - 1); main186[186] = '\0';
    const struct { const char *name, *owner, *main_key; bool accepted; } cases[] = {
        {"256-byte composed key", owner64, main185, true},
        {"257-byte composed key", owner64, main186, false},
        {"65-byte owner slug", owner65, NULL, false},
        {"257-byte owner input", oversized, NULL, false},
        {"257-byte main input without owner", NULL, oversized, false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        esp_peer_signaling_handle_t handle = start_signaling_with_key(NULL);
        char *config = routing_config(cases[i].owner, cases[i].main_key);
        respond_to_config(true, config, NULL);
        free(config);
        if (cases[i].accepted) {
            char expected[257];
            snprintf(expected, sizeof(expected), "agent:%s:%s", owner64, main185);
            respond_as_configured_gateway(expected);
            stop_signaling(handle);
            TEST_ASSERT_EQUAL(0, s_talk.ownership_rejections);
            TEST_ASSERT_EQUAL(1, s_talk.connected_count);
            TEST_ASSERT_TRUE(request_has_string(s_talk.close_params, "sessionKey", expected));
        } else {
            stop_and_drain(handle);
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_talk.create_count, cases[i].name);
            assert_setup_failure();
        }
    }
}

TEST_CASE("configuration RPC failures do not submit a default create", "[esp_openclaw_talk]")
{
    const char *errors[] = {"UNAVAILABLE", "INVALID_REQUEST", "NOT_SUPPORTED"};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        esp_peer_signaling_handle_t handle = start_signaling_with_key(NULL);
        respond_to_config(false, NULL, errors[i]);
        stop_signaling(handle);
        assert_setup_failure();
        TEST_ASSERT_EQUAL(0, s_talk.create_count);
        TEST_ASSERT_EQUAL(0, s_talk.close_count);
    }
}

TEST_CASE("immediate submission failures release the request and notify only its owner", "[esp_openclaw_talk]")
{
    const char *keys[] = {NULL, "agent:manual:main"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        reset_talk_state();
        s_talk.config_submit_error = s_talk.create_submit_error = ESP_FAIL;
        esp_peer_signaling_handle_t handle = NULL;
        TEST_ASSERT_EQUAL(ESP_PEER_ERR_FAIL, begin_signaling(keys[i], (esp_openclaw_node_handle_t)1, &handle));
        TEST_ASSERT_NULL(handle);
        TEST_ASSERT_NULL(s_talk.config.callback);
        TEST_ASSERT_NULL(s_talk.create.callback);
        TEST_ASSERT_EQUAL(0, s_talk.setup_failed_count);
        TEST_ASSERT_EQUAL(0, s_talk.signaling_close_count);
    }
    esp_peer_signaling_handle_t handle = start_signaling_with_key(NULL);
    s_talk.create_submit_error = ESP_FAIL;
    respond_to_config(true, "{\"config\":{\"talk\":{\"agentId\":\"guide\"}}}", NULL);
    TEST_ASSERT_NULL(s_talk.create.callback);
    TEST_ASSERT_TRUE(request_has_string(s_talk.create_params, "sessionKey", "agent:guide:main"));
    stop_signaling(handle);
    assert_setup_failure();
    TEST_ASSERT_EQUAL(1, s_talk.create_count);
    TEST_ASSERT_EQUAL(0, s_talk.close_count);
}

TEST_CASE("stop before configuration ignores late success malformed data and RPC failure", "[esp_openclaw_talk]")
{
    const char *payloads[] = {"{\"config\":{\"talk\":{\"agentId\":\"guide\"}}}", "malformed", NULL};
    for (size_t i = 0; i < sizeof(payloads) / sizeof(payloads[0]); ++i) {
        esp_peer_signaling_handle_t handle = start_signaling_with_key(NULL);
        stop_signaling(handle);
        respond_to_config(payloads[i] != NULL, payloads[i], payloads[i] == NULL ? "INVALID_REQUEST" : NULL);
        TEST_ASSERT_EQUAL(0, s_talk.create_count);
        TEST_ASSERT_EQUAL(0, s_talk.close_count);
        TEST_ASSERT_EQUAL(0, s_talk.setup_failed_count);
        TEST_ASSERT_EQUAL(0, s_talk.connected_count);
        TEST_ASSERT_EQUAL(1, s_talk.signaling_close_count);
#ifdef OPENCLAW_TALK_HOST_TEST
        TEST_ASSERT_EQUAL(0, talk_host_error_count);
#endif
    }
}

TEST_CASE("Gateway-owned Talk create preserves the broker offer", "[esp_openclaw_talk]")
{
    esp_peer_signaling_handle_t handle = start_signaling();
    TEST_ASSERT_NOT_NULL(strstr(s_talk.create_params, "\"capabilities\":[\"gateway-control-v1\"]"));
    respond_to_create(true, valid_response(), NULL);
    talk_signaling_t *talk = handle;
    TEST_ASSERT_TRUE(talk->gateway_owned);
    TEST_ASSERT_EQUAL_STRING("https://gateway.example/talk/offer", talk->offer_url);
    TEST_ASSERT_EQUAL_STRING("broker-token", talk->client_secret);
    TEST_ASSERT_EQUAL_STRING("X-Talk", talk->offer_headers[0].name);
    TEST_ASSERT_EQUAL(1, s_talk.ice_count);
    TEST_ASSERT_EQUAL(1, s_talk.connected_count);
    stop_signaling(handle);
    TEST_ASSERT_EQUAL(1, s_talk.close_count);
    TEST_ASSERT_TRUE(request_has_string(s_talk.close_params, "voiceSessionId", "voice-1"));
}

TEST_CASE("Talk rejects non-Gateway control before peer creation", "[esp_openclaw_talk]")
{
    const char *responses[] = {
        "{\"transport\":\"webrtc\",\"offerUrl\":\"/o\",\"clientSecret\":\"t\",\"voiceSessionId\":\"missing\"}",
        "{\"transport\":\"webrtc\",\"offerUrl\":\"/o\",\"clientSecret\":\"t\",\"voiceSessionId\":\"client\",\"clientControl\":{\"owner\":\"client\"}}",
        "{\"transport\":\"webrtc\",\"offerUrl\":\"/o\",\"clientSecret\":\"t\",\"voiceSessionId\":\"extra\",\"clientControl\":{\"owner\":\"gateway\",\"version\":1}}",
    };
    for (size_t i = 0; i < sizeof(responses) / sizeof(responses[0]); ++i) {
        esp_peer_signaling_handle_t handle = start_signaling();
        respond_to_create(true, responses[i], NULL);
        stop_signaling(handle);
        TEST_ASSERT_EQUAL(ESP_OPENCLAW_TALK_GATEWAY_UPGRADE_REQUIRED, s_talk.setup_result);
        TEST_ASSERT_EQUAL(1, s_talk.setup_failed_count);
        TEST_ASSERT_EQUAL(1, s_talk.signaling_close_count);
        TEST_ASSERT_EQUAL(0, s_talk.ice_count);
        TEST_ASSERT_EQUAL(1, s_talk.close_count);
    }
}

TEST_CASE("late Talk create closes the frozen owner after stop and ignores stale failure", "[esp_openclaw_talk]")
{
    for (int failed = 0; failed < 2; ++failed) {
        esp_peer_signaling_handle_t handle = start_signaling_with_key(NULL);
        respond_to_config(true, "{\"config\":{\"talk\":{\"agentId\":\"guide\"},\"session\":{\"mainKey\":\"desk\"}}}", NULL);
        stop_signaling(handle);
        respond_to_create(!failed, failed ? NULL : valid_response(), failed ? "INVALID_REQUEST" : NULL);
        TEST_ASSERT_EQUAL(failed ? 0 : 1, s_talk.close_count);
        if (!failed) TEST_ASSERT_TRUE(request_has_string(s_talk.close_params, "sessionKey", "agent:guide:desk"));
        TEST_ASSERT_EQUAL(0, s_talk.ice_count);
        TEST_ASSERT_EQUAL(0, s_talk.setup_failed_count);
        TEST_ASSERT_EQUAL(1, s_talk.signaling_close_count);
#ifdef OPENCLAW_TALK_HOST_TEST
        TEST_ASSERT_EQUAL(0, talk_host_error_count);
#endif
    }
}

TEST_CASE("cancelled callbacks cannot affect a new call with another configured owner", "[esp_openclaw_talk]")
{
    for (int old_create = 0; old_create < 2; ++old_create) {
        esp_peer_signaling_handle_t old = start_on_node(NULL, (esp_openclaw_node_handle_t)1);
        if (old_create) respond_to_config(true, "{\"config\":{\"talk\":{\"agentId\":\"first\"}}}", NULL);
        pending_talk_request_t late = take_request(old_create ? &s_talk.create : &s_talk.config);
        stop_signaling(old);
        esp_peer_signaling_handle_t current = start_on_node(NULL, (esp_openclaw_node_handle_t)2);
        respond_to_config(true, "{\"config\":{\"talk\":{\"agentId\":\"second\"},\"session\":{\"mainKey\":\"work\"}}}", NULL);
        reply_request(late, true, old_create ? valid_response() : "{\"config\":{\"talk\":{\"agentId\":\"first\"}}}", NULL);
        TEST_ASSERT_EQUAL(1, s_talk.create_count);
        TEST_ASSERT_EQUAL(old_create ? 1 : 0, s_talk.close_count);
        if (old_create) {
            TEST_ASSERT_EQUAL_PTR((esp_openclaw_node_handle_t)1, s_talk.close_node);
            TEST_ASSERT_TRUE(request_has_string(s_talk.close_params, "sessionKey", "agent:first:main"));
        }
        TEST_ASSERT_EQUAL(0, s_talk.setup_failed_count);
        TEST_ASSERT_EQUAL(0, s_talk.signaling_close_count);
        TEST_ASSERT_EQUAL(0, s_talk.connected_count);
        respond_as_configured_gateway("agent:second:work");
        stop_signaling(current);
        TEST_ASSERT_EQUAL(1, s_talk.connected_count);
        TEST_ASSERT_EQUAL(1, s_talk.signaling_close_count);
        TEST_ASSERT_EQUAL(old_create ? 2 : 1, s_talk.close_count);
        TEST_ASSERT_EQUAL_PTR((esp_openclaw_node_handle_t)2, s_talk.close_node);
        TEST_ASSERT_TRUE(request_has_string(s_talk.close_params, "sessionKey", "agent:second:work"));
    }
}

TEST_CASE("Talk close bounds voice session ids", "[esp_openclaw_talk]")
{
    char accepted[129], rejected[130];
    memset(accepted, 'a', sizeof(accepted) - 1U); accepted[sizeof(accepted) - 1U] = '\0';
    memset(rejected, 'b', sizeof(rejected) - 1U); rejected[sizeof(rejected) - 1U] = '\0';
    cJSON *accepted_json = cJSON_CreateString(accepted);
    cJSON *rejected_json = cJSON_CreateString(rejected);
    char *copy = duplicate_voice_session_id(accepted_json);
    TEST_ASSERT_EQUAL_STRING(accepted, copy);
    TEST_ASSERT_NULL(duplicate_voice_session_id(rejected_json));
    free(copy);
    cJSON_Delete(accepted_json);
    cJSON_Delete(rejected_json);
}

TEST_CASE("create rejection is classified accurately and a second call recovers", "[esp_openclaw_talk]")
{
    const struct { const char *code; esp_openclaw_talk_setup_result_t expected; } cases[] = {
        {"INVALID_REQUEST", ESP_OPENCLAW_TALK_SETUP_FAILED},
        {"UNAVAILABLE", ESP_OPENCLAW_TALK_SETUP_FAILED},
        {"NOT_SUPPORTED", ESP_OPENCLAW_TALK_GATEWAY_UPGRADE_REQUIRED},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        esp_peer_signaling_handle_t failed = start_signaling();
        respond_to_create(false, NULL, cases[i].code);
        stop_signaling(failed);
        TEST_ASSERT_EQUAL(cases[i].expected, s_talk.setup_result);
        TEST_ASSERT_EQUAL(1, s_talk.setup_failed_count);
        TEST_ASSERT_EQUAL(1, s_talk.signaling_close_count);
        TEST_ASSERT_EQUAL(0, s_talk.connected_count);
    }
    esp_peer_signaling_handle_t recovered = start_signaling();
    respond_to_create(true, valid_response(), NULL);
    TEST_ASSERT_EQUAL(1, s_talk.connected_count);
    TEST_ASSERT_EQUAL(0, s_talk.setup_failed_count);
    stop_signaling(recovered);
}
