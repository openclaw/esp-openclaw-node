#include "esp_openclaw_node_handshake.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    const char *name;
    const char *start, *end;
} field_t;

typedef struct {
    const char *next, *end;
} cursor_t;

static void whitespace(cursor_t *cursor)
{
    while (cursor->next < cursor->end && *cursor->next != '\0' && strchr(" \t\r\n", *cursor->next) != NULL)
        ++cursor->next;
}

static int hex(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool hex_quad(cursor_t *cursor, unsigned *value)
{
    *value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (cursor->next == cursor->end) return false;
        int digit = hex(*cursor->next++);
        if (digit < 0) return false;
        *value = (*value << 4) | (unsigned)digit;
    }
    return true;
}

static bool string(cursor_t *cursor)
{
    if (cursor->next == cursor->end || *cursor->next++ != '"') return false;
    while (cursor->next < cursor->end) {
        unsigned char value = (unsigned char)*cursor->next++;
        if (value == '"') return true;
        if (value < 0x20) return false;
        if (value == '\\') {
            if (cursor->next == cursor->end) return false;
            char escape = *cursor->next++;
            if (strchr("\"\\/bfnrt", escape) != NULL && escape != '\0') continue;
            unsigned codepoint;
            if (escape != 'u' || !hex_quad(cursor, &codepoint)) return false;
            if (codepoint == 0) return false;
            if (codepoint >= 0xdc00 && codepoint <= 0xdfff) return false;
            if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                unsigned low;
                if (cursor->end - cursor->next < 2 || cursor->next[0] != '\\' || cursor->next[1] != 'u') return false;
                cursor->next += 2;
                if (!hex_quad(cursor, &low) || low < 0xdc00 || low > 0xdfff) return false;
            }
        } else if (value >= 0x80) {
            unsigned tail = value >= 0xc2 && value <= 0xdf ? 1 :
                value >= 0xe0 && value <= 0xef ? 2 : value >= 0xf0 && value <= 0xf4 ? 3 : 0;
            if (tail == 0 || (size_t)(cursor->end - cursor->next) < tail) return false;
            unsigned char second = (unsigned char)*cursor->next;
            if ((value == 0xe0 && second < 0xa0) || (value == 0xed && second >= 0xa0) ||
                (value == 0xf0 && second < 0x90) || (value == 0xf4 && second >= 0x90)) return false;
            while (tail-- != 0) if (((unsigned char)*cursor->next++ & 0xc0) != 0x80) return false;
        }
    }
    return false;
}

/* Keys may use JSON escapes. Match their decoded ASCII spelling without
 * allocating strings for snapshot keys that will never be retained. */
static bool key_is(const char *start, const char *end, const char *name)
{
    cursor_t key = {.next = start + 1, .end = end - 1};
    while (key.next < key.end && *name != '\0') {
        unsigned value = (unsigned char)*key.next++;
        if (value == '\\') {
            value = (unsigned char)*key.next++;
            if (value == 'u') {
                if (!hex_quad(&key, &value)) return false;
            } else if (value != '"' && value != '\\' && value != '/') return false;
        }
        if (value != (unsigned char)*name++) return false;
    }
    return key.next == key.end && *name == '\0';
}

static bool value(cursor_t *cursor, unsigned depth, field_t *fields, size_t count);

static bool container(cursor_t *cursor, unsigned depth, field_t *fields, size_t count)
{
    bool object = *cursor->next++ == '{';
    char close = object ? '}' : ']';
    whitespace(cursor);
    if (cursor->next < cursor->end && *cursor->next == close) { ++cursor->next; return true; }
    for (;;) {
        const char *key_start = cursor->next, *key_end = cursor->next;
        if (object) {
            if (!string(cursor)) return false;
            key_end = cursor->next;
            whitespace(cursor);
            if (cursor->next == cursor->end || *cursor->next++ != ':') return false;
            whitespace(cursor);
        }
        const char *start = cursor->next;
        if (!value(cursor, depth, NULL, 0)) return false;
        if (object) {
            for (size_t i = 0; i < count; ++i) {
                if (fields[i].start == NULL && key_is(key_start, key_end, fields[i].name)) {
                    fields[i].start = start;
                    fields[i].end = cursor->next;
                    break;
                }
            }
        }
        whitespace(cursor);
        if (cursor->next == cursor->end) return false;
        char separator = *cursor->next++;
        if (separator == close) return true;
        if (separator != ',') return false;
        whitespace(cursor);
    }
}

static bool digit(cursor_t *cursor)
{
    return cursor->next < cursor->end && *cursor->next >= '0' && *cursor->next <= '9';
}

static bool value(cursor_t *cursor, unsigned depth, field_t *fields, size_t count)
{
    if (cursor->next == cursor->end) return false;
    char first = *cursor->next;
    if (first == '{' || first == '[')
        return depth < 64 && container(cursor, depth + 1, fields, count);
    if (first == '"') return string(cursor);
    const char *literal = first == 't' ? "true" : first == 'f' ? "false" : first == 'n' ? "null" : NULL;
    if (literal != NULL) {
        size_t length = strlen(literal);
        if ((size_t)(cursor->end - cursor->next) < length || memcmp(cursor->next, literal, length) != 0) return false;
        cursor->next += length;
        return true;
    }
    if (first == '-') ++cursor->next;
    if (!digit(cursor)) return false;
    if (*cursor->next == '0') ++cursor->next;
    else while (digit(cursor)) ++cursor->next;
    if (cursor->next < cursor->end && *cursor->next == '.') {
        ++cursor->next;
        if (!digit(cursor)) return false;
        while (digit(cursor)) ++cursor->next;
    }
    if (cursor->next < cursor->end && (*cursor->next == 'e' || *cursor->next == 'E')) {
        ++cursor->next;
        if (cursor->next < cursor->end && (*cursor->next == '+' || *cursor->next == '-')) ++cursor->next;
        if (!digit(cursor)) return false;
        while (digit(cursor)) ++cursor->next;
    }
    return true;
}

static cJSON *materialize(field_t *fields, size_t count)
{
    cJSON *object = cJSON_CreateObject();
    if (object == NULL) return NULL;
    for (size_t i = 0; i < count; ++i) {
        if (fields[i].start == NULL) continue;
        cJSON *item;
        if (strcmp(fields[i].name, "payload") == 0 && *fields[i].start == '{') {
            field_t payload[] = {{.name = "type"}, {.name = "nonce"}, {.name = "ts"}, {.name = "auth"}, {.name = "pluginSurfaceUrls"}};
            cursor_t cursor = {.next = fields[i].start, .end = fields[i].end};
            if (!value(&cursor, 0, payload, sizeof(payload) / sizeof(payload[0]))) goto fail;
            item = materialize(payload, sizeof(payload) / sizeof(payload[0]));
        } else {
            item = cJSON_ParseWithLength(fields[i].start, (size_t)(fields[i].end - fields[i].start));
        }
        if (item == NULL) goto fail;
        if (!cJSON_AddItemToObject(object, fields[i].name, item)) { cJSON_Delete(item); goto fail; }
    }
    return object;
fail:
    cJSON_Delete(object);
    return NULL;
}

cJSON *esp_openclaw_node_parse_handshake(const char *json, size_t length)
{
    if (json == NULL || length == 0) return NULL;
    field_t fields[] = {{.name = "type"}, {.name = "id"}, {.name = "ok"}, {.name = "event"}, {.name = "error"}, {.name = "payload"}};
    cursor_t cursor = {.next = json, .end = json + length};
    whitespace(&cursor);
    if (cursor.next == cursor.end || *cursor.next != '{' ||
        !value(&cursor, 0, fields, sizeof(fields) / sizeof(fields[0]))) return NULL;
    whitespace(&cursor);
    if (cursor.next != cursor.end) return NULL;
    return materialize(fields, sizeof(fields) / sizeof(fields[0]));
}

bool esp_openclaw_node_json_strings_valid(const char *json, size_t length)
{
    if (json == NULL) return false;
    cursor_t cursor = {.next = json, .end = json + length};
    while (cursor.next < cursor.end) {
        if (*cursor.next == '"') {
            if (!string(&cursor)) return false;
        } else {
            if (*cursor.next++ == '\0') return false;
        }
    }
    return true;
}
