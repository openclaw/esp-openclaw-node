#include "room_watch_settings.h"

#include <stdint.h>
#include "room_watch_navigation.h"
#include "openclaw_reading.h"
#include "openclaw_density.h"

static lv_obj_t *root, *heading, *back, *list;
static lv_obj_t *rows[4], *level, *detail, *adjust[2], *display_rows[3], *reading_rows[2], *density_rows[2];
static lv_obj_t *messaging_status, *messaging_detail, *messaging_action;

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

void room_watch_settings_create(lv_obj_t *parent, lv_event_cb_t handler, lv_event_cb_t messaging_handler)
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
    list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 26, 120);
    lv_obj_set_size(list, 316, 244);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    rows[0] = button(LV_SYMBOL_IMAGE "  Display", 26, 120, 316, ROOM_WATCH_DISPLAY, handler);
    rows[1] = button(LV_SYMBOL_WIFI "  Connection", 26, 184, 316, ROOM_WATCH_CONNECTION, handler);
    rows[2] = button(LV_SYMBOL_SETTINGS "  Device status", 26, 248, 316, ROOM_WATCH_DEVICE_STATUS, handler);
    rows[3] = button(LV_SYMBOL_ENVELOPE "  Messaging", 26, 312, 316, ROOM_WATCH_MESSAGING, handler);
    messaging_status = label(root, "", 126, 316);
    lv_obj_set_x(messaging_status, 26);
#if LV_FONT_MONTSERRAT_20
    lv_obj_set_style_text_font(messaging_status, &lv_font_montserrat_20, 0);
#endif
    messaging_detail = label(root, "", 170, 316);
    lv_obj_set_x(messaging_detail, 26);
    lv_label_set_long_mode(messaging_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(messaging_detail, lv_color_hex(0xb8b0bd), 0);
    messaging_action = button("Request access", 26, 292, 316, ROOM_WATCH_MESSAGING, messaging_handler);
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
    display_rows[0] = button(LV_SYMBOL_IMAGE "  Brightness", 26, 120, 316, ROOM_WATCH_BRIGHTNESS, handler);
    display_rows[1] = button(LV_SYMBOL_LIST "  Reading text", 26, 184, 316, ROOM_WATCH_READING, handler);
    display_rows[2] = button(LV_SYMBOL_LIST "  List spacing", 26, 248, 316, ROOM_WATCH_DENSITY, handler);
    for (unsigned i = 0; i < 4; ++i) lv_obj_set_parent(rows[i], list);
    for (unsigned i = 0; i < 3; ++i) lv_obj_set_parent(display_rows[i], list);
    density_rows[0] = button("Compact", 26, 184, 316, ROOM_WATCH_COMPACT, handler);
    density_rows[1] = button("Comfortable", 26, 248, 316, ROOM_WATCH_COMFORTABLE, handler);
    reading_rows[0] = button("Standard", 26, 184, 316, ROOM_WATCH_TEXT_STANDARD, handler);
    reading_rows[1] = button("Larger", 26, 248, 316, ROOM_WATCH_TEXT_LARGE, handler);
    lv_obj_set_hidden(root, true);
}

void room_watch_settings_render(room_watch_page_t page, room_watch_page_t parent, int brightness, bool failed, bool camera_minimum, const room_messaging_snapshot_t *messaging, bool hidden, int content_bottom)
{
    if (root == NULL) return;
    lv_obj_set_hidden(root, hidden);
    if (hidden) return;
    lv_obj_set_height(list, content_bottom - 120);
    bool display = page == ROOM_WATCH_DISPLAY;
    bool brightness_page = page == ROOM_WATCH_BRIGHTNESS;
    bool reading = page == ROOM_WATCH_READING;
    bool density = page == ROOM_WATCH_DENSITY;
    bool permissions = page == ROOM_WATCH_MESSAGING;
    lv_label_set_text_static(heading, display ? "Display" : brightness_page ? "Brightness" : reading ? "Reading text" : density ? "List spacing" : permissions ? "Messaging" : page == ROOM_WATCH_LOCAL_DEVICE ? "This Watch" : "Settings");
    lv_label_set_text_static(lv_obj_get_child(back, 0), parent == ROOM_WATCH_LOCAL_DEVICE ? LV_SYMBOL_LEFT " This Watch" :
        parent == ROOM_WATCH_SETTINGS ? LV_SYMBOL_LEFT " Settings" :
        parent == ROOM_WATCH_DISPLAY ? LV_SYMBOL_LEFT " Display" :
        parent == ROOM_WATCH_DEVICES ? LV_SYMBOL_LEFT " Devices" : LV_SYMBOL_LEFT " Menu");
    lv_obj_set_hidden(list, brightness_page || reading || density || permissions);
    int row_height = openclaw_density_row_height(false);
    int row_step = row_height + openclaw_density_row_gap();
    for (unsigned i = 0; i < 4; ++i) {
        lv_obj_set_hidden(rows[i], display || brightness_page || reading || density || permissions);
        lv_obj_set_pos(rows[i], 0, (int)i * row_step);
        lv_obj_set_height(rows[i], row_height);
        lv_obj_set_y(lv_obj_get_child(rows[i], 0), (row_height - 16) / 2);
    }
    for (unsigned i = 0; i < 3; ++i) {
        lv_obj_set_hidden(display_rows[i], !display);
        lv_obj_set_pos(display_rows[i], 0, (int)i * row_step);
        lv_obj_set_height(display_rows[i], row_height);
        lv_obj_set_y(lv_obj_get_child(display_rows[i], 0), (row_height - 16) / 2);
    }
    lv_obj_set_hidden(messaging_status, !permissions);
    lv_obj_set_hidden(messaging_detail, !permissions);
    lv_obj_set_hidden(messaging_action, !permissions);
    if (permissions && messaging != NULL) {
        const char *status = "Messaging off";
        const char *description = "Approve access on your Gateway to send messages and create conversations.";
        const char *action = "Request access";
        switch (messaging->state) {
        case ROOM_MESSAGING_REQUESTING:
            status = "Requesting access"; description = "Sending your request to the Gateway."; action = "Requesting..."; break;
        case ROOM_MESSAGING_WAITING:
            status = "Awaiting approval"; description = "Approve this Watch on your Gateway. You can leave this screen while you wait."; action = "Waiting..."; break;
        case ROOM_MESSAGING_CONNECTING:
            status = "Connecting"; description = "Access approved. Reconnecting before messaging is enabled."; action = "Connecting..."; break;
        case ROOM_MESSAGING_READY:
            status = messaging->connected ? "Messaging enabled" : "Gateway offline";
            description = messaging->connected ? "You can send reviewed voice drafts and create conversations." : "Messaging is approved. Reconnect to your Gateway to use it.";
            lv_obj_set_hidden(messaging_action, true); break;
        case ROOM_MESSAGING_REJECTED:
            status = "Request declined"; description = "The Gateway declined access. You can request it again."; action = "Try again"; break;
        case ROOM_MESSAGING_EXPIRED:
            status = "Request expired"; description = "Approval did not finish in time. You can try again when ready."; action = "Try again"; break;
        case ROOM_MESSAGING_ERROR:
            status = "Couldn't enable messaging"; description = "Check your Gateway connection, then try again."; action = "Try again"; break;
        case ROOM_MESSAGING_REQUIRED: break;
        }
        if (messaging->state == ROOM_MESSAGING_REQUIRED && !messaging->can_request)
            description = messaging->connected ? "Finish Talk, drafts, or pending chats before requesting access." : "Connect to your Gateway before requesting access.";
        lv_label_set_text_static(messaging_status, status);
        lv_label_set_text_static(messaging_detail, description);
        lv_label_set_text_static(lv_obj_get_child(messaging_action, 0), action);
        lv_obj_set_clickable(messaging_action, messaging->can_request);
        if (messaging->can_request) lv_obj_clear_state(messaging_action, LV_STATE_DISABLED);
        else lv_obj_add_state(messaging_action, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(messaging_action, lv_color_hex(messaging->can_request ? 0xa83648 : 0x302535), 0);
    }
    lv_obj_set_hidden(level, !brightness_page && !reading && !density);
    lv_obj_set_hidden(detail, !brightness_page && !reading && !density);
    for (unsigned i = 0; i < 2; ++i) {
        lv_obj_set_hidden(adjust[i], !brightness_page);
        lv_obj_set_hidden(reading_rows[i], !reading);
        lv_obj_set_hidden(density_rows[i], !density);
        lv_obj_set_height(reading_rows[i], row_height);
        lv_obj_set_height(density_rows[i], row_height);
        lv_obj_set_y(lv_obj_get_child(reading_rows[i], 0), (row_height - 16) / 2);
        lv_obj_set_y(lv_obj_get_child(density_rows[i], 0), (row_height - 16) / 2);
    }
    lv_obj_set_y(detail, reading || density ? 320 : 206);
    if (density) {
        lv_obj_set_style_text_font(level, &lv_font_montserrat_14, 0);
        bool compact = openclaw_density() == OPENCLAW_DENSITY_COMPACT;
        lv_label_set_text_static(level, compact ? "More items in view" : "More room between items");
        lv_label_set_text_static(detail, "Menu spacing / until restart");
        lv_label_set_text_static(lv_obj_get_child(density_rows[0], 0), compact ? LV_SYMBOL_OK "  Compact" : "Compact");
        lv_label_set_text_static(lv_obj_get_child(density_rows[1], 0), compact ? "Comfortable" : LV_SYMBOL_OK "  Comfortable");
    }
    if (reading) {
        lv_obj_set_style_text_font(level, openclaw_reading_font(), 0);
        lv_label_set_text_static(level, "Hello from your assistant.");
        lv_label_set_text_static(detail, "Reading views / until restart");
        bool large = openclaw_reading_size() == OPENCLAW_READING_LARGE;
        lv_label_set_text_static(lv_obj_get_child(reading_rows[0], 0), large ? "Standard" : LV_SYMBOL_OK "  Standard");
        lv_label_set_text_static(lv_obj_get_child(reading_rows[1], 0), large ? LV_SYMBOL_OK "  Larger" : "Larger");
    }
    if (brightness_page) {
#if LV_FONT_MONTSERRAT_28
        lv_obj_set_style_text_font(level, &lv_font_montserrat_28, 0);
#else
        lv_obj_set_style_text_font(level, &lv_font_montserrat_14, 0);
#endif
        lv_label_set_text_fmt(level, "%d%%", brightness);
        lv_label_set_text_static(detail, failed ? "Couldn't change. Try again." :
            camera_minimum ? "Camera needs at least 40%" : "Awake brightness / until restart");
    }
    if (!lv_obj_is_hidden(list)) {
        lv_obj_update_layout(list);
        lv_obj_readjust_scroll(list, LV_ANIM_OFF);
    }
}
