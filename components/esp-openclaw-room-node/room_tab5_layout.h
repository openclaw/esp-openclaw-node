#pragma once

#include <string.h>
#include "room_board.h"
#include "openclaw_reading.h"
#include "openclaw_density.h"

static inline bool room_tab5_layout(void)
{
    const esp_openclaw_room_node_config_t *board = room_board_config();
    return board != NULL && board->model_identifier != NULL &&
        strcmp(board->model_identifier, "m5stack-tab5") == 0 &&
        board->display.native_width == 1280 && board->display.native_height == 720;
}

static inline const lv_font_t *room_page_font(void)
{
#if LV_FONT_MONTSERRAT_36
    if (room_tab5_layout() && openclaw_reading_size() == OPENCLAW_READING_LARGE)
        return &lv_font_montserrat_36;
#endif
#if LV_FONT_MONTSERRAT_28
    if (room_tab5_layout()) return &lv_font_montserrat_28;
#endif
    return openclaw_reading_font();
}

static inline int room_page_row(bool secondary)
{
    if (room_tab5_layout()) return (secondary ? 112 : 96) +
        (openclaw_density() == OPENCLAW_DENSITY_COMFORTABLE ? 16 : 0);
    return openclaw_density_row_height(secondary);
}

static inline int room_page_gap(void)
{
    return room_tab5_layout() ? 16 : openclaw_density_row_gap();
}

static inline void room_tab5_button(lv_obj_t *button, int x, int y, int width)
{
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, 96);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x292723), 0);
    lv_obj_set_style_radius(button, 20, 0);
    lv_obj_set_style_opa(button, LV_OPA_40, LV_STATE_DISABLED);
    for (unsigned i = 0; i < lv_obj_get_child_count(button); ++i) {
        lv_obj_t *text = lv_obj_get_child(button, i);
        lv_obj_set_width(text, width - 48);
        lv_obj_set_style_text_font(text, room_page_font(), 0);
        lv_obj_align(text, LV_ALIGN_LEFT_MID, 24, 0);
    }
}

static inline void room_tab5_header(lv_obj_t *root, lv_obj_t *heading, lv_obj_t *back)
{
    lv_obj_set_style_bg_color(root, lv_color_hex(0x1c1b19), 0);
    room_tab5_button(back, 64, 80, 272);
    lv_obj_set_pos(heading, 368, 112);
    lv_obj_set_width(heading, 544);
    lv_obj_set_style_text_font(heading, room_page_font(), 0);
}
