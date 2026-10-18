#include "room_board.h"

#include <stdatomic.h>
#include <string.h>

static esp_openclaw_room_node_config_t board;
static bool bound;
static atomic_int display_brightness = -1;

esp_err_t room_board_bind(const esp_openclaw_room_node_config_t *config)
{
    if (config == NULL || config->display.start == NULL ||
        config->display.lock == NULL || config->display.unlock == NULL ||
        config->display.set_brightness == NULL || config->audio.open == NULL ||
        config->audio.afe_layout == NULL || config->audio.record_channels == 0 ||
        config->display.idle_brightness > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    board = *config;
    atomic_store(&display_brightness, -1);
    bound = true;
    return ESP_OK;
}

const esp_openclaw_room_node_config_t *room_board_config(void)
{
    return bound ? &board : NULL;
}

lv_display_t *room_board_display_start(void)
{
    return bound ? board.display.start(board.display.ctx) : NULL;
}

bool room_board_display_lock(uint32_t timeout_ms)
{
    return bound && board.display.lock(board.display.ctx, timeout_ms);
}

void room_board_display_unlock(void)
{
    if (bound) {
        board.display.unlock(board.display.ctx);
    }
}

esp_err_t room_board_display_brightness_set(int percent)
{
    if (!bound) return ESP_ERR_INVALID_STATE;
    esp_err_t err = board.display.set_brightness(board.display.ctx, percent);
    if (err == ESP_OK) atomic_store(&display_brightness, percent);
    return err;
}

bool room_board_display_is_dark(void)
{
    return atomic_load(&display_brightness) == 0;
}
