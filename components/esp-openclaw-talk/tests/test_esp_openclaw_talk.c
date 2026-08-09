#include <string.h>

#include "unity.h"

#define esp_openclaw_node_gateway_request test_talk_gateway_request
#include "../src/esp_openclaw_talk.c"
#undef esp_openclaw_node_gateway_request

typedef struct {
    esp_openclaw_node_gateway_request_cb_t create_cb;
    void *create_ctx;
    char create_params[512];
    char close_params[256];
    int close_count;
    int ice_count;
    int connected_count;
    int signaling_close_count;
    int setup_failed_count;
    esp_openclaw_talk_setup_result_t setup_result;
} talk_test_state_t;

static talk_test_state_t s_talk;

esp_err_t test_talk_gateway_request(
    esp_openclaw_node_handle_t node,
    const char *method,
    const char *params_json,
    esp_openclaw_node_gateway_request_cb_t callback,
    void *user_ctx)
{
    (void)node;
    if (strcmp(method, "talk.client.create") == 0) {
        s_talk.create_cb = callback;
        s_talk.create_ctx = user_ctx;
        snprintf(s_talk.create_params, sizeof(s_talk.create_params), "%s", params_json);
    } else {
        TEST_ASSERT_EQUAL_STRING("talk.client.close", method);
        ++s_talk.close_count;
        snprintf(s_talk.close_params, sizeof(s_talk.close_params), "%s", params_json);
    }
    return ESP_OK;
}

static int record_ice(esp_peer_signaling_ice_info_t *info, void *ctx)
{
    (void)ctx;
    TEST_ASSERT_TRUE(info->is_initiator);
    ++s_talk.ice_count;
    return 0;
}

static int record_connected(void *ctx)
{
    (void)ctx;
    ++s_talk.connected_count;
    return 0;
}

static int record_close(void *ctx)
{
    (void)ctx;
    ++s_talk.signaling_close_count;
    return 0;
}

static void record_setup_failed(esp_openclaw_talk_setup_result_t result, void *ctx)
{
    (void)ctx;
    ++s_talk.setup_failed_count;
    s_talk.setup_result = result;
}

static esp_peer_signaling_handle_t start_signaling(void)
{
    memset(&s_talk, 0, sizeof(s_talk));
    const esp_openclaw_talk_signaling_config_t extra = {
        .operator_node = (esp_openclaw_node_handle_t)1,
        .gateway_http_base_url = "https://gateway.example/",
        .session_key = "main",
        .setup_failed_cb = record_setup_failed,
    };
    esp_peer_signaling_cfg_t config = {
        .on_ice_info = record_ice,
        .on_connected = record_connected,
        .on_close = record_close,
        .extra_cfg = (void *)&extra,
    };
    esp_peer_signaling_handle_t handle = NULL;
    TEST_ASSERT_EQUAL(ESP_PEER_ERR_NONE, talk_signaling_start(&config, &handle));
    TEST_ASSERT_NOT_NULL(handle);
    TEST_ASSERT_NOT_NULL(s_talk.create_cb);
    return handle;
}

static void respond_to_create(bool ok, const char *payload, const char *error_code)
{
    esp_openclaw_node_gateway_request_cb_t callback = s_talk.create_cb;
    void *ctx = s_talk.create_ctx;
    s_talk.create_cb = NULL;
    s_talk.create_ctx = NULL;
    const esp_openclaw_node_gateway_result_t result = {
        .ok = ok,
        .payload_json = payload,
        .error_code = error_code,
    };
    callback(NULL, &result, ctx);
}

static const char *valid_response(void)
{
    return "{\"transport\":\"webrtc\",\"offerUrl\":\"/talk/offer\","
           "\"clientSecret\":\"broker-token\",\"voiceSessionId\":\"voice-1\","
           "\"offerHeaders\":{\"X-Talk\":\"one\"},"
           "\"clientControl\":{\"owner\":\"gateway\"}}";
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
    TEST_ASSERT_EQUAL(ESP_PEER_ERR_NONE, talk_signaling_stop(handle));
    TEST_ASSERT_EQUAL(1, s_talk.close_count);
    TEST_ASSERT_NOT_NULL(strstr(s_talk.close_params, "\"voiceSessionId\":\"voice-1\""));
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
        TEST_ASSERT_EQUAL(ESP_OPENCLAW_TALK_GATEWAY_UPGRADE_REQUIRED, s_talk.setup_result);
        TEST_ASSERT_EQUAL(1, s_talk.setup_failed_count);
        TEST_ASSERT_EQUAL(1, s_talk.signaling_close_count);
        TEST_ASSERT_EQUAL(0, s_talk.ice_count);
        TEST_ASSERT_EQUAL(1, s_talk.close_count);
        TEST_ASSERT_EQUAL(ESP_PEER_ERR_NONE, talk_signaling_stop(handle));
        TEST_ASSERT_EQUAL(1, s_talk.setup_failed_count);
    }
}

TEST_CASE("late Talk create closes after stop and ignores stale failure", "[esp_openclaw_talk]")
{
    esp_peer_signaling_handle_t handle = start_signaling();
    TEST_ASSERT_EQUAL(ESP_PEER_ERR_NONE, talk_signaling_stop(handle));
    respond_to_create(true, valid_response(), NULL);
    TEST_ASSERT_EQUAL(1, s_talk.close_count);
    TEST_ASSERT_EQUAL(0, s_talk.ice_count);
    TEST_ASSERT_EQUAL(0, s_talk.setup_failed_count);
    TEST_ASSERT_EQUAL(1, s_talk.signaling_close_count);
}

TEST_CASE("Talk close bounds voice session ids", "[esp_openclaw_talk]")
{
    char accepted[129];
    char rejected[130];
    memset(accepted, 'a', sizeof(accepted) - 1U);
    accepted[sizeof(accepted) - 1U] = '\0';
    memset(rejected, 'b', sizeof(rejected) - 1U);
    rejected[sizeof(rejected) - 1U] = '\0';
    cJSON *accepted_json = cJSON_CreateString(accepted);
    cJSON *rejected_json = cJSON_CreateString(rejected);
    char *copy = duplicate_voice_session_id(accepted_json);
    TEST_ASSERT_EQUAL_STRING(accepted, copy);
    TEST_ASSERT_NULL(duplicate_voice_session_id(rejected_json));
    free(copy);
    cJSON_Delete(accepted_json);
    cJSON_Delete(rejected_json);
}

TEST_CASE("Talk setup failure is actionable and a second call recovers", "[esp_openclaw_talk]")
{
    esp_peer_signaling_handle_t failed = start_signaling();
    respond_to_create(false, NULL, "UNAVAILABLE");
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_TALK_SETUP_FAILED, s_talk.setup_result);
    TEST_ASSERT_EQUAL(1, s_talk.setup_failed_count);
    TEST_ASSERT_EQUAL(ESP_PEER_ERR_NONE, talk_signaling_stop(failed));

    esp_peer_signaling_handle_t recovered = start_signaling();
    respond_to_create(true, valid_response(), NULL);
    TEST_ASSERT_EQUAL(1, s_talk.connected_count);
    TEST_ASSERT_EQUAL(0, s_talk.setup_failed_count);
    TEST_ASSERT_EQUAL(ESP_PEER_ERR_NONE, talk_signaling_stop(recovered));
}
