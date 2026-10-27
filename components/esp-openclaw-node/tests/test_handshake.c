#include "esp_openclaw_node_handshake.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t allocated, peak, budget = 8192;
typedef union { max_align_t alignment; size_t size; } allocation_t;

static void *limited_malloc(size_t size)
{
    if (size > budget - allocated) return NULL;
    allocation_t *allocation = malloc(sizeof(*allocation) + size);
    if (allocation == NULL) return NULL;
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

static cJSON *parse(const char *json)
{
    return esp_openclaw_node_parse_handshake(json, strlen(json));
}

static const char *text(const cJSON *object, const char *key)
{
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(object, key);
    assert(cJSON_IsString(field));
    return field->valuestring;
}

int main(void)
{
    cJSON_Hooks hooks = {.malloc_fn = limited_malloc, .free_fn = limited_free};
    cJSON_InitHooks(&hooks);
    const char prefix[] = "{\"payload\":{\"snapshot\":[null";
    const char row[] = ",{\"name\":\"ignored\",\"cap\":true,\"value\":123}";
    const char suffix[] = "] ,\"type\":\"hello-ok\",\"auth\":{\"deviceToken\":\"synthetic-node-token\","
        "\"deviceTokens\":[{\"role\":\"operator\",\"deviceToken\":\"synthetic-reader-token\"}]},"
        "\"pluginSurfaceUrls\":{\"canvas\":\"https://example.com/canvas\"}},"
        "\"id\":\"connect-1\",\"ok\":true,\"type\":\"res\"}";
    enum { ROWS = 400 };
    char json[sizeof(prefix) - 1 + ROWS * (sizeof(row) - 1) + sizeof(suffix)];
    char *next = json;
    memcpy(next, prefix, sizeof(prefix) - 1);
    next += sizeof(prefix) - 1;
    for (unsigned i = 0; i < ROWS; ++i) {
        memcpy(next, row, sizeof(row) - 1);
        next += sizeof(row) - 1;
    }
    memcpy(next, suffix, sizeof(suffix));
    assert(cJSON_Parse(json) == NULL && allocated == 0);
    peak = 0;
    cJSON *root = parse(json);
    assert(root != NULL && peak < 4096);
    assert(strcmp(text(root, "id"), "connect-1") == 0);
    cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    assert(cJSON_GetObjectItemCaseSensitive(payload, "snapshot") == NULL);
    cJSON *auth = cJSON_GetObjectItemCaseSensitive(payload, "auth");
    assert(strcmp(text(auth, "deviceToken"), "synthetic-node-token") == 0);
    cJSON *handoff = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(auth, "deviceTokens"), 0);
    assert(strcmp(text(handoff, "role"), "operator") == 0);
    assert(strcmp(text(handoff, "deviceToken"), "synthetic-reader-token") == 0);
    assert(strcmp(text(cJSON_GetObjectItemCaseSensitive(payload, "pluginSurfaceUrls"), "canvas"),
                  "https://example.com/canvas") == 0);
    /* Headroom remains for the session copy while the parsed reply is alive. */
    void *session = limited_malloc(4096);
    assert(session != NULL);
    limited_free(session);
    cJSON_Delete(root);
    assert(allocated == 0);

    root = parse("{\"event\":\"connect.challenge\",\"type\":\"event\",\"payload\":{\"nonce\":\"challenge\",\"ts\":1790000000000}}");
    assert(root != NULL);
    payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    assert(strcmp(text(payload, "nonce"), "challenge") == 0);
    assert(cJSON_GetObjectItemCaseSensitive(payload, "ts")->valuedouble == 1790000000000.0);
    cJSON_Delete(root);

    root = parse("{\"type\":\"res\",\"ok\":false,\"error\":{\"message\":\"Denied\",\"details\":{\"code\":\"PAIRING_REQUIRED\",\"requestId\":\"request-1\"}}}");
    assert(root != NULL);
    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    assert(strcmp(text(error, "message"), "Denied") == 0);
    cJSON *details = cJSON_GetObjectItemCaseSensitive(error, "details");
    assert(strcmp(text(details, "code"), "PAIRING_REQUIRED") == 0);
    assert(strcmp(text(details, "requestId"), "request-1") == 0);
    cJSON_Delete(root);

    root = parse("{\"ty\\u0070e\":\"res\",\"type\":\"event\",\"payload\":{\"au\\u0074h\":{\"role\":\"node\"},\"auth\":null},\"ignored\":\"\\\"{}[]\\\\\"}");
    assert(root != NULL && strcmp(text(root, "type"), "res") == 0);
    payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    assert(strcmp(text(cJSON_GetObjectItemCaseSensitive(payload, "auth"), "role"), "node") == 0);
    cJSON_Delete(root);

    const char *invalid[] = {
        "{} trailing", "{\"payload\":{\"snapshot\":[1,]}}", "{\"ignored\":[1}}",
        "{\"ignored\":01}", "{\"ignored\":1.}", "{\"ignored\":1e+}",
        "{\"ignored\":tru}", "{\"ignored\":null false}", "{\"ignored\":\"\\uZZZZ\"}",
        "{\"ignored\":\"\\ud800\"}", "{\"ignored\":\"\\udc00\"}",
        "{\"ignored\":\"\\q\"}", "{\"ignored\":\"raw\nline\"}", "[]", "",
        "{\"ignored\":\"\xc0\xaf\"}", "{\"ignored\":\"\xf4\x90\x80\x80\"}",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) assert(parse(invalid[i]) == NULL);
    const char nul[] = "{}\0";
    assert(esp_openclaw_node_parse_handshake(nul, sizeof(nul) - 1) == NULL);
    char deep[160] = "{\"ignored\":";
    size_t offset = strlen(deep);
    for (unsigned i = 0; i < 65; ++i) deep[offset++] = '[';
    deep[offset++] = '0';
    for (unsigned i = 0; i < 65; ++i) deep[offset++] = ']';
    deep[offset++] = '}';
    deep[offset] = '\0';
    assert(parse(deep) == NULL);

    /* Allocation failures leave no partial result or leaked retained fields. */
    for (budget = 0; budget < 1024; budget += 16) {
        root = parse("{\"type\":\"res\",\"payload\":{\"type\":\"hello-ok\",\"auth\":{\"deviceToken\":\"synthetic\"}}}");
        cJSON_Delete(root);
        assert(allocated == 0);
    }
    cJSON_InitHooks(NULL);
    puts("Handshake: constrained heap, complete validation, auth, handoff and challenge passed");
    return 0;
}
