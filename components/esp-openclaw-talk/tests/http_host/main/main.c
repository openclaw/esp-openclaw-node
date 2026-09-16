#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_openclaw_talk.h"

static esp_openclaw_node_gateway_request_cb_t create_callback;
static void *create_context;
static int answers;

esp_err_t esp_openclaw_node_gateway_request(esp_openclaw_node_handle_t node,
    const char *method, const char *params, esp_openclaw_node_gateway_request_cb_t callback, void *ctx)
{
    (void)node; (void)params;
    if (strcmp(method, "talk.client.create") == 0) {
        create_callback = callback;
        create_context = ctx;
    } else {
        assert(strcmp(method, "talk.client.close") == 0);
    }
    return ESP_OK;
}

static int on_message(esp_peer_signaling_msg_t *message, void *ctx)
{
    (void)ctx;
    assert(message->type == ESP_PEER_SIGNALING_MSG_SDP);
    assert(message->size >= 3 && memcmp(message->data, "v=0", 3) == 0);
    ++answers;
    return 0;
}

void app_main(void)
{
    const char *url = getenv("PROOF_ORIGIN");
    assert(url != NULL);
    esp_openclaw_talk_signaling_config_t extra = {
        .operator_node = (esp_openclaw_node_handle_t)1,
        .gateway_http_base_url = url,
        .session_key = "synthetic-proof",
    };
    esp_peer_signaling_cfg_t config = {
        .on_msg = on_message, .extra_cfg = &extra,
    };
    esp_peer_signaling_handle_t handle = NULL;
    const esp_peer_signaling_impl_t *impl = esp_openclaw_talk_signaling_impl();
    assert(impl->start(&config, &handle) == ESP_PEER_ERR_NONE);
    assert(create_callback != NULL);
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "transport", "webrtc");
    cJSON_AddStringToObject(payload, "offerUrl", getenv("PROOF_PATH"));
    cJSON_AddStringToObject(payload, "clientSecret", "synthetic-broker-token");
    cJSON_AddStringToObject(payload, "voiceSessionId", "synthetic-voice");
    cJSON_AddStringToObject(cJSON_AddObjectToObject(payload, "clientControl"), "owner", "gateway");
    char *json = cJSON_PrintUnformatted(payload);
    assert(json != NULL);
    esp_openclaw_node_gateway_result_t created = {.ok = true, .payload_json = json};
    create_callback(extra.operator_node, &created, create_context);
    free(json);
    cJSON_Delete(payload);
    const char *sdp = "v=0\r\no=synthetic 0 0 IN IP4 127.0.0.1\r\n";
    esp_peer_signaling_msg_t message = {
        .type = ESP_PEER_SIGNALING_MSG_SDP, .data = (uint8_t *)sdp, .size = strlen(sdp),
    };
    int result = impl->send_msg(handle, &message);
    printf("PROOF_RESULT result=%d answers=%d\n", result, answers);
    assert(impl->stop(handle) == ESP_PEER_ERR_NONE);
    fflush(stdout);
    exit(0);
}
