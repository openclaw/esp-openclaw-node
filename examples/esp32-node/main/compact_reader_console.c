// SPDX-License-Identifier: Apache-2.0
#include "compact_reader_console.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_console.h"
#include "esp_openclaw_reader.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

static void erase_code(char *code, size_t length)
{
    volatile char *cursor = code;
    while (length--) *cursor++ = 0;
}

static int reader_command(int argc, char **argv)
{
#ifdef ESP_PLATFORM
    if (argc == 2 && strcmp(argv[1], "heap") == 0) {
        multi_heap_info_t info;
        heap_caps_get_info(&info, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        printf("internalFreeBytes=%zu internalLargestBlockBytes=%zu internalMinimumFreeBytes=%zu\n",
               info.total_free_bytes, info.largest_free_block, info.minimum_free_bytes);
        return 0;
    }
#endif
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        cJSON *status = esp_openclaw_reader_status();
        esp_openclaw_reader_messaging_snapshot_t messaging;
        esp_openclaw_reader_messaging_copy(&messaging);
        if (status != NULL &&
            (cJSON_AddNumberToObject(status, "messagingState", messaging.state) == NULL ||
             cJSON_AddNumberToObject(status, "messagingRevision", messaging.revision) == NULL ||
             cJSON_AddStringToObject(status, "messagingRequestId", messaging.request_id) == NULL)) {
            cJSON_Delete(status);
            status = NULL;
        }
        char *json = status != NULL ? cJSON_PrintUnformatted(status) : NULL;
        bool printed = json != NULL;
        if (json != NULL) puts(json);
        free(json);
        cJSON_Delete(status);
        return printed ? 0 : 1;
    }
    if (argc != 2 || strcmp(argv[1], "setup-code") != 0) {
        puts("Use reader status, reader heap, or reader setup-code. Enter the code at the hidden prompt, never as an argument.");
        return 1;
    }
    char *code = calloc(ESP_OPENCLAW_NODE_BOOTSTRAP_CODE_MAX_LENGTH + 1, 1);
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (code == NULL || flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) {
        free(code);
        puts("Reader input unavailable.");
        return 1;
    }
    /* The REPL has already finished its outer line. Read directly from VFS so
     * bootstrap material is neither echoed by linenoise nor added to history. */
    printf("Reader setup code (hidden, 30 seconds): ");
    fflush(stdout);
    size_t length = 0;
    bool complete = false, valid = true, started = false, io_failed = false;
    int64_t deadline = esp_timer_get_time() + 30000000;
    while (!complete) {
        if (valid && esp_timer_get_time() >= deadline) {
            valid = false;
            started = true;
            erase_code(code, length);
            length = 0;
            puts("\nInput expired. Press Enter to return.");
        }
        char ch;
        ssize_t count = read(STDIN_FILENO, &ch, 1);
        if (count == 1) {
            /* CRLF on the outer command can leave a leading LF in the VFS.
             * Keep owning input until a started secret line is terminated. */
            if (ch == '\r' || ch == '\n') { if (started) complete = true; continue; }
            started = true;
            if (ch == 3) {
                valid = false;
                erase_code(code, length);
                length = 0;
                puts("\nInput cancelled. Press Enter to return.");
                continue;
            }
            if (!valid) continue;
            if (ch == '\b' || ch == 127) { if (length) code[--length] = 0; continue; }
            bool base64 = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '=';
            if (!base64 || length == ESP_OPENCLAW_NODE_BOOTSTRAP_CODE_MAX_LENGTH) {
                valid = false;
                erase_code(code, length);
                length = 0;
                puts("\nInvalid input. Press Enter to return.");
            }
            else code[length++] = ch;
        } else {
            if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && !io_failed) {
                io_failed = true;
                valid = false;
                started = true;
                erase_code(code, length);
                length = 0;
                puts("\nInput unavailable. Press Enter after reconnecting.");
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (fcntl(STDIN_FILENO, F_SETFL, flags) < 0) valid = false;
    esp_err_t err = complete && valid && length ? esp_openclaw_reader_setup_code(code) : ESP_ERR_INVALID_ARG;
    erase_code(code, ESP_OPENCLAW_NODE_BOOTSTRAP_CODE_MAX_LENGTH + 1);
    free(code);
    printf("\nReader setup: %s\n", err == ESP_OK ? "queued once; check reader status" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

esp_err_t compact_reader_console_register(void)
{
    const esp_console_cmd_t command = {
        .command = "reader",
        .help = "Reader status, heap metrics, or one-time, hidden setup-code input",
        .func = reader_command,
    };
    return esp_console_cmd_register(&command);
}
