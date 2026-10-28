#include <stdio.h>
#include <string.h>
#include "unity.h"

#define esp_openclaw_node_gateway_request_on_connection test_transcription_request
#define esp_openclaw_node_get_connection_id test_transcription_connection
#define esp_timer_get_time test_transcription_time
#include "../src/esp_openclaw_transcription.c"
#undef esp_timer_get_time
#undef esp_openclaw_node_get_connection_id
#undef esp_openclaw_node_gateway_request_on_connection

static struct {
    esp_openclaw_node_gateway_request_cb_t callback;
    void *ctx;
    unsigned requests, closes;
    uint32_t connection;
    int64_t now;
    esp_err_t submit_error;
    char method[64], params[8192];
} transport;
static char node_storage;
#define NODE ((esp_openclaw_node_handle_t)&node_storage)

uint32_t test_transcription_connection(esp_openclaw_node_handle_t node)
{
    TEST_ASSERT_EQUAL_PTR(NODE, node);
    return transport.connection;
}

int64_t test_transcription_time(void) { return transport.now; }

esp_err_t test_transcription_request(esp_openclaw_node_handle_t node, uint32_t connection,
    const char *method, const char *params, esp_openclaw_node_gateway_request_cb_t cb, void *ctx)
{
    TEST_ASSERT_EQUAL_PTR(NODE, node);
    if (connection != transport.connection) return ESP_ERR_INVALID_STATE;
    if (transport.submit_error != ESP_OK) return transport.submit_error;
    TEST_ASSERT_NULL(transport.callback);
    TEST_ASSERT_LESS_THAN(sizeof(transport.params), strlen(params));
    strcpy(transport.method, method);
    strcpy(transport.params, params);
    transport.callback = cb;
    transport.ctx = ctx;
    ++transport.requests;
    if (strcmp(method, "talk.session.close") == 0) ++transport.closes;
    return ESP_OK;
}

static void reply(bool ok, const char *json)
{
    esp_openclaw_node_gateway_request_cb_t cb = transport.callback;
    void *ctx = transport.ctx;
    TEST_ASSERT_NOT_NULL(cb);
    transport.callback = NULL;
    transport.ctx = NULL;
    const esp_openclaw_node_gateway_result_t result = {.ok = ok, .payload_json = json};
    cb(NODE, &result, ctx);
}

static const char created[] = "{\"sessionId\":\"draft-1\",\"transcriptionSessionId\":\"draft-1\","
    "\"mode\":\"transcription\",\"transport\":\"gateway-relay\",\"brain\":\"none\","
    "\"audio\":{\"inputEncoding\":\"g711_ulaw\",\"inputSampleRateHz\":8000}}";

static esp_openclaw_transcription_handle_t begin(void)
{
    memset(&transport, 0, sizeof(transport));
    transport.connection = 7;
    esp_openclaw_transcription_handle_t h = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_transcription_begin(NODE, &h));
    TEST_ASSERT_EQUAL_STRING("talk.session.create", transport.method);
    TEST_ASSERT_EQUAL_STRING("{\"mode\":\"transcription\",\"transport\":\"gateway-relay\",\"brain\":\"none\"}", transport.params);
    return h;
}

static void event(esp_openclaw_transcription_handle_t h, const char *id,
    const char *type, unsigned seq, const char *payload)
{
    char json[6144];
    snprintf(json, sizeof(json), "{\"transcriptionSessionId\":\"%s\",\"talkEvent\":{"
        "\"sessionId\":\"%s\",\"mode\":\"transcription\",\"transport\":\"gateway-relay\","
        "\"brain\":\"none\",\"type\":\"%s\",\"seq\":%u,\"payload\":%s}}",
        id, id, type, seq, payload);
    esp_openclaw_transcription_event(h, NODE, "talk.event", json);
}

static esp_openclaw_transcription_snapshot_t snapshot(esp_openclaw_transcription_handle_t h)
{
    esp_openclaw_transcription_snapshot_t result;
    esp_openclaw_transcription_copy(h, &result);
    return result;
}

static esp_openclaw_transcription_handle_t ready(void)
{
    esp_openclaw_transcription_handle_t h = begin();
    reply(true, created);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_TRANSCRIPTION_STARTING, snapshot(h).state);
    event(h, "draft-1", "session.ready", 1, "null");
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_TRANSCRIPTION_READY, snapshot(h).state);
    return h;
}

static void dispose(esp_openclaw_transcription_handle_t h)
{
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_transcription_destroy(h));
    if (transport.callback != NULL) reply(true, "{\"ok\":true}");
    TEST_ASSERT_NULL(transport.callback);
}

TEST_CASE("transcription waits for provider readiness even when it precedes create", "[transcription]")
{
    esp_openclaw_transcription_handle_t h = begin();
    const uint8_t audio[] = {0, 1, 2};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, esp_openclaw_transcription_append(h, audio, sizeof(audio)));
    event(h, "unrelated", "session.error", 1, "{}");
    event(h, "draft-1", "session.ready", 1, "null");
    reply(true, created);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_TRANSCRIPTION_READY, snapshot(h).state);
    dispose(h);
}

TEST_CASE("transcription returns all final segments only after close drain", "[transcription]")
{
    esp_openclaw_transcription_handle_t h = ready();
    const uint8_t audio[] = {0, 1, 2, 255};
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_transcription_append(h, audio, sizeof(audio)));
    TEST_ASSERT_EQUAL_STRING("talk.session.appendAudio", transport.method);
    TEST_ASSERT_EQUAL_STRING("{\"sessionId\":\"draft-1\",\"audioBase64\":\"AAEC/w==\"}", transport.params);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, esp_openclaw_transcription_finish(h));
    reply(true, "{\"ok\":true}");
    event(h, "draft-1", "transcript.delta", 2, "{\"text\":\"wrong partial\"}");
    event(h, "draft-1", "transcript.done", 3, "{\"text\":\"hello\"}");
    event(h, "draft-1", "transcript.done", 3, "{\"text\":\"duplicate\"}");
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_transcription_finish(h));
    event(h, "draft-1", "input.audio.committed", 4, "{}");
    reply(true, "{\"ok\":true}");
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_TRANSCRIPTION_FINISHING, snapshot(h).state);
    event(h, "draft-1", "turn.ended", 5, "{}");
    event(h, "other", "session.closed", 99, "{\"reason\":\"completed\"}");
    event(h, "draft-1", "transcript.done", 6, "{\"text\":\"world\"}");
    event(h, "draft-1", "session.closed", 7, "{\"reason\":\"completed\"}");
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_TRANSCRIPTION_COMPLETE, snapshot(h).state);
    TEST_ASSERT_EQUAL_STRING("hello world", snapshot(h).text);
    TEST_ASSERT_EQUAL(sizeof(audio), snapshot(h).audio_bytes);
    TEST_ASSERT_EQUAL(1, transport.closes);
    dispose(h);
    TEST_ASSERT_EQUAL(1, transport.closes);
}

TEST_CASE("transcription close event may precede acknowledgement and empty means no speech", "[transcription]")
{
    esp_openclaw_transcription_handle_t h = ready();
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_transcription_finish(h));
    event(h, "draft-1", "session.closed", 2, "{\"reason\":\"completed\"}");
    reply(true, "{\"ok\":true}");
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_TRANSCRIPTION_COMPLETE, snapshot(h).state);
    TEST_ASSERT_EQUAL_STRING("", snapshot(h).text);
    dispose(h);
}

TEST_CASE("transcription destruction waits until late create cleanup can be queued", "[transcription]")
{
    esp_openclaw_transcription_handle_t h = begin();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, esp_openclaw_transcription_destroy(h));
    transport.submit_error = ESP_ERR_NO_MEM;
    reply(true, created);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, esp_openclaw_transcription_destroy(h));
    transport.submit_error = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_transcription_destroy(h));
    TEST_ASSERT_EQUAL_STRING("talk.session.close", transport.method);
    reply(true, "{\"ok\":true}");
    TEST_ASSERT_EQUAL(1, transport.closes);
}

TEST_CASE("transcription cancels pending audio without making partial text sendable", "[transcription]")
{
    esp_openclaw_transcription_handle_t h = ready();
    event(h, "draft-1", "transcript.done", 2, "{\"text\":\"discard me\"}");
    const uint8_t audio[] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_transcription_append(h, audio, 1));
    esp_openclaw_transcription_cancel(h);
    TEST_ASSERT_EQUAL_STRING("", snapshot(h).text);
    reply(true, "{\"ok\":true}");
    event(h, "draft-1", "transcript.done", 3, "{\"text\":\"late words\"}");
    reply(true, "{\"ok\":true}");
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_TRANSCRIPTION_CANCELED, snapshot(h).state);
    TEST_ASSERT_EQUAL_STRING("", snapshot(h).text);
    dispose(h);
}

TEST_CASE("transcription fails closed on provider format or permission failure", "[transcription]")
{
    esp_openclaw_transcription_handle_t h = begin();
    reply(false, NULL);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_TRANSCRIPTION_ERROR_GATEWAY, snapshot(h).error);
    dispose(h);
    h = begin();
    reply(true, "{\"sessionId\":\"draft-1\",\"mode\":\"realtime\"}");
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_TRANSCRIPTION_ERROR_FORMAT, snapshot(h).error);
    TEST_ASSERT_EQUAL_STRING("talk.session.close", transport.method);
    dispose(h);
}

TEST_CASE("transcription reconnect never sends audio or cleanup on replacement connection", "[transcription]")
{
    esp_openclaw_transcription_handle_t h = ready();
    ++transport.connection;
    const uint8_t audio[] = {0};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, esp_openclaw_transcription_append(h, audio, 1));
    TEST_ASSERT_EQUAL(1, transport.requests);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_TRANSCRIPTION_FAILED, snapshot(h).state);
    dispose(h);
    TEST_ASSERT_EQUAL(0, transport.closes);
}

TEST_CASE("transcription timeout clears words and handles a late create reply", "[transcription]")
{
    esp_openclaw_transcription_handle_t h = begin();
    transport.now = 15000000;
    esp_openclaw_transcription_poll(h);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_TRANSCRIPTION_ERROR_TIMEOUT, snapshot(h).error);
    reply(true, created);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_TRANSCRIPTION_FAILED, snapshot(h).state);
    dispose(h);
    h = ready();
    event(h, "draft-1", "transcript.done", 2, "{\"text\":\"not complete\"}");
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_transcription_finish(h));
    reply(true, "{\"ok\":true}");
    transport.now = 10000000;
    esp_openclaw_transcription_poll(h);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_TRANSCRIPTION_ERROR_TIMEOUT, snapshot(h).error);
    TEST_ASSERT_EQUAL_STRING("", snapshot(h).text);
    dispose(h);
}

TEST_CASE("transcription enforces the recording cap and rejects oversized drafts", "[transcription]")
{
    esp_openclaw_transcription_handle_t h = ready();
    uint8_t audio[ESP_OPENCLAW_TRANSCRIPTION_CHUNK_BYTES] = {0};
    for (unsigned i = 0; i < 75; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_transcription_append(h, audio, sizeof(audio)));
        reply(true, "{\"ok\":true}");
    }
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_TRANSCRIPTION_AUDIO_BYTES, snapshot(h).audio_bytes);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esp_openclaw_transcription_append(h, audio, 1));
    char payload[ESP_OPENCLAW_TRANSCRIPTION_TEXT_BYTES + 32];
    memset(payload, 'x', sizeof(payload));
    memcpy(payload, "{\"text\":\"", 9);
    strcpy(payload + 9 + ESP_OPENCLAW_TRANSCRIPTION_TEXT_BYTES, "\"}");
    event(h, "draft-1", "transcript.done", 2, payload);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_TRANSCRIPTION_ERROR_TOO_LONG, snapshot(h).error);
    TEST_ASSERT_EQUAL_STRING("", snapshot(h).text);
    dispose(h);
}

TEST_CASE("transcription discards finalized words on provider or append failure", "[transcription]")
{
    esp_openclaw_transcription_handle_t h = ready();
    event(h, "draft-1", "transcript.done", 2, "{\"text\":\"discard me\"}");
    event(h, "draft-1", "session.error", 3, "{\"message\":\"provider failed\"}");
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_TRANSCRIPTION_ERROR_GATEWAY, snapshot(h).error);
    TEST_ASSERT_EQUAL_STRING("", snapshot(h).text);
    dispose(h);
    h = ready();
    const uint8_t audio[] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_transcription_append(h, audio, 1));
    reply(true, "{\"ok\":\"true\"}");
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_TRANSCRIPTION_ERROR_FORMAT, snapshot(h).error);
    dispose(h);
}

TEST_CASE("transcription retries a close rejected before queue admission", "[transcription]")
{
    esp_openclaw_transcription_handle_t h = ready();
    transport.submit_error = ESP_ERR_NO_MEM;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, esp_openclaw_transcription_finish(h));
    TEST_ASSERT_EQUAL(0, transport.closes);
    transport.submit_error = ESP_OK;
    esp_openclaw_transcription_poll(h);
    TEST_ASSERT_EQUAL(1, transport.closes);
    dispose(h);
}

TEST_CASE("transcription grants a fresh idle deadline after readiness and audio ack", "[transcription]")
{
    esp_openclaw_transcription_handle_t h = begin();
    reply(true, created);
    transport.now = 14900000;
    event(h, "draft-1", "session.ready", 1, "null");
    transport.now = 15000000;
    esp_openclaw_transcription_poll(h);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_TRANSCRIPTION_READY, snapshot(h).state);
    const uint8_t audio[] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, esp_openclaw_transcription_append(h, audio, 1));
    transport.now = 29900000;
    reply(true, "{\"ok\":true}");
    transport.now = 30000000;
    esp_openclaw_transcription_poll(h);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_TRANSCRIPTION_READY, snapshot(h).state);
    dispose(h);
    h = begin();
    event(h, "draft-1", "session.ready", 1, "null");
    transport.now = 14900000;
    reply(true, created);
    transport.now = 15000000;
    esp_openclaw_transcription_poll(h);
    TEST_ASSERT_EQUAL(ESP_OPENCLAW_TRANSCRIPTION_READY, snapshot(h).state);
    dispose(h);
}
