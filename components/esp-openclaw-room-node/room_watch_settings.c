#include "room_watch_settings.h"

#include <stdint.h>
#include "room_watch_navigation.h"

static lv_obj_t *root, *heading, *back;
static lv_obj_t *rows[3], *level, *detail, *adjust[2];

static lv_obj_t *label(lv_obj_t *parent, const char *value, int y, int width)
{
    lv_obj_t *text = lv_label_create(parent);
    lv_obj_set_pos(text, 0, y);
    lv_obj_set_width(text, width);
    lv_obj_set_style_text_font(text, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(text, lv_color_hex(0xf4efee), 0);
    lv_label_set_text_static(text, value);
    lv_obj_set_clickable(text, false);
    return text;
}

static lv_obj_t *button(const char *text, int x, int y, int width, room_watch_page_t action, lv_event_cb_t handler)
{
    lv_obj_t *row = lv_obj_create(root);
    lv_obj_remove_style_all(row);
    lv_obj_set_pos(row, x, y);
    lv_obj_set_size(row, width, 52);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x302535), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 16, 0);
    lv_obj_set_clickable(row, true);
    lv_obj_set_scrollable(row, false);
    lv_obj_t *text_label = label(row, text, 18, width - 32);
    lv_obj_set_x(text_label, 16);
    lv_obj_add_event_cb(row, handler, LV_EVENT_ALL, (void *)(intptr_t)action);
    return row;
}

void room_watch_settings_create(lv_obj_t *parent, lv_event_cb_t handler)
{
    root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollable(root, false);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x15101b), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    back = button(LV_SYMBOL_LEFT " Menu", 26, 12, 164, ROOM_WATCH_BACK, handler);
    heading = label(root, "Settings", 76, 316);
    lv_obj_set_x(heading, 26);
#if LV_FONT_MONTSERRAT_28
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_28, 0);
#endif
    rows[0] = button(LV_SYMBOL_IMAGE "  Display", 26, 120, 316, ROOM_WATCH_DISPLAY, handler);
    rows[1] = button(LV_SYMBOL_WIFI "  Connection", 26, 184, 316, ROOM_WATCH_CONNECTION, handler);
    rows[2] = button(LV_SYMBOL_SETTINGS "  Device status", 26, 248, 316, ROOM_WATCH_DEVICE_STATUS, handler);
    level = label(root, "", 136, 316);
    lv_obj_set_x(level, 26);
    lv_obj_set_style_text_align(level, LV_TEXT_ALIGN_CENTER, 0);
#if LV_FONT_MONTSERRAT_28
    lv_obj_set_style_text_font(level, &lv_font_montserrat_28, 0);
#endif
    detail = label(root, "Until restart", 206, 316);
    lv_obj_set_x(detail, 26);
    lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(detail, lv_color_hex(0xb8b0bd), 0);
    adjust[0] = button(LV_SYMBOL_MINUS "  5%", 26, 264, 152, ROOM_WATCH_DIMMER, handler);
    adjust[1] = button(LV_SYMBOL_PLUS "  5%", 190, 264, 152, ROOM_WATCH_BRIGHTER, handler);
    lv_obj_set_hidden(root, true);
}

void room_watch_settings_render(room_watch_page_t page, room_watch_page_t parent, int brightness, bool failed, bool camera_minimum, bool hidden)
{
    if (root == NULL) return;
    lv_obj_set_hidden(root, hidden);
    if (hidden) return;
    bool display = page == ROOM_WATCH_DISPLAY;
    lv_label_set_text_static(heading, display ? "Display" : page == ROOM_WATCH_LOCAL_DEVICE ? "This Watch" : "Settings");
    lv_label_set_text_static(lv_obj_get_child(back, 0), parent == ROOM_WATCH_LOCAL_DEVICE ? LV_SYMBOL_LEFT " This Watch" :
        parent == ROOM_WATCH_SETTINGS ? LV_SYMBOL_LEFT " Settings" :
        parent == ROOM_WATCH_DEVICES ? LV_SYMBOL_LEFT " Devices" : LV_SYMBOL_LEFT " Menu");
    for (unsigned i = 0; i < 3; ++i) lv_obj_set_hidden(rows[i], display);
    lv_obj_set_hidden(level, !display);
    lv_obj_set_hidden(detail, !display);
    for (unsigned i = 0; i < 2; ++i) lv_obj_set_hidden(adjust[i], !display);
    if (display) {
        lv_label_set_text_fmt(level, "%d%%", brightness);
        lv_label_set_text_static(detail, failed ? "Couldn't change. Try again." :
            camera_minimum ? "Camera needs at least 40%" : "Awake brightness / until restart");
    }
}
