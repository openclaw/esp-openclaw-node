#include "esp_openclaw_device_control.h"
#include "cJSON.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *payload = "{\"ready\":true,\"heading\":\"Identify\",\"text\":\"Code: A1B2C3D4\","
    "\"generation\":7,\"renderedGeneration\":6,\"rendered\":false}";

static cJSON *envelope(bool as_string)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "nodeId", "target-node");
    cJSON_AddStringToObject(root, "command", "display.show");
    if (as_string) cJSON_AddStringToObject(root, "payloadJSON", payload);
    else cJSON_AddItemToObject(root, "payload", cJSON_Parse(payload));
    return root;
}

static bool parse(const char *json, const char *command, esp_openclaw_display_receipt_t *receipt)
{
    return esp_openclaw_display_receipt_parse(json, strlen(json), "target-node", command,
        "Identify", "Code: A1B2C3D4", receipt);
}

static void invalid(cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    assert(json != NULL);
    esp_openclaw_display_receipt_t receipt = {7, 7, true};
    assert(!parse(json, "display.show", &receipt));
    assert(receipt.generation == 0 && receipt.rendered_generation == 0 && !receipt.rendered);
    cJSON_free(json);
    cJSON_Delete(root);
}

int main(void)
{
    for (int as_string = 0; as_string < 2; ++as_string) {
        cJSON *root = envelope(as_string);
        char *json = cJSON_PrintUnformatted(root);
        esp_openclaw_display_receipt_t receipt;
        assert(parse(json, "display.show", &receipt));
        assert(receipt.generation == 7 && receipt.rendered_generation == 6 && !receipt.rendered);
        cJSON_free(json);
        cJSON_Delete(root);
    }

    cJSON *root = envelope(false);
    cJSON *body = cJSON_GetObjectItem(root, "payload");
    cJSON_ReplaceItemInObject(body, "rendered", cJSON_CreateBool(true));
    cJSON_ReplaceItemInObject(body, "renderedGeneration", cJSON_CreateNumber(7));
    cJSON_ReplaceItemInObject(root, "command", cJSON_CreateString("display.status"));
    char *json = cJSON_PrintUnformatted(root);
    esp_openclaw_display_receipt_t receipt;
    assert(parse(json, "display.status", &receipt) && receipt.rendered && receipt.generation == 7);
    cJSON_free(json);
    cJSON_Delete(root);

    const char *bad_fields[] = {"nodeId", "command", "ok", "payloadJSON"};
    for (size_t i = 0; i < sizeof(bad_fields) / sizeof(bad_fields[0]); ++i) {
        root = envelope(true);
        cJSON_ReplaceItemInObject(root, bad_fields[i], cJSON_CreateString("other"));
        invalid(root);
        root = envelope(true);
        cJSON_AddItemToObject(root, bad_fields[i], cJSON_Duplicate(cJSON_GetObjectItem(root, bad_fields[i]), true));
        invalid(root);
    }
    const char *body_fields[] = {"ready", "heading", "text", "generation", "renderedGeneration", "rendered"};
    for (size_t i = 0; i < sizeof(body_fields) / sizeof(body_fields[0]); ++i) {
        root = envelope(false);
        body = cJSON_GetObjectItem(root, "payload");
        cJSON_DeleteItemFromObject(body, body_fields[i]);
        invalid(root);
        root = envelope(false);
        body = cJSON_GetObjectItem(root, "payload");
        cJSON_AddNullToObject(body, body_fields[i]);
        invalid(root);
    }
    const char *content_fields[] = {"heading", "text"};
    for (size_t i = 0; i < sizeof(content_fields) / sizeof(content_fields[0]); ++i) {
        root = envelope(false);
        cJSON_ReplaceItemInObject(cJSON_GetObjectItem(root, "payload"), content_fields[i], cJSON_CreateString("other"));
        invalid(root);
    }
    const double invalid_generations[] = {-1, 0.5, 4294967296.0};
    for (size_t i = 0; i < sizeof(invalid_generations) / sizeof(invalid_generations[0]); ++i) {
        root = envelope(false);
        body = cJSON_GetObjectItem(root, "payload");
        cJSON_ReplaceItemInObject(body, "generation", cJSON_CreateNumber(invalid_generations[i]));
        invalid(root);
    }
    root = envelope(false);
    body = cJSON_GetObjectItem(root, "payload");
    cJSON_ReplaceItemInObject(body, "rendered", cJSON_CreateBool(true));
    invalid(root);

    root = envelope(true);
    cJSON_AddItemToObject(root, "payload", cJSON_Parse(payload));
    json = cJSON_PrintUnformatted(root);
    assert(parse(json, "display.show", &receipt));
    cJSON_free(json);
    cJSON_ReplaceItemInObject(cJSON_GetObjectItem(root, "payload"), "text", cJSON_CreateString("different"));
    invalid(root);
    root = envelope(true);
    body = cJSON_Parse(payload);
    cJSON_AddNumberToObject(body, "generation", 7);
    cJSON_AddItemToObject(root, "payload", body);
    invalid(root);

    const char *bad_json[] = {
        "{\"ok\":true,\"nodeId\":\"target-node\\u0000other\",\"command\":\"display.show\"}",
        "{\"ok\":true,\"nodeId\":\"target-node\",\"nodeId\":\"other\"}",
        "[]", "{} trailing", "{\"extra\":[[[[[[[[[[[[[[[[]]]]]]]]]]]]]]]]}",
    };
    for (size_t i = 0; i < sizeof(bad_json) / sizeof(bad_json[0]); ++i)
        assert(!parse(bad_json[i], "display.show", &receipt));
    char nul_target[1024];
    snprintf(nul_target, sizeof(nul_target), "{\"ok\":true,\"nodeId\":\"target-node\\u0000other\","
        "\"command\":\"display.show\",\"payload\":%s}", payload);
    assert(!parse(nul_target, "display.show", &receipt));
    root = envelope(false);
    cJSON *nested = cJSON_AddArrayToObject(root, "extra");
    for (int i = 0; i < 16; ++i) {
        cJSON *next = cJSON_CreateArray();
        cJSON_AddItemToArray(nested, next);
        nested = next;
    }
    invalid(root);
    root = envelope(true);
    json = cJSON_PrintUnformatted(root);
    assert(!parse(json, "other.command", &receipt));
    cJSON_free(json);
    cJSON_Delete(root);
    puts("Device control: target, content, generation, rendering and payload envelopes passed");
}
