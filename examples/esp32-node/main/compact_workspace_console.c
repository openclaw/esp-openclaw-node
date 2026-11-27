// SPDX-License-Identifier: Apache-2.0
#include "compact_workspace_console.h"
#include "compact_board.h"
#include "esp_console.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int workspace_command(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        char *json = NULL;
        esp_err_t err = compact_board_console_status(&json);
        if (err == ESP_OK && json != NULL) puts(json);
        free(json);
        return err == ESP_OK ? 0 : 1;
    }
    if (argc != 3 || argv[2][0] < '0' || argv[2][0] > '9') {
        puts("Use workspace status, or workspace <home|next|previous|select> <workspaceRevision>.");
        return 1;
    }
    char *end;
    errno = 0;
    unsigned long revision = strtoul(argv[2], &end, 10);
    if (errno || *end || revision == 0 || revision > UINT32_MAX) return 1;
    compact_input_event_t event = {0};
    if (strcmp(argv[1], "home") == 0) event.home = true;
    else if (strcmp(argv[1], "next") == 0) event.move = 1;
    else if (strcmp(argv[1], "previous") == 0) event.move = -1;
    else if (strcmp(argv[1], "select") == 0) event.select = true;
    else return 1;
    uint32_t request_id = 0;
    esp_err_t err = compact_board_console_input(event, (uint32_t)revision, &request_id);
    if (err == ESP_OK) printf("{\"requestId\":%lu,\"outcome\":\"queued\"}\n", (unsigned long)request_id);
    else printf("Workspace input not queued: %s. Run workspace status to check readiness, pending input and the current revision before retrying.\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

esp_err_t compact_workspace_console_register(void)
{
    const esp_console_cmd_t command = {
        .command = "workspace",
        .help = "Inspect or navigate the current rendered workspace using its revision",
        .func = workspace_command,
    };
    return esp_console_cmd_register(&command);
}
