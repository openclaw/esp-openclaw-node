#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "talk_host_runner.h"
#include "esp_http_client.h"

static struct {
    void (*run)(void);
    const char *name;
    int line;
} tests[128];
static size_t test_count;
unsigned talk_host_critical_depth;
unsigned talk_host_error_count;

void talk_host_log_error(const char *tag, const char *format, ...)
{
    ++talk_host_error_count;
    va_list args;
    va_start(args, format);
    fprintf(stderr, "[%s] ", tag);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

void talk_host_register_test(void (*test)(void), const char *name, int line)
{
    if (test_count >= sizeof(tests) / sizeof(tests[0])) abort();
    tests[test_count].run = test;
    tests[test_count].name = name;
    tests[test_count++].line = line;
}

void setUp(void) { TEST_ASSERT_EQUAL(0, talk_host_critical_depth); }
void tearDown(void) { TEST_ASSERT_EQUAL(0, talk_host_critical_depth); }

int main(int argc, char **argv)
{
    UnityBegin("test_esp_openclaw_talk.c");
    size_t selected = 0;
    for (size_t i = 0; i < test_count; ++i) {
        if (argc > 1 && strstr(tests[i].name, argv[1]) == NULL) continue;
        ++selected;
        UnityDefaultTestRun(tests[i].run, tests[i].name, tests[i].line);
    }
    if (selected == 0) {
        fprintf(stderr, "No Talk tests matched\n");
        return 2;
    }
    return UnityEnd();
}

/* This suite exercises Gateway signaling and lifecycle, never SDP networking.
 * Fail rather than let a transport stub manufacture an HTTP success. */
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    (void)config;
    TEST_FAIL_MESSAGE("Unexpected HTTP exchange in host signaling test");
    return NULL;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value)
{
    (void)client; (void)key; (void)value;
    return ESP_FAIL;
}

esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *data, int length)
{
    (void)client; (void)data; (void)length;
    return ESP_FAIL;
}

esp_err_t esp_http_client_perform(esp_http_client_handle_t client)
{
    (void)client;
    return ESP_FAIL;
}

int esp_http_client_get_status_code(esp_http_client_handle_t client)
{
    (void)client;
    return 0;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{
    (void)client;
    return ESP_OK;
}

const char *esp_err_to_name(esp_err_t error)
{
    (void)error;
    return "host transport unavailable";
}
