#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "esp_openclaw_node_persisted_session.h"
#include "nvs.h"

typedef struct { unsigned role, stage, presence; int err; } record_t;
static record_t records[16];
static unsigned record_count, string_calls, erase_calls, commit_calls, close_calls;
static int open_error, version_error, size_error[2], value_error[2], clear_error;
static uint8_t version;
static const char *values[2];
static bool contention, emitter_entered;
static pthread_mutex_t emitter_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t emitter_ready = PTHREAD_COND_INITIALIZER;

static int capture_record(const char *format, va_list args)
{
    assert(record_count < 16);
    record_t *r = &records[record_count++];
    r->role = va_arg(args, unsigned);
    r->stage = va_arg(args, unsigned);
    r->err = va_arg(args, int);
    r->presence = va_arg(args, unsigned);
    char text[160];
    snprintf(text, sizeof(text), format, r->role, r->stage, r->err, r->presence);
    assert(strstr(text, "canary") == NULL);
    assert(strstr(text, "://") == NULL);
    return 0;
}

static void signal_emitter(void)
{
    assert(pthread_mutex_lock(&emitter_lock) == 0);
    emitter_entered = true;
    assert(pthread_cond_signal(&emitter_ready) == 0);
    assert(pthread_mutex_unlock(&emitter_lock) == 0);
}

int host_diagnostic_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (contention) signal_emitter();
    int result = contention ? vprintf(format, args) : capture_record(format, args);
    va_end(args);
    return result;
}

int esp_rom_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result;
    if (contention) {
        /* Model ROM's separate output path, which does not take stdout's lock. */
        char text[160];
        result = vsnprintf(text, sizeof(text), format, args);
        assert(result > 0 && (size_t)result < sizeof(text));
        assert(write(STDOUT_FILENO, text, (size_t)result) == result);
        signal_emitter();
    } else {
        result = capture_record(format, args);
    }
    va_end(args);
    return result;
}

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    assert(strcmp(name, "openclaw") == 0 && mode == NVS_READWRITE);
    *handle = 1;
    return open_error;
}
void nvs_close(nvs_handle_t handle) { assert(handle == 1); close_calls++; }
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out)
{
    (void)h; (void)key; *out = version; return version_error;
}
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *size)
{
    (void)h;
    string_calls++;
    unsigned field = strstr(key, "uri") != NULL ? 0 : 1;
    int err = out == NULL ? size_error[field] : value_error[field];
    if (err != ESP_OK) return err;
    size_t required = strlen(values[field]) + 1;
    if (out != NULL) { assert(*size >= required); memcpy(out, values[field], required); }
    *size = required;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    (void)h; (void)key;
    assert(record_count > 0); /* The original failure must be captured before clearing. */
    erase_calls++;
    return clear_error;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; commit_calls++; return ESP_OK; }
esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v)
{ (void)h; (void)k; (void)v; assert(0); return ESP_FAIL; }
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v)
{ (void)h; (void)k; (void)v; assert(0); return ESP_FAIL; }

static void reset(void)
{
    record_count = string_calls = erase_calls = commit_calls = close_calls = 0;
    open_error = version_error = clear_error = 0;
    memset(size_error, 0, sizeof(size_error));
    memset(value_error, 0, sizeof(value_error));
    version = 1;
    values[0] = "wss://synthetic-url-canary.example/synthetic-id-canary";
    values[1] = "synthetic-token-canary";
}

static void expect(unsigned index, unsigned role, unsigned stage, int err, unsigned presence)
{
    assert(index < record_count);
    record_t r = records[index];
    assert(r.role == role && r.stage == stage && r.err == err && r.presence == presence);
}

static void *load_session_during_log(void *arg)
{
    (void)arg;
    esp_openclaw_node_persisted_session_t session;
    assert(esp_openclaw_node_persisted_session_load("operator", &session) == 4363);
    return NULL;
}

static void test_stdio_contention(void)
{
    reset();
    open_error = 4363;
    contention = true;
    FILE *capture = tmpfile();
    assert(capture != NULL);
    assert(fflush(stdout) == 0);
    int saved_stdout = dup(STDOUT_FILENO);
    assert(saved_stdout >= 0);
    assert(dup2(fileno(capture), STDOUT_FILENO) == STDOUT_FILENO);

    flockfile(stdout);
    assert(fputs("I ", stdout) >= 0);
    assert(fflush(stdout) == 0);
    pthread_t worker;
    assert(pthread_create(&worker, NULL, load_session_during_log, NULL) == 0);
    assert(pthread_mutex_lock(&emitter_lock) == 0);
    while (!emitter_entered) {
        assert(pthread_cond_wait(&emitter_ready, &emitter_lock) == 0);
    }
    assert(pthread_mutex_unlock(&emitter_lock) == 0);
    assert(fputs("(1) synthetic: complete\n", stdout) >= 0);
    assert(fflush(stdout) == 0);
    funlockfile(stdout);
    assert(pthread_join(worker, NULL) == 0);
    assert(fflush(stdout) == 0);
    assert(dup2(saved_stdout, STDOUT_FILENO) == STDOUT_FILENO);
    assert(close(saved_stdout) == 0);

    rewind(capture);
    char output[256] = {0};
    size_t length = fread(output, 1, sizeof(output) - 1, capture);
    assert(!ferror(capture) && feof(capture));
    const char *expected =
        "I (1) synthetic: complete\n"
        "nvs_session_diag role=2 stage=1 err=4363 presence=0\n";
    assert(length == strlen(expected) && strcmp(output, expected) == 0);
    assert(fclose(capture) == 0);
    puts("runtime diagnostic preserved stdout log framing");
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--stdio-contention") == 0) {
        test_stdio_contention();
        return 0;
    }
    assert(argc == 1);
    const char *roles[] = {"node", "operator"};
    for (unsigned role = 1; role <= 2; role++) {
        esp_openclaw_node_persisted_session_t session;
        reset();
        assert(esp_openclaw_node_persisted_session_load(roles[role - 1], &session) == ESP_OK);
        assert(esp_openclaw_node_persisted_session_is_present(&session));
        assert(strcmp(session.gateway_uri, values[0]) == 0 && strcmp(session.device_token, values[1]) == 0);
        assert(record_count == 0 && string_calls == 4 && erase_calls == 0 && close_calls == 1);
        esp_openclaw_node_persisted_session_free(&session);

        reset(); open_error = 4363;
        assert(esp_openclaw_node_persisted_session_load(roles[role - 1], &session) == 4363);
        expect(0, role, 1, 4363, 0);
        assert(close_calls == 0 && string_calls == 0 && erase_calls == 0);

        reset(); version_error = ESP_ERR_NVS_NOT_FOUND;
        assert(esp_openclaw_node_persisted_session_load(roles[role - 1], &session) == ESP_OK);
        expect(0, role, 2, ESP_ERR_NVS_NOT_FOUND, 1);
        assert(!esp_openclaw_node_persisted_session_is_present(&session) && erase_calls == 0);

        for (unsigned field = 0; field < 2; field++) {
            for (unsigned value_read = 0; value_read < 2; value_read++) {
                reset();
                (value_read ? value_error : size_error)[field] = 4363;
                assert(esp_openclaw_node_persisted_session_load(roles[role - 1], &session) == 4363);
                expect(0, role, 3 + 2 * field + value_read, 4363, 0);
                assert(session.gateway_uri == NULL && session.device_token == NULL && erase_calls == 0);
            }
        }

        reset(); version = 2; clear_error = 4363;
        assert(esp_openclaw_node_persisted_session_load(roles[role - 1], &session) == ESP_OK);
        expect(0, role, 2, ESP_OK, 3);
        expect(1, role, 8, 4363, 0);
        assert(erase_calls == 1 && commit_calls == 0 && !esp_openclaw_node_persisted_session_is_present(&session));

        reset(); size_error[1] = ESP_ERR_NVS_NOT_FOUND;
        assert(esp_openclaw_node_persisted_session_load(roles[role - 1], &session) == ESP_OK);
        expect(0, role, 5, ESP_ERR_NVS_NOT_FOUND, 1);
        expect(1, role, 7, ESP_ERR_INVALID_ARG, 4);
        expect(2, role, 8, ESP_OK, 0);
        assert(erase_calls == 3 && commit_calls == 1 && session.gateway_uri == NULL);

        reset(); values[0] = "synthetic-invalid-uri-canary"; clear_error = 4363;
        assert(esp_openclaw_node_persisted_session_load(roles[role - 1], &session) == ESP_OK);
        expect(0, role, 7, ESP_ERR_INVALID_ARG, 5);
        expect(1, role, 8, 4363, 0);
        assert(session.gateway_uri == NULL && session.device_token == NULL);

        reset(); values[0] = ""; values[1] = "";
        assert(esp_openclaw_node_persisted_session_load(roles[role - 1], &session) == ESP_OK);
        expect(0, role, 4, ESP_OK, 2);
        expect(1, role, 6, ESP_OK, 2);
        assert(record_count == 2 && erase_calls == 0 && !esp_openclaw_node_persisted_session_is_present(&session));
    }
    puts("session diagnostic boundary cases passed");
    return 0;
}
