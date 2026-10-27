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

static cJSON *parse_payload(const char *json, size_t length,
                            esp_openclaw_directory_result_t *result)
{
    *result = ESP_OPENCLAW_DIRECTORY_INVALID;
    if (length > ESP_OPENCLAW_DIRECTORY_MAX_JSON_BYTES) {
        *result = ESP_OPENCLAW_DIRECTORY_TOO_LARGE;
        return NULL;
    }
    if (json == NULL || length == 0 || !valid_utf8(json, length) ||
        !bounded_depth(json, length)) return NULL;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &end, false);
    while (end != NULL && end < json + length &&
           (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t')) ++end;
    if (!cJSON_IsObject(root) || end != json + length) {
        cJSON_Delete(root);
        return NULL;
    }
    *result = ESP_OPENCLAW_DIRECTORY_OK;
    return root;
}

esp_openclaw_directory_result_t esp_openclaw_directory_parse(
    const char *json, size_t length, size_t offset, esp_openclaw_agent_page_t *page)
{
    if (page == NULL) return ESP_OPENCLAW_DIRECTORY_INVALID;
    memset(page, 0, sizeof(*page));
    esp_openclaw_directory_result_t result;
    cJSON *root = parse_payload(json, length, &result);
    if (root == NULL) return result;
    const cJSON *agents = cJSON_GetObjectItemCaseSensitive(root, "agents");
    bool valid = cJSON_IsArray(agents);
    const char *main_key = string_field(root, "mainKey");
    if (main_key != NULL && strlen(main_key) < sizeof(page->main_key)) {
        memcpy(page->main_key, main_key, strlen(main_key) + 1);
    }
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

static bool page_number(const cJSON *value, size_t *number)
{
    /* Bound offsets independently of the target's size_t width. */
    if (!cJSON_IsNumber(value) || !(value->valuedouble >= 0) ||
        !(value->valuedouble <= 2147483647.0)) return false;
    *number = (size_t)value->valuedouble;
    return (double)*number == value->valuedouble;
}

static bool session_key(const char *key, const char *agent_id)
{
    if (key == NULL || strncmp(key, "agent:", 6) != 0) return false;
    size_t length = strlen(agent_id);
    return strncmp(key + 6, agent_id, length) == 0 &&
        key[6 + length] == ':' && key[7 + length] != '\0';
}

esp_openclaw_directory_result_t esp_openclaw_sessions_parse(
    const char *json, size_t length, const char *agent_id, size_t offset,
    esp_openclaw_session_page_t *page)
{
    if (page == NULL) return ESP_OPENCLAW_DIRECTORY_INVALID;
    memset(page, 0, sizeof(*page));
    if (!valid_id(agent_id) || offset > 2147483647u) return ESP_OPENCLAW_DIRECTORY_INVALID;
    esp_openclaw_directory_result_t result;
    cJSON *root = parse_payload(json, length, &result);
    if (root == NULL) return result;
    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(root, "sessions");
    bool valid = cJSON_IsArray(rows);
    size_t returned_offset = offset;
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(root, "offset");
    if (field != NULL && (!page_number(field, &returned_offset) || returned_offset != offset)) valid = false;
    const cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        const char *key = string_field(row, "key");
        const char *owner = string_field(row, "agentId");
        if (!session_key(key, agent_id) || (owner != NULL && strcmp(owner, agent_id) != 0)) {
            valid = false;
            break;
        }
        if (strlen(key) >= ESP_OPENCLAW_SESSION_KEY_BYTES ||
            page->count >= ESP_OPENCLAW_DIRECTORY_PAGE_SIZE) {
            result = ESP_OPENCLAW_DIRECTORY_TOO_LARGE;
            valid = false;
            break;
        }
        for (size_t i = 0; i < page->count; ++i) {
            if (strcmp(page->sessions[i].key, key) == 0) valid = false;
        }
        if (!valid) break;
        esp_openclaw_session_t *session = &page->sessions[page->count++];
        memcpy(session->key, key, strlen(key) + 1);
        const char *title = string_field(row, "label");
        if (title == NULL) title = string_field(row, "displayName");
        if (title == NULL) title = string_field(row, "derivedTitle");
        copy_label(session->title, sizeof(session->title), title != NULL ? title : key);
        const char *message = string_field(row, "lastMessagePreview");
        if (message != NULL) copy_label(session->last_message, sizeof(session->last_message), message);
    }
    const cJSON *more = cJSON_GetObjectItemCaseSensitive(root, "hasMore");
    const cJSON *next = cJSON_GetObjectItemCaseSensitive(root, "nextOffset");
    if (more != NULL && !cJSON_IsBool(more)) valid = false;
    page->has_more = cJSON_IsTrue(more);
    if (page->has_more) {
        if (page->count == 0 || !page_number(next, &page->next_offset) ||
            page->next_offset != offset + page->count) valid = false;
    } else if (next != NULL && !cJSON_IsNull(next)) valid = false;
    page->offset = offset;
    cJSON_Delete(root);
    if (!valid) {
        memset(page, 0, sizeof(*page));
        return result == ESP_OPENCLAW_DIRECTORY_TOO_LARGE ? result : ESP_OPENCLAW_DIRECTORY_INVALID;
    }
    return ESP_OPENCLAW_DIRECTORY_OK;
}

esp_openclaw_directory_result_t esp_openclaw_preview_parse(
    const char *json, size_t length, const char *key,
    esp_openclaw_session_preview_t *preview)
{
    if (preview == NULL) return ESP_OPENCLAW_DIRECTORY_INVALID;
    memset(preview, 0, sizeof(*preview));
    preview->status = ESP_OPENCLAW_PREVIEW_ERROR;
    if (key == NULL || key[0] == '\0' || strlen(key) >= ESP_OPENCLAW_SESSION_KEY_BYTES)
        return ESP_OPENCLAW_DIRECTORY_INVALID;
    esp_openclaw_directory_result_t result;
    cJSON *root = parse_payload(json, length, &result);
    if (root == NULL) return result;
    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(root, "previews");
    const cJSON *row = cJSON_GetArrayItem(rows, 0);
    const char *returned_key = string_field(row, "key");
    const char *status = string_field(row, "status");
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(row, "items");
    bool valid = cJSON_IsArray(rows) && cJSON_GetArraySize(rows) == 1 &&
        returned_key != NULL && strcmp(returned_key, key) == 0 && status != NULL && cJSON_IsArray(items);
    if (valid) {
        if (strcmp(status, "ok") == 0) preview->status = ESP_OPENCLAW_PREVIEW_OK;
        else if (strcmp(status, "empty") == 0) preview->status = ESP_OPENCLAW_PREVIEW_EMPTY;
        else if (strcmp(status, "missing") == 0) preview->status = ESP_OPENCLAW_PREVIEW_MISSING;
        else if (strcmp(status, "error") != 0) valid = false;
    }
    const char *roles[] = {"user", "assistant", "tool", "system", "other"};
    const cJSON *item;
    cJSON_ArrayForEach(item, items) {
        if (!valid) break;
        if (preview->count >= ESP_OPENCLAW_PREVIEW_ITEMS) {
            result = ESP_OPENCLAW_DIRECTORY_TOO_LARGE;
            valid = false;
            break;
        }
        const char *role = string_field(item, "role");
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(item, "text");
        if (role == NULL || !cJSON_IsString(text)) { valid = false; break; }
        size_t index = 0;
        while (index < 5 && strcmp(role, roles[index]) != 0) ++index;
        if (index == 5) { valid = false; break; }
        esp_openclaw_preview_item_t *entry = &preview->items[preview->count++];
        entry->role = (esp_openclaw_preview_role_t)index;
        copy_label(entry->text, sizeof(entry->text), text->valuestring);
    }
    if ((preview->status == ESP_OPENCLAW_PREVIEW_OK) != (preview->count > 0)) valid = false;
    cJSON_Delete(root);
    if (!valid) {
        memset(preview, 0, sizeof(*preview));
        preview->status = ESP_OPENCLAW_PREVIEW_ERROR;
        return result == ESP_OPENCLAW_DIRECTORY_TOO_LARGE ? result : ESP_OPENCLAW_DIRECTORY_INVALID;
    }
    return ESP_OPENCLAW_DIRECTORY_OK;
}
