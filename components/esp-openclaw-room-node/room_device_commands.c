#include "room_device_commands.h"

#include "esp_check.h"
#include "esp_openclaw_node_common_device_node_cmd.h"
#include "room_board.h"

#define TAG "room_device"

esp_err_t room_device_register_node_commands(esp_openclaw_node_handle_t node)
{
    const esp_openclaw_room_node_config_t *board = room_board_config();
    static esp_openclaw_node_device_commands_config_t config;
    config = (esp_openclaw_node_device_commands_config_t){
        .device_name = board->display_name,
        .model_identifier = board->model_identifier,
        .locale = "en-US",
        .get_storage_metrics = board->storage.get_metrics,
        .context = board->storage.ctx,
    };
    return esp_openclaw_node_register_device_commands(node, &config);
}
