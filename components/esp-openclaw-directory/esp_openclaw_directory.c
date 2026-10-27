#include "esp_openclaw_directory.h"

#include <stdbool.h>
#include <string.h>

#include "cJSON.h"

static const char *string_field(const cJSON *object, const char *name)
{
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(field) && field->valuestring[0] != '\0' ? field->valuestring : NULL;
}

static bool valid_id(const char *id)
{
    if (id == NULL || strlen(id) > 64) return false;
    for (size_t i = 0; id[i] != '\0'; ++i) {
        bool alphanumeric = (id[i] >= 'a' && id[i] <= 'z') || (id[i] >= '0' && id[i] <= '9');
        if (!alphanumeric && (i == 0 || (id[i] != '_' && id[i] != '-'))) return false;
    }
    return id[0] != '\0';
}

static void copy_label(char *out, size_t capacity, const char *text)
{
    size_t length = strlen(text);
    if (length >= capacity) {
        length = capacity - 1;
        while (length > 0 && ((unsigned char)text[length] & 0xc0) == 0x80) --length;
    }
    memcpy(out, text, length);
    out[length] = '\0';
}

static bool valid_utf8(const char *text, size_t length)
{
    for (size_t i = 0; i < length;) {
        unsigned char first = (unsigned char)text[i++];
        if (first == 0) return false;
        if (first < 0x80) continue;
        size_t tail = first >= 0xc2 && first <= 0xdf ? 1 :
            first >= 0xe0 && first <= 0xef ? 2 : first >= 0xf0 && first <= 0xf4 ? 3 : 0;
        if (tail == 0 || tail > length - i) return false;
        unsigned char second = (unsigned char)text[i];
        if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0) ||
            (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90)) return false;
        for (size_t j = 0; j < tail; ++j) {
            if (((unsigned char)text[i++] & 0xc0) != 0x80) return false;
        }
    }
    return true;
}

static bool bounded_depth(const char *json, size_t length)
{
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < length; ++i) {
        char c = json[i];
        if (quoted) {
            if ((unsigned char)c < 0x20) return false;
            if (escaped) {
                /* cJSON decodes this into a C-string terminator, losing the
                 * suffix of an identifier or label. Reject before decoding. */
                if (c == 'u') {
                    if (length - i <= 4) return false;
                    for (size_t j = 1; j <= 4; ++j) {
                        char digit = json[i + j];
                        if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f') ||
                              (digit >= 'A' && digit <= 'F'))) return false;
                    }
                    if (memcmp(json + i + 1, "0000", 4) == 0) return false;
                    i += 4;
                }
                escaped = false;
            }
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 16) return false; }
        else if (c == '}' || c == ']') { if (depth == 0) return false; --depth; }
    }
    return depth == 0 && !quoted;
}

esp_openclaw_directory_result_t esp_openclaw_directory_parse(
    const char *json, size_t length, size_t offset, esp_openclaw_agent_page_t *page)
{
    if (page == NULL) return ESP_OPENCLAW_DIRECTORY_INVALID;
    memset(page, 0, sizeof(*page));
    if (length > ESP_OPENCLAW_DIRECTORY_MAX_JSON_BYTES) return ESP_OPENCLAW_DIRECTORY_TOO_LARGE;
    if (json == NULL || length == 0) return ESP_OPENCLAW_DIRECTORY_INVALID;
    if (!valid_utf8(json, length) || !bounded_depth(json, length)) return ESP_OPENCLAW_DIRECTORY_INVALID;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &end, false);
    while (end != NULL && end < json + length && (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t')) ++end;
    const cJSON *agents = cJSON_GetObjectItemCaseSensitive(root, "agents");
    bool valid = cJSON_IsObject(root) && cJSON_IsArray(agents) && end == json + length;
    size_t index = 0;
    const cJSON *row;
    cJSON_ArrayForEach(row, agents) {
        const char *id = string_field(row, "id");
        if (!valid_id(id)) { valid = false; break; }
        /* Ambiguous identities must not become two different picker rows. */
        for (const cJSON *previous = agents->child; previous != row; previous = previous->next) {
            const char *previous_id = string_field(previous, "id");
            if (previous_id != NULL && strcmp(previous_id, id) == 0) valid = false;
        }
        if (!valid) break;
        if (index >= offset && page->count < ESP_OPENCLAW_DIRECTORY_PAGE_SIZE) {
            esp_openclaw_agent_t *agent = &page->agents[page->count++];
            const cJSON *identity = cJSON_GetObjectItemCaseSensitive(row, "identity");
            const char *name = string_field(identity, "name");
            if (name == NULL) name = string_field(row, "name");
            copy_label(agent->id, sizeof(agent->id), id);
            copy_label(agent->name, sizeof(agent->name), name != NULL ? name : id);
            const char *emoji = string_field(identity, "emoji");
            if (emoji != NULL) copy_label(agent->emoji, sizeof(agent->emoji), emoji);
        }
        ++index;
    }
    page->offset = offset;
    page->total = index;
    if (offset > index) valid = false;
    cJSON_Delete(root);
    if (!valid) {
        memset(page, 0, sizeof(*page));
        return ESP_OPENCLAW_DIRECTORY_INVALID;
    }
    return ESP_OPENCLAW_DIRECTORY_OK;
}
