#include "dial_home.h"

#include <string.h>

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int width)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, width);
    lv_obj_set_height(object, 20);
    lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(object, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(0xf4efee), 0);
    lv_obj_set_clickable(object, false);
    lv_label_set_text_static(object, text);
    return object;
}

void dial_home_create(dial_home_t *home, lv_obj_t *parent)
{
    home->root = lv_obj_create(parent);
    lv_obj_remove_style_all(home->root);
    lv_obj_set_size(home->root, 240, 240);
    lv_obj_set_clickable(home->root, false);
    lv_obj_set_scrollable(home->root, false);
    home->wifi = label(home->root, LV_SYMBOL_WIFI, 76, 20, 20);
    home->link = label(home->root, "Starting", 98, 20, 66);
    lv_obj_set_style_text_color(home->link, lv_color_hex(0xb8b0bd), 0);
    lv_obj_t *title = label(home->root, "OpenClaw", 50, 44, 140);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_t *frame = lv_obj_create(home->root);
    lv_obj_remove_style_all(frame);
    lv_obj_set_pos(frame, 78, 68);
    lv_obj_set_size(frame, 84, 84);
    lv_obj_set_style_radius(frame, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(frame, lv_color_hex(0x332336), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(frame, 1, 0);
    lv_obj_set_style_border_color(frame, lv_color_hex(0x5a3946), 0);
    lv_obj_set_clickable(frame, false);
    lv_obj_set_scrollable(frame, false);
    home->headline = label(home->root, "Starting up", 32, 158, 176);
    lv_obj_set_style_text_font(home->headline, &lv_font_montserrat_16, 0);
    lv_obj_t *pill = lv_obj_create(home->root);
    lv_obj_remove_style_all(pill);
    lv_obj_set_pos(pill, 58, 185);
    lv_obj_set_size(pill, 124, 28);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(0xf4efee), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_clickable(pill, false);
    lv_obj_set_scrollable(pill, false);
    home->action = label(pill, "Open menu", 0, 0, 124);
    lv_obj_set_style_text_color(home->action, lv_color_hex(0x302131), 0);
    lv_obj_center(home->action);
}

void dial_home_render(dial_home_t *home, bool wifi, bool connected, bool assigned,
                      const char *activity)
{
    lv_obj_set_style_text_color(home->wifi, lv_color_hex(wifi ? 0xbfe5d9 : 0x746e7c), 0);
    lv_label_set_text_static(home->link, connected ? "Online" : "Offline");
    const char *headline = activity != NULL ? activity : !connected ? "Gateway offline"
        : assigned ? "Control ready" : "No action assigned";
    if (strcmp(lv_label_get_text(home->headline), headline) != 0)
        lv_label_set_text(home->headline, headline);
}
