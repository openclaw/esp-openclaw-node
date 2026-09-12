#include "room_files.h"
#include "room_file_validation.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_check.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"

#define TAG "room_files"
#define FILE_LIMIT (1024U * 1024U)
#define DIR_LIMIT 256

static char configured_root[PATH_MAX];
static char canonical_root[PATH_MAX];

static int room_lstat(const char *path, struct stat *st)
{
#if defined(ESP_PLATFORM)
    /* ESP-IDF 5.5 VFS exposes stat but not lstat. FAT has no symlinks; a
     * symlink-capable host/VFS build takes the no-follow branch below. */
    return stat(path, st);
#else
    return lstat(path, st);
#endif
}

static esp_err_t fail(
    esp_openclaw_node_error_t *error,
    const char *code,
    const char *message,
    esp_err_t err)
{
    error->code = code;
    error->message = message;
    return err;
}

static bool has_only_fields(cJSON *object, const char *const *allowed, size_t count)
{
    for (cJSON *field = object->child; field != NULL; field = field->next) {
        bool found = false;
        for (size_t i = 0; i < count; ++i) {
            if (field->string != NULL && strcmp(field->string, allowed[i]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

static cJSON *parse_object(
    const char *json,
    size_t len,
    esp_openclaw_node_error_t *error)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        fail(error, "INVALID_PARAMS", "params must be a JSON object", ESP_ERR_INVALID_ARG);
        return NULL;
    }
    return root;
}

static bool root_contains(const char *path)
{
    size_t root_len = strlen(canonical_root);
    return strncmp(path, canonical_root, root_len) == 0 &&
        (path[root_len] == '\0' || path[root_len] == '/');
}

static bool path_has_symlink(const char *path)
{
    size_t root_len = strlen(configured_root);
    if (strncmp(path, configured_root, root_len) != 0) return true;
    char current[PATH_MAX];
    strlcpy(current, configured_root, sizeof(current));
    const char *part = path + root_len;
    while (*part == '/') ++part;
    while (*part != '\0') {
        const char *slash = strchr(part, '/');
        size_t length = slash != NULL ? (size_t)(slash - part) : strlen(part);
        size_t used = strlen(current);
        if (used + 1 + length >= sizeof(current)) return true;
        current[used++] = '/';
        memcpy(current + used, part, length);
        current[used + length] = '\0';
        struct stat st;
        if (room_lstat(current, &st) == 0 && S_ISLNK(st.st_mode)) return true;
        if (slash == NULL) break;
        part = slash + 1;
    }
    return false;
}

static esp_err_t resolve_read_path(
    const char *requested,
    bool follow_symlinks,
    char canonical[PATH_MAX],
    struct stat *st,
    esp_openclaw_node_error_t *error)
{
    char normalized[PATH_MAX];
    if (!room_file_normalize_public_path(
            configured_root, requested, normalized, sizeof(normalized))) {
        return fail(error, "INVALID_PATH", "path must be canonical and inside the configured root", ESP_ERR_INVALID_ARG);
    }
    struct stat lst;
    if (room_lstat(normalized, &lst) != 0) {
        return fail(error, "NOT_FOUND", "path not found", ESP_ERR_NOT_FOUND);
    }
    if (!follow_symlinks && path_has_symlink(normalized)) {
        (void)realpath(normalized, canonical);
        return fail(error, "SYMLINK_REDIRECT", "path traverses a symlink while followSymlinks is false", ESP_ERR_INVALID_ARG);
    }
    if (realpath(normalized, canonical) == NULL || !root_contains(canonical)) {
        return fail(error, "PATH_TRAVERSAL", "canonical path escapes the configured root", ESP_ERR_INVALID_ARG);
    }
    if (stat(canonical, st) != 0) {
        return fail(error, "READ_ERROR", "canonical path could not be inspected", ESP_FAIL);
    }
    return ESP_OK;
}

static const char *mime_for_path(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == NULL) return "application/octet-stream";
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(dot, ".png") == 0) return "image/png";
    if (strcasecmp(dot, ".json") == 0) return "application/json";
    if (strcasecmp(dot, ".txt") == 0 || strcasecmp(dot, ".md") == 0) return "text/plain";
    return "application/octet-stream";
}

static void sha256_hex(const uint8_t *data, size_t len, char out[65])
{
    uint8_t digest[32];
    mbedtls_sha256(data, len, digest, 0);
    for (size_t i = 0; i < sizeof(digest); ++i) snprintf(out + i * 2, 3, "%02x", digest[i]);
}

static esp_err_t finish_result(
    cJSON *result,
    cJSON *params,
    char **out,
    esp_openclaw_node_error_t *error)
{
    *out = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    cJSON_Delete(params);
    return *out != NULL ? ESP_OK
                        : fail(error, "INTERNAL", "not enough memory", ESP_ERR_NO_MEM);
}

static esp_err_t dir_list(
    esp_openclaw_node_handle_t node, void *context, const char *params_json,
    size_t params_len, char **out, esp_openclaw_node_error_t *error)
{
    (void)node;
    (void)context;
    cJSON *params = parse_object(params_json, params_len, error);
    if (params == NULL) return ESP_ERR_INVALID_ARG;
    static const char *const fields[] = {
        "path", "pageToken", "maxEntries", "followSymlinks", "preflightOnly",
    };
    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(params, "path");
    cJSON *max_item = cJSON_GetObjectItemCaseSensitive(params, "maxEntries");
    cJSON *page_item = cJSON_GetObjectItemCaseSensitive(params, "pageToken");
    cJSON *follow_item = cJSON_GetObjectItemCaseSensitive(params, "followSymlinks");
    cJSON *preflight_item = cJSON_GetObjectItemCaseSensitive(params, "preflightOnly");
    bool types_ok = cJSON_IsString(path_item) && path_item->valuestring != NULL &&
        (max_item == NULL || (cJSON_IsNumber(max_item) && max_item->valuedouble == (double)max_item->valueint)) &&
        (page_item == NULL || (cJSON_IsString(page_item) && page_item->valuestring != NULL)) &&
        (follow_item == NULL || cJSON_IsBool(follow_item)) &&
        (preflight_item == NULL || cJSON_IsBool(preflight_item));
    if (!has_only_fields(params, fields, sizeof(fields) / sizeof(fields[0])) || !types_ok) {
        cJSON_Delete(params);
        return fail(error, "INVALID_PARAMS", "dir.list has unknown fields or wrong field types", ESP_ERR_INVALID_ARG);
    }
    int max_entries = max_item != NULL ? max_item->valueint : 200;
    if (max_entries < 1) max_entries = 200;
    if (max_entries > DIR_LIMIT) max_entries = DIR_LIMIT;
    long offset = 0;
    if (page_item != NULL) {
        char *end = NULL;
        offset = strtol(page_item->valuestring, &end, 10);
        if (end == page_item->valuestring || *end != '\0' || offset < 0) offset = 0;
    }
    char canonical[PATH_MAX] = {0};
    struct stat st;
    esp_err_t err = resolve_read_path(
        path_item->valuestring, cJSON_IsTrue(follow_item), canonical, &st, error);
    if (err != ESP_OK) {
        cJSON_Delete(params);
        return err;
    }
    if (!S_ISDIR(st.st_mode)) {
        cJSON_Delete(params);
        return fail(error, "IS_FILE", "path is not a directory", ESP_ERR_INVALID_ARG);
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddStringToObject(result, "path", canonical);
    cJSON *entries = cJSON_AddArrayToObject(result, "entries");
    if (cJSON_IsTrue(preflight_item)) {
        cJSON_AddBoolToObject(result, "truncated", false);
        cJSON_AddBoolToObject(result, "preflightOnly", true);
        return finish_result(result, params, out, error);
    }
    DIR *dir = opendir(canonical);
    if (dir == NULL) {
        cJSON_Delete(result);
        cJSON_Delete(params);
        return fail(error, "READ_ERROR", "directory could not be opened", ESP_FAIL);
    }
    struct dirent *entry;
    long seen = 0;
    int added = 0;
    bool truncated = false;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (seen++ < offset) continue;
        if (added >= max_entries) {
            truncated = true;
            break;
        }
        char child[PATH_MAX];
        if (snprintf(child, sizeof(child), "%s/%s", canonical, entry->d_name) >= (int)sizeof(child)) continue;
        struct stat child_st;
        if (room_lstat(child, &child_st) != 0) continue;
        bool is_dir = S_ISDIR(child_st.st_mode);
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", entry->d_name);
        cJSON_AddStringToObject(item, "path", child);
        cJSON_AddNumberToObject(item, "size", is_dir ? 0 : (double)child_st.st_size);
        cJSON_AddStringToObject(item, "mimeType", is_dir ? "inode/directory" : mime_for_path(entry->d_name));
        cJSON_AddBoolToObject(item, "isDir", is_dir);
        cJSON_AddNumberToObject(item, "mtime", (double)child_st.st_mtime * 1000.0);
        cJSON_AddItemToArray(entries, item);
        ++added;
    }
    closedir(dir);
    cJSON_AddBoolToObject(result, "truncated", truncated);
    if (truncated) {
        char token[24];
        snprintf(token, sizeof(token), "%ld", offset + added);
        cJSON_AddStringToObject(result, "nextPageToken", token);
    }
    return finish_result(result, params, out, error);
}

static esp_err_t file_fetch(
    esp_openclaw_node_handle_t node, void *context, const char *params_json,
    size_t params_len, char **out, esp_openclaw_node_error_t *error)
{
    (void)node;
    (void)context;
    cJSON *params = parse_object(params_json, params_len, error);
    if (params == NULL) return ESP_ERR_INVALID_ARG;
    static const char *const fields[] = {"path", "maxBytes", "followSymlinks", "preflightOnly"};
    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(params, "path");
    cJSON *max_item = cJSON_GetObjectItemCaseSensitive(params, "maxBytes");
    cJSON *follow_item = cJSON_GetObjectItemCaseSensitive(params, "followSymlinks");
    cJSON *preflight_item = cJSON_GetObjectItemCaseSensitive(params, "preflightOnly");
    bool types_ok = cJSON_IsString(path_item) && path_item->valuestring != NULL &&
        (max_item == NULL || (cJSON_IsNumber(max_item) && max_item->valuedouble == (double)max_item->valueint && max_item->valueint > 0)) &&
        (follow_item == NULL || cJSON_IsBool(follow_item)) &&
        (preflight_item == NULL || cJSON_IsBool(preflight_item));
    if (!has_only_fields(params, fields, sizeof(fields) / sizeof(fields[0])) || !types_ok) {
        cJSON_Delete(params);
        return fail(error, "INVALID_PARAMS", "file.fetch has unknown fields or wrong field types", ESP_ERR_INVALID_ARG);
    }
    size_t requested_max = max_item != NULL ? (size_t)max_item->valuedouble : 8U * 1024U * 1024U;
    size_t max_bytes = requested_max < FILE_LIMIT ? requested_max : FILE_LIMIT;
    char canonical[PATH_MAX] = {0};
    struct stat st;
    esp_err_t err = resolve_read_path(
        path_item->valuestring, cJSON_IsTrue(follow_item), canonical, &st, error);
    if (err != ESP_OK) {
        cJSON_Delete(params);
        return err;
    }
    if (!S_ISREG(st.st_mode)) {
        cJSON_Delete(params);
        return fail(error, "IS_DIRECTORY", "path is a directory", ESP_ERR_INVALID_ARG);
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > max_bytes) {
        cJSON_Delete(params);
        return fail(error, "FILE_TOO_LARGE", "file exceeds the effective 1 MiB node limit", ESP_ERR_INVALID_SIZE);
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddStringToObject(result, "path", canonical);
    cJSON_AddNumberToObject(result, "size", (double)st.st_size);
    if (cJSON_IsTrue(preflight_item)) {
        cJSON_AddStringToObject(result, "mimeType", "");
        cJSON_AddStringToObject(result, "base64", "");
        cJSON_AddStringToObject(result, "sha256", "");
        cJSON_AddBoolToObject(result, "preflightOnly", true);
        return finish_result(result, params, out, error);
    }
    int fd = open(canonical, O_RDONLY
#ifdef O_NOFOLLOW
        | O_NOFOLLOW
#endif
    );
    uint8_t *bytes = fd >= 0 ? malloc((size_t)st.st_size + 1U) : NULL;
    ssize_t read_len = bytes != NULL ? read(fd, bytes, (size_t)st.st_size) : -1;
    if (fd >= 0) close(fd);
    if (bytes == NULL || read_len != st.st_size) {
        free(bytes);
        cJSON_Delete(result);
        cJSON_Delete(params);
        return fail(error, "READ_ERROR", "file could not be read", ESP_FAIL);
    }
    size_t b64_size = ((size_t)st.st_size + 2U) / 3U * 4U + 1U;
    unsigned char *b64 = malloc(b64_size);
    size_t written = 0;
    char hash[65] = {0};
    sha256_hex(bytes, (size_t)st.st_size, hash);
    int rc = b64 != NULL ? mbedtls_base64_encode(b64, b64_size, &written, bytes, (size_t)st.st_size) : -1;
    free(bytes);
    if (rc != 0) {
        free(b64);
        cJSON_Delete(result);
        cJSON_Delete(params);
        return fail(error, "INTERNAL", "base64 encoding failed", ESP_ERR_NO_MEM);
    }
    b64[written] = '\0';
    cJSON_AddStringToObject(result, "mimeType", mime_for_path(canonical));
    cJSON_AddStringToObject(result, "base64", (char *)b64);
    cJSON_AddStringToObject(result, "sha256", hash);
    free(b64);
    return finish_result(result, params, out, error);
}

static esp_err_t decode_base64(
    const char *encoded,
    uint8_t **out,
    size_t *out_len,
    esp_openclaw_node_error_t *error)
{
    size_t decoded_len = 0;
    if (!room_file_inspect_base64(encoded, &decoded_len)) {
        return fail(error, "INVALID_BASE64", "contentBase64 is not valid base64", ESP_ERR_INVALID_ARG);
    }
    if (decoded_len > FILE_LIMIT) {
        return fail(error, "FILE_TOO_LARGE", "decoded content exceeds the 1 MiB node limit", ESP_ERR_INVALID_SIZE);
    }
    size_t encoded_len = strlen(encoded);
    size_t padded_len = (encoded_len + 3U) & ~3U;
    unsigned char *normalized = calloc(padded_len + 1U, 1);
    uint8_t *bytes = malloc(decoded_len + 1U);
    if (normalized == NULL || bytes == NULL) {
        free(normalized);
        free(bytes);
        return fail(error, "INTERNAL", "not enough memory", ESP_ERR_NO_MEM);
    }
    for (size_t i = 0; i < encoded_len; ++i) {
        normalized[i] = encoded[i] == '-' ? '+' : encoded[i] == '_' ? '/' : (unsigned char)encoded[i];
    }
    for (size_t i = encoded_len; i < padded_len; ++i) normalized[i] = '=';
    size_t actual = 0;
    int rc = decoded_len == 0 ? 0 : mbedtls_base64_decode(
        bytes, decoded_len + 1U, &actual, normalized, padded_len);
    free(normalized);
    if (rc != 0 || actual != decoded_len) {
        free(bytes);
        return fail(error, "INVALID_BASE64", "contentBase64 is not valid base64", ESP_ERR_INVALID_ARG);
    }
    *out = bytes;
    *out_len = actual;
    return ESP_OK;
}

static bool valid_sha256(const char *value)
{
    if (value == NULL || strlen(value) != 64) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; ++p) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
              (*p >= 'A' && *p <= 'F'))) return false;
    }
    return true;
}

static bool make_parents_safely(char *parent)
{
    char *walk = room_file_parent_walk_start(parent, configured_root);
    if (walk != NULL) {
        for (char *p = walk; *p != '\0'; ++p) {
            if (*p != '/') continue;
            *p = '\0';
            struct stat st;
            if (room_lstat(parent, &st) != 0) {
                if (mkdir(parent, 0755) != 0) return false;
            } else if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
                return false;
            }
            *p = '/';
        }
    }
    struct stat st;
    if (room_lstat(parent, &st) != 0) return mkdir(parent, 0755) == 0;
    return S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
}

static esp_err_t resolve_write_path(
    const char *requested,
    bool follow_symlinks,
    bool create_parents,
    bool preflight,
    char target[PATH_MAX],
    bool *exists,
    esp_openclaw_node_error_t *error)
{
    char normalized[PATH_MAX];
    if (!room_file_normalize_public_path(
            configured_root, requested, normalized, sizeof(normalized)) ||
        strcmp(normalized, configured_root) == 0) {
        return fail(error, "INVALID_PATH", "destination must be a file inside the configured root", ESP_ERR_INVALID_ARG);
    }
    char parent[PATH_MAX];
    strlcpy(parent, normalized, sizeof(parent));
    char *slash = strrchr(parent, '/');
    *slash = '\0';
    if (!follow_symlinks && path_has_symlink(parent)) {
        return fail(error, "SYMLINK_REDIRECT", "parent path traverses a symlink", ESP_ERR_INVALID_ARG);
    }
    struct stat parent_st;
    bool parent_exists = room_lstat(parent, &parent_st) == 0;
    if (!parent_exists) {
        if (!create_parents) {
            return fail(error, "PARENT_NOT_FOUND", "parent directory does not exist", ESP_ERR_NOT_FOUND);
        }
        if (room_file_mutation_allowed(preflight)) {
            char parent_copy[PATH_MAX];
            strlcpy(parent_copy, parent, sizeof(parent_copy));
            if (!make_parents_safely(parent_copy)) {
                return fail(error, "WRITE_ERROR", "failed to create safe parent directories", ESP_FAIL);
            }
            if (room_lstat(parent, &parent_st) != 0) {
                return fail(error, "WRITE_ERROR", "created parent could not be inspected", ESP_FAIL);
            }
        }
    }
    char resolved_parent[PATH_MAX];
    if (realpath(parent, resolved_parent) == NULL) {
        if (!preflight) {
            return fail(error, "PARENT_NOT_FOUND", "parent directory does not exist", ESP_ERR_NOT_FOUND);
        }
        /* Side-effect-free preflight: derive from the nearest existing ancestor. */
        char ancestor[PATH_MAX];
        strlcpy(ancestor, parent, sizeof(ancestor));
        while (realpath(ancestor, resolved_parent) == NULL) {
            char *cut = strrchr(ancestor, '/');
            if (cut == NULL || (size_t)(cut - ancestor) <= strlen(configured_root)) {
                return fail(error, "PARENT_NOT_FOUND", "no safe parent ancestor exists", ESP_ERR_NOT_FOUND);
            }
            *cut = '\0';
        }
        if (!root_contains(resolved_parent)) {
            return fail(error, "PATH_TRAVERSAL", "parent escapes the configured root", ESP_ERR_INVALID_ARG);
        }
        const char *suffix = normalized + strlen(ancestor);
        if (snprintf(target, PATH_MAX, "%s%s", resolved_parent, suffix) >= PATH_MAX) {
            return fail(error, "INVALID_PATH", "destination path is too long", ESP_ERR_INVALID_SIZE);
        }
    } else {
        if (!root_contains(resolved_parent) || !S_ISDIR(parent_st.st_mode)) {
            return fail(error, "PATH_TRAVERSAL", "parent escapes the configured root", ESP_ERR_INVALID_ARG);
        }
        if (snprintf(target, PATH_MAX, "%s/%s", resolved_parent, slash + 1) >= PATH_MAX) {
            return fail(error, "INVALID_PATH", "destination path is too long", ESP_ERR_INVALID_SIZE);
        }
    }
    struct stat lst;
    *exists = room_lstat(target, &lst) == 0;
    if (*exists) {
        if (S_ISLNK(lst.st_mode)) {
            if (!follow_symlinks) {
                return fail(error, "SYMLINK_TARGET_DENIED", "destination is a symlink", ESP_ERR_INVALID_ARG);
            }
            char resolved_target[PATH_MAX];
            if (realpath(target, resolved_target) == NULL || !root_contains(resolved_target)) {
                return fail(error, "PATH_TRAVERSAL", "symlink target escapes the configured root", ESP_ERR_INVALID_ARG);
            }
            strlcpy(target, resolved_target, PATH_MAX);
            if (stat(target, &lst) != 0) return ESP_FAIL;
        }
        if (!S_ISREG(lst.st_mode)) {
            return fail(error, "IS_DIRECTORY", "destination is not a regular file", ESP_ERR_INVALID_ARG);
        }
    }
    return ESP_OK;
}

static esp_err_t write_transaction(
    const char *target,
    const uint8_t *bytes,
    size_t len,
    bool overwrite,
    bool existed,
    esp_openclaw_node_error_t *error)
{
    char parent[PATH_MAX];
    strlcpy(parent, target, sizeof(parent));
    char *slash = strrchr(parent, '/');
    *slash = '\0';
    char temp[PATH_MAX];
    if (snprintf(temp, sizeof(temp), "%s/.openclaw-tmp-XXXXXX", parent) >= (int)sizeof(temp)) {
        return fail(error, "INVALID_PATH", "destination path is too long", ESP_ERR_INVALID_SIZE);
    }
    int fd = mkstemp(temp);
    bool wrote = fd >= 0 && write(fd, bytes, len) == (ssize_t)len && fsync(fd) == 0;
    if (fd >= 0) close(fd);
    if (!wrote) {
        unlink(temp);
        return fail(error, "WRITE_ERROR", "temporary file write failed", ESP_FAIL);
    }
    if (!overwrite || !existed) {
        if (rename(temp, target) != 0) {
            unlink(temp);
            return fail(error,
                errno == EEXIST ? "EXISTS_NO_OVERWRITE" : "WRITE_ERROR",
                "no-overwrite atomic rename failed", ESP_FAIL);
        }
        return ESP_OK;
    }

    char backup[PATH_MAX];
    if (snprintf(backup, sizeof(backup), "%s/.openclaw-bak-XXXXXX", parent) >= (int)sizeof(backup)) {
        unlink(temp);
        return fail(error, "INVALID_PATH", "destination path is too long", ESP_ERR_INVALID_SIZE);
    }
    int backup_fd = mkstemp(backup);
    if (backup_fd < 0) {
        unlink(temp);
        return fail(error, "WRITE_ERROR", "backup reservation failed", ESP_FAIL);
    }
    close(backup_fd);
    unlink(backup);
    if (rename(target, backup) != 0) {
        unlink(temp);
        return fail(error, "WRITE_ERROR", "existing file could not be backed up", ESP_FAIL);
    }
    if (rename(temp, target) != 0) {
        int saved_errno = errno;
        (void)rename(backup, target);
        unlink(temp);
        errno = saved_errno;
        return fail(error, "WRITE_ERROR", "replacement failed; original restore attempted", ESP_FAIL);
    }
    unlink(backup);
    return ESP_OK;
}

static esp_err_t file_write(
    esp_openclaw_node_handle_t node, void *context, const char *params_json,
    size_t params_len, char **out, esp_openclaw_node_error_t *error)
{
    (void)node;
    (void)context;
    cJSON *params = parse_object(params_json, params_len, error);
    if (params == NULL) return ESP_ERR_INVALID_ARG;
    static const char *const fields[] = {
        "path", "contentBase64", "overwrite", "createParents", "expectedSha256",
        "followSymlinks", "preflightOnly",
    };
    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(params, "path");
    cJSON *data_item = cJSON_GetObjectItemCaseSensitive(params, "contentBase64");
    cJSON *overwrite_item = cJSON_GetObjectItemCaseSensitive(params, "overwrite");
    cJSON *parents_item = cJSON_GetObjectItemCaseSensitive(params, "createParents");
    cJSON *hash_item = cJSON_GetObjectItemCaseSensitive(params, "expectedSha256");
    cJSON *follow_item = cJSON_GetObjectItemCaseSensitive(params, "followSymlinks");
    cJSON *preflight_item = cJSON_GetObjectItemCaseSensitive(params, "preflightOnly");
    bool types_ok = cJSON_IsString(path_item) && path_item->valuestring != NULL &&
        cJSON_IsString(data_item) && data_item->valuestring != NULL &&
        (overwrite_item == NULL || cJSON_IsBool(overwrite_item)) &&
        (parents_item == NULL || cJSON_IsBool(parents_item)) &&
        (hash_item == NULL || (cJSON_IsString(hash_item) && valid_sha256(hash_item->valuestring))) &&
        (follow_item == NULL || cJSON_IsBool(follow_item)) &&
        (preflight_item == NULL || cJSON_IsBool(preflight_item));
    if (!has_only_fields(params, fields, sizeof(fields) / sizeof(fields[0])) || !types_ok) {
        cJSON_Delete(params);
        return fail(error, "INVALID_PARAMS", "file.write has unknown fields or wrong field types", ESP_ERR_INVALID_ARG);
    }
    uint8_t *bytes = NULL;
    size_t data_len = 0;
    esp_err_t err = decode_base64(data_item->valuestring, &bytes, &data_len, error);
    if (err != ESP_OK) {
        cJSON_Delete(params);
        return err;
    }
    char actual[65] = {0};
    sha256_hex(bytes, data_len, actual);
    if (hash_item != NULL && strcasecmp(actual, hash_item->valuestring) != 0) {
        free(bytes);
        cJSON_Delete(params);
        return fail(error, "INTEGRITY_FAILURE", "expectedSha256 does not match decoded bytes", ESP_ERR_INVALID_CRC);
    }
    bool existed = false;
    char target[PATH_MAX];
    bool overwrite = cJSON_IsTrue(overwrite_item);
    bool preflight = cJSON_IsTrue(preflight_item);
    err = resolve_write_path(
        path_item->valuestring,
        cJSON_IsTrue(follow_item),
        cJSON_IsTrue(parents_item),
        preflight,
        target,
        &existed,
        error);
    if (err != ESP_OK) {
        free(bytes);
        cJSON_Delete(params);
        return err;
    }
    if (existed && !overwrite) {
        free(bytes);
        cJSON_Delete(params);
        return fail(error, "EXISTS_NO_OVERWRITE", "file exists and overwrite is false", ESP_ERR_INVALID_STATE);
    }
    if (room_file_mutation_allowed(preflight)) {
        err = write_transaction(target, bytes, data_len, overwrite, existed, error);
    }
    free(bytes);
    if (err != ESP_OK) {
        cJSON_Delete(params);
        return err;
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddStringToObject(result, "path", target);
    cJSON_AddNumberToObject(result, "size", data_len);
    cJSON_AddStringToObject(result, "sha256", actual);
    cJSON_AddBoolToObject(result, "overwritten", existed);
    if (preflight) cJSON_AddBoolToObject(result, "preflightOnly", true);
    return finish_result(result, params, out, error);
}

esp_err_t room_files_register_node_commands(
    esp_openclaw_node_handle_t node,
    const char *public_root)
{
    if (node == NULL || public_root == NULL || strlen(public_root) >= sizeof(configured_root)) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(configured_root, public_root, sizeof(configured_root));
    size_t len = strlen(configured_root);
    while (len > 1 && configured_root[len - 1] == '/') configured_root[--len] = '\0';
    if (realpath(configured_root, canonical_root) == NULL) return ESP_ERR_NOT_FOUND;
    const esp_openclaw_node_command_t commands[] = {
        {.name = "dir.list", .handler = dir_list},
        {.name = "file.fetch", .handler = file_fetch},
        {.name = "file.write", .handler = file_write},
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        ESP_RETURN_ON_ERROR(
            esp_openclaw_node_register_command(node, &commands[i]), TAG,
            "register %s", commands[i].name);
    }
    return ESP_OK;
}
