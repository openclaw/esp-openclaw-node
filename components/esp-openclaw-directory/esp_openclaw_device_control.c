#include "esp_openclaw_device_control.h"
#include "esp_openclaw_directory_json.h"

#include <string.h>

static const cJSON *field(const cJSON *object, const char *name, bool *valid)
{
    const cJSON *found = NULL, *item;
    cJSON_ArrayForEach(item, object) {
        if (item->string != NULL && strcmp(item->string, name) == 0) {
            if (found != NULL) *valid = false;
            found = item;
        }
    }
    return found;
}

static bool string_is(const cJSON *value, const char *expected)
{
    return cJSON_IsString(value) && strcmp(value->valuestring, expected) == 0;
}

static bool uint32_value(const cJSON *value, uint32_t *out)
{
    if (!cJSON_IsNumber(value) || !(value->valuedouble >= 0 && value->valuedouble <= UINT32_MAX) ||
        value->valuedouble != (uint32_t)value->valuedouble) return false;
    *out = (uint32_t)value->valuedouble;
    return true;
}

static bool payload_receipt(const cJSON *payload, const char *heading, const char *text,
    esp_openclaw_display_receipt_t *result)
{
    bool valid = cJSON_IsObject(payload);
    const cJSON *ready = field(payload, "ready", &valid);
    const cJSON *returned_heading = field(payload, "heading", &valid);
    const cJSON *returned_text = field(payload, "text", &valid);
    const cJSON *generation = field(payload, "generation", &valid);
    const cJSON *rendered_generation = field(payload, "renderedGeneration", &valid);
    const cJSON *rendered = field(payload, "rendered", &valid);
    valid = valid && cJSON_IsTrue(ready) && string_is(returned_heading, heading) &&
        string_is(returned_text, text) && cJSON_IsBool(rendered) &&
        uint32_value(generation, &result->generation) && uint32_value(rendered_generation, &result->rendered_generation);
    result->rendered = cJSON_IsTrue(rendered);
    return valid && (!result->rendered || result->rendered_generation == result->generation);
}

bool esp_openclaw_display_receipt_parse(const char *json, size_t length,
    const char *node_id, const char *command, const char *heading, const char *text,
    esp_openclaw_display_receipt_t *receipt)
{
    if (receipt == NULL) return false;
    *receipt = (esp_openclaw_display_receipt_t){0};
    if (node_id == NULL || command == NULL || heading == NULL || text == NULL ||
        (strcmp(command, "display.show") != 0 && strcmp(command, "display.status") != 0)) return false;
    esp_openclaw_directory_result_t parsed;
    cJSON *root = esp_openclaw_directory_parse_payload(json, length, &parsed), *decoded = NULL;
    bool valid = cJSON_IsObject(root);
    const cJSON *ok = field(root, "ok", &valid);
    const cJSON *id = field(root, "nodeId", &valid);
    const cJSON *method = field(root, "command", &valid);
    const cJSON *payload = field(root, "payload", &valid);
    const cJSON *payload_json = field(root, "payloadJSON", &valid);
    valid = valid && cJSON_IsTrue(ok) && string_is(id, node_id) && string_is(method, command);
    if (payload_json != NULL && !cJSON_IsNull(payload_json)) {
        if (!cJSON_IsString(payload_json)) valid = false;
        else decoded = esp_openclaw_directory_parse_payload(payload_json->valuestring,
            strlen(payload_json->valuestring), &parsed);
        if (decoded == NULL) valid = false;
        if (payload != NULL && !cJSON_IsNull(payload)) {
            esp_openclaw_display_receipt_t other = {0};
            if (!payload_receipt(payload, heading, text, &other) ||
                !cJSON_Compare(payload, decoded, true)) valid = false;
        }
        payload = decoded;
    }
    esp_openclaw_display_receipt_t result = {0};
    valid = valid && payload_receipt(payload, heading, text, &result);
    if (valid) *receipt = result;
    cJSON_Delete(decoded);
    cJSON_Delete(root);
    return valid;
}
