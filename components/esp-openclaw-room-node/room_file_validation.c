#include "room_file_validation.h"

#include <string.h>

bool room_file_normalize_public_path(
    const char *root,
    const char *path,
    char *out,
    size_t out_size)
{
    if (root == NULL || root[0] != '/' || path == NULL || path[0] != '/' ||
        out == NULL || out_size == 0 || strlen(path) >= out_size ||
        strstr(path, "//") != NULL || strstr(path, "/./") != NULL ||
        strstr(path, "/../") != NULL) {
        return false;
    }
    size_t len = strlen(path);
    if ((len >= 2 && strcmp(path + len - 2, "/.") == 0) ||
        (len >= 3 && strcmp(path + len - 3, "/..") == 0)) {
        return false;
    }
    memcpy(out, path, len + 1U);
    while (len > 1 && out[len - 1] == '/') out[--len] = '\0';
    size_t root_len = strlen(root);
    return strncmp(out, root, root_len) == 0 &&
        (out[root_len] == '\0' || out[root_len] == '/');
}

static int base64_value(unsigned char ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+' || ch == '-') return 62;
    if (ch == '/' || ch == '_') return 63;
    return -1;
}

bool room_file_inspect_base64(const char *value, size_t *decoded_len)
{
    if (value == NULL || decoded_len == NULL) return false;
    size_t data_chars = 0;
    size_t padding = 0;
    bool saw_padding = false;
    int last_value = 0;
    for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; ++p) {
        if (*p == '=') {
            if (++padding > 2) return false;
            saw_padding = true;
            continue;
        }
        last_value = base64_value(*p);
        if (saw_padding || last_value < 0) return false;
        ++data_chars;
    }
    if (data_chars == 0) {
        *decoded_len = 0;
        return padding == 0;
    }
    size_t remainder = data_chars % 4U;
    if (padding == 0) {
        if (remainder == 1) return false;
    } else if (data_chars + padding < 4 || (data_chars + padding) % 4U != 0 ||
               (padding == 1 && remainder != 3) || (padding == 2 && remainder != 2)) {
        return false;
    }
    if ((remainder == 2 && (last_value & 0x0f) != 0) ||
        (remainder == 3 && (last_value & 0x03) != 0)) return false;
    *decoded_len = (data_chars * 3U) / 4U;
    return true;
}

bool room_file_mutation_allowed(bool preflight_only)
{
    return !preflight_only;
}

char *room_file_parent_walk_start(char *parent, const char *root)
{
    if (parent == NULL || root == NULL) return NULL;
    size_t root_len = strlen(root);
    if (strlen(parent) <= root_len) return NULL;
    return parent + root_len + 1;
}

