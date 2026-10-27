#include "room_watch_navigation.h"

#include <stdint.h>

static lv_obj_t *root, *heading, *rows[5], *values[5], *empty;
static const char *menu[] = {LV_SYMBOL_LIST "  Agents", LV_SYMBOL_ENVELOPE "  Conversations",
    LV_SYMBOL_IMAGE "  Canvas", LV_SYMBOL_WIFI "  Connection", LV_SYMBOL_SETTINGS "  Settings"};

static lv_obj_t *label(lv_obj_t *parent, const char *value, int x, int y, int width)
{
    lv_obj_t *text = lv_label_create(parent);
    lv_obj_set_pos(text, x, y);
    lv_obj_set_width(text, width);
    lv_obj_set_style_text_color(text, lv_color_hex(0xf4efee), 0);
    lv_obj_set_style_text_font(text, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(text, LV_LABEL_LONG_DOT);
    lv_label_set_text_static(text, value);
    lv_obj_set_clickable(text, false);
    return text;
}

void room_watch_navigation_create(lv_obj_t *parent, lv_event_cb_t handler)
{
    root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollable(root, false);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x15101b), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_t *back = lv_obj_create(root);
    lv_obj_remove_style_all(back);
    lv_obj_set_pos(back, 26, 18);
    lv_obj_set_size(back, 80, 44);
    lv_obj_set_clickable(back, true);
    lv_obj_set_scrollable(back, false);
    label(back, LV_SYMBOL_LEFT " Home", 0, 14, 80);
    lv_obj_add_event_cb(back, handler, LV_EVENT_ALL, (void *)(intptr_t)ROOM_WATCH_HOME);
    heading = label(root, "Menu", 26, 72, 316);
#if LV_FONT_MONTSERRAT_28
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_28, 0);
#endif
    const room_watch_page_t pages[] = {ROOM_WATCH_AGENTS, ROOM_WATCH_CONVERSATIONS,
        ROOM_WATCH_CANVAS, ROOM_WATCH_CONNECTION, ROOM_WATCH_SETTINGS};
    for (unsigned i = 0; i < 5; ++i) {
        rows[i] = lv_obj_create(root);
        lv_obj_remove_style_all(rows[i]);
        lv_obj_set_pos(rows[i], 26, 116 + (int)i * 48);
        lv_obj_set_size(rows[i], 316, 44);
        lv_obj_set_style_bg_color(rows[i], lv_color_hex(0x302535), 0);
        lv_obj_set_style_bg_opa(rows[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(rows[i], 16, 0);
        lv_obj_set_scrollable(rows[i], false);
        lv_obj_set_clickable(rows[i], true);
        values[i] = label(rows[i], menu[i], 18, 14, 280);
        lv_obj_add_event_cb(rows[i], handler, LV_EVENT_ALL, (void *)(intptr_t)pages[i]);
    }
    empty = label(root, "No card yet\n\nAsk your assistant to show\na card on this watch.", 26, 156, 316);
    lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_hidden(root, true);
}

void room_watch_navigation_render(room_watch_page_t page, const room_ui_facts_t *facts,
                                  bool hidden, bool resume_failed)
{
    if (root == NULL) return;
    lv_obj_set_hidden(root, hidden);
    if (hidden) return;
    bool connection = page == ROOM_WATCH_CONNECTION;
    bool canvas = page == ROOM_WATCH_CANVAS;
    bool resuming = page == ROOM_WATCH_CONVERSATIONS;
    lv_label_set_text_static(heading, connection ? "Connection" : canvas ? "Canvas" : resuming ? "Conversations" : "Menu");
    lv_label_set_text_static(empty, resuming ? resume_failed ? "Reader unavailable.\n\nRetrying...\nYou can return Home."
        : "Opening your last conversation..."
        : "No card yet\n\nAsk your assistant to show\na card on this watch.");
    lv_obj_set_hidden(empty, !canvas && !resuming);
    const char *wifi[] = {"Starting", "Not configured", "Connecting", "Connected", "Offline", "Unavailable"};
    const char *gateway[] = {"Starting", "Connecting", "Connected", "Offline", "Pairing needed"};
    const char *talk[] = {"Starting", "Unavailable", "Waiting for access", "Pairing needed", "Ready", "Connecting", "Active", "Stopping"};
    const char *states[] = {
        (unsigned)facts->wifi < sizeof(wifi) / sizeof(*wifi) ? wifi[facts->wifi] : "Unknown",
        (unsigned)facts->gateway < sizeof(gateway) / sizeof(*gateway) ? gateway[facts->gateway] : "Unknown",
        (unsigned)facts->talk < sizeof(talk) / sizeof(*talk) ? talk[facts->talk] : "Unknown",
    };
    const char *names[] = {"Wi-Fi", "Gateway", "Talk"};
    for (unsigned i = 0; i < 5; ++i) {
        lv_obj_set_hidden(rows[i], canvas || resuming || (connection && i >= 3));
        lv_obj_set_clickable(rows[i], !connection);
        lv_obj_set_y(rows[i], 116 + (int)i * (connection ? 60 : 48));
        lv_obj_set_height(rows[i], connection ? 52 : 44);
        lv_obj_set_y(values[i], connection ? 18 : 14);
        if (connection && i < 3) lv_label_set_text_fmt(values[i], "%s: %s", names[i], states[i]);
        else lv_label_set_text_static(values[i], menu[i]);
    }
}
