#pragma once

#include "esp_openclaw_node.h"

esp_err_t room_files_register_node_commands(
    esp_openclaw_node_handle_t node,
    const char *public_root);
