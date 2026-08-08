#include "esp_openclaw_room_node.h"
#include "tab5_room_board.h"

void app_main(void)
{
    esp_openclaw_room_node_config_t config = {0};
    ESP_ERROR_CHECK(tab5_room_board_config(&config));
    ESP_ERROR_CHECK(esp_openclaw_room_node_start(&config));
}
