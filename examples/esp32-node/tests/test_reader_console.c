// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "esp_console.h"
#include "esp_openclaw_reader.h"

static const char *input;
static size_t consumed, expire_at;
static int flags, submitted;
static bool hard_error;
static int64_t now;
static char received[ESP_OPENCLAW_NODE_BOOTSTRAP_CODE_MAX_LENGTH + 1];
static ssize_t test_read(int fd, void *out, size_t size) {
    assert(fd == STDIN_FILENO && size == 1);
    if (expire_at && consumed == expire_at) {
        expire_at = 0;
        if (hard_error) errno = EIO;
        else { now = 31000000; errno = EAGAIN; }
        return -1;
    }
    assert(input[consumed]); *(char *)out = input[consumed++]; return 1;
}
static int test_fcntl(int fd, int operation, ...) {
    assert(fd == STDIN_FILENO);
    if (operation == F_GETFL) return flags;
    assert(operation == F_SETFL); va_list args; va_start(args, operation);
    flags = va_arg(args, int); va_end(args); return 0;
}
int64_t esp_timer_get_time(void) { return now; }
void vTaskDelay(unsigned ticks) { now += (int64_t)ticks * 1000; }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "rejected"; }
esp_err_t esp_openclaw_reader_setup_code(const char *code) {
    ++submitted; strcpy(received, code); return ESP_OK;
}
cJSON *esp_openclaw_reader_status(void) { return cJSON_CreateObject(); }
esp_err_t esp_console_cmd_register(const esp_console_cmd_t *cmd) { (void)cmd; return ESP_OK; }

#define read test_read
#define fcntl test_fcntl
#include "../main/compact_reader_console.c"
#undef read
#undef fcntl

static void run(const char *text, size_t expiry, bool accepted) {
    input = text; consumed = 0; expire_at = expiry; now = 0; flags = 0; submitted = 0;
    received[0] = 0;
    FILE *capture = tmpfile(); assert(capture);
    int saved = dup(STDOUT_FILENO); assert(saved >= 0);
    fflush(stdout); assert(dup2(fileno(capture), STDOUT_FILENO) >= 0);
    char *argv[] = {"reader", "setup-code"};
    int result = reader_command(2, argv);
    fflush(stdout); assert(dup2(saved, STDOUT_FILENO) >= 0); close(saved);
    rewind(capture); char output[1024] = {0}; fread(output, 1, sizeof(output) - 1, capture); fclose(capture);
    assert((result == 0) == accepted && submitted == (int)accepted);
    assert(flags == 0 && consumed == strlen(input));
    assert(strstr(output, "synthetic-bootstrap") == NULL && strstr(output, "secret-tail") == NULL);
}

int main(void) {
    run("synthetic-bootstrap\n", 0, true); assert(strcmp(received, "synthetic-bootstrap") == 0);
    run("\nsynthetic-bootstrap\n", 0, true); assert(strcmp(received, "synthetic-bootstrap") == 0);
    run("synthetic-bootstrap\n", 4, false);
    hard_error = true; run("synthetic-bootstrap\n", 4, false); hard_error = false;
    run("synthetic\3secret-tail\n", 0, false);
    char oversized[ESP_OPENCLAW_NODE_BOOTSTRAP_CODE_MAX_LENGTH + 4];
    memset(oversized, 'A', sizeof(oversized) - 2);
    oversized[sizeof(oversized) - 2] = '\n'; oversized[sizeof(oversized) - 1] = 0;
    run(oversized, 0, false);
    puts("Reader console: hidden input, timeout/cancel quarantine and oversize rejection passed");
}
