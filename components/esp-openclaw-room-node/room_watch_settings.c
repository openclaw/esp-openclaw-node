#include "room_watch_settings.h"

#include <stdint.h>
#include "room_watch_navigation.h"
#include "openclaw_reading.h"
#include "openclaw_density.h"

static lv_obj_t *root, *heading, *back, *list;
static lv_obj_t *rows[5], *level, *detail, *adjust[2], *display_rows[3], *reading_rows[2], *density_rows[2];
static lv_obj_t *messaging_status, *messaging_detail, *messaging_action;
static lv_obj_t *help_list, *help_rows[4], *help_body, *help_text;
static const struct help_topic {
    room_watch_page_t page;
    const char *label, *title, *text;
} help_topics[] = {
    {ROOM_WATCH_HELP_NAVIGATION, LV_SYMBOL_HOME "  Navigation", "Navigation",
        "Tap Menu to choose Agents, Conversations, Devices, or Settings. Swipe lists to see more.\n\n"
        "Back returns to the previous page. Return Home from the Menu.\n\n"
        "The first tap on a dark screen only wakes it. Tap again to choose an action.\n\n"
        "For larger text or more space between rows, open Settings > Display."},
    {ROOM_WATCH_HELP_MESSAGES, LV_SYMBOL_ENVELOPE "  Messages", "Messages",
        "Choose your assistant in Agents and a thread in Conversations.\n\n"
        "Hold to record captures a short message when recording is ready. Release to review the words, then tap Send. "
        "Leaving before Send discards an unsent recording or draft. If Send is unavailable, request access in Settings > Messaging.\n\n"
        "Live call is separate: open it from Menu, then tap Start call. Its microphone stays open until you tap End call there. "
        "Back and Home keep the call running; Return to call takes you back."},
    {ROOM_WATCH_HELP_SETUP, LV_SYMBOL_USB "  USB setup", "USB setup",
        "First setup uses USB and a computer.\n\n"
        "Open the device's serial console and enter:\n"
        "wifi set \"<ssid>\" \"<password>\"\n"
        "gateway setup-code <code>\n\n"
        "Use a fresh device setup code from OpenClaw. Keep your password and code private.\n\n"
        "After setup, this Watch connects over Wi-Fi independently. It does not need a phone or Dial."},
    {ROOM_WATCH_HELP_STATUS, LV_SYMBOL_WIFI "  Status", "Status icons",
        "Wi-Fi and Gateway are separate connections. Wi-Fi alone does not mean OpenClaw is ready.\n\n"
        "More Wi-Fi arcs mean a stronger measured signal. A broken link means the Gateway is disconnected. "
        "A battery dash means the level is unknown.\n\n"
        "Open Settings > Connection for network details, or Device status for diagnostics. "
        "For message access, open Settings > Messaging and approve the request on your Gateway."},
};

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
    rows[4] = button("?  Help", 26, 376, 316, ROOM_WATCH_HELP, handler);
    help_list = lv_obj_create(root);
    lv_obj_remove_style_all(help_list);
    lv_obj_set_pos(help_list, 26, 120);
    lv_obj_set_size(help_list, 316, 244);
    lv_obj_set_scroll_dir(help_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(help_list, LV_SCROLLBAR_MODE_AUTO);
    for (unsigned i = 0; i < 4; ++i) {
        help_rows[i] = button(help_topics[i].label, 0, (int)i * 64, 316, help_topics[i].page, handler);
        lv_obj_set_parent(help_rows[i], help_list);
    }
    help_body = lv_obj_create(root);
    lv_obj_remove_style_all(help_body);
    lv_obj_set_pos(help_body, 26, 120);
    lv_obj_set_size(help_body, 316, 244);
    lv_obj_set_scroll_dir(help_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(help_body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_t *help_scroll[] = {help_list, help_body};
    for (unsigned i = 0; i < 2; ++i) {
        lv_obj_set_style_width(help_scroll[i], 2, LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_color(help_scroll[i], lv_color_hex(0xb8b0bd), LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_opa(help_scroll[i], LV_OPA_60, LV_PART_SCROLLBAR);
    }
    help_text = label(help_body, "", 0, 306);
    lv_label_set_long_mode(help_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_hidden(help_body, true);
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
    for (unsigned i = 0; i < 5; ++i) lv_obj_set_parent(rows[i], list);
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
    if (hidden) { lv_obj_set_hidden(help_body, true); return; }
    lv_obj_set_height(list, content_bottom - 120);
    lv_obj_set_height(help_list, content_bottom - 120);
    lv_obj_set_height(help_body, content_bottom - 120);
    bool display = page == ROOM_WATCH_DISPLAY;
    bool brightness_page = page == ROOM_WATCH_BRIGHTNESS;
    bool reading = page == ROOM_WATCH_READING;
    bool density = page == ROOM_WATCH_DENSITY;
    bool permissions = page == ROOM_WATCH_MESSAGING;
    bool help = page == ROOM_WATCH_HELP;
    const struct help_topic *topic = NULL;
    for (unsigned i = 0; i < 4; ++i)
        if (page == help_topics[i].page) topic = &help_topics[i];
    bool entering_topic = topic != NULL && lv_obj_is_hidden(help_body);
    lv_obj_set_hidden(help_list, !help);
    lv_obj_set_hidden(help_body, topic == NULL);
    if (topic != NULL) {
        lv_label_set_text_static(help_text, topic->text);
        lv_obj_set_style_text_font(help_text, openclaw_reading_font(), 0);
        lv_obj_update_layout(help_body);
        lv_obj_readjust_scroll(help_body, LV_ANIM_OFF);
        if (entering_topic) lv_obj_scroll_to_y(help_body, 0, LV_ANIM_OFF);
    }
    lv_label_set_text_static(heading, topic != NULL ? topic->title : help ? "Help" : display ? "Display" : brightness_page ? "Brightness" : reading ? "Reading text" : density ? "List spacing" : permissions ? "Messaging" : page == ROOM_WATCH_LOCAL_DEVICE ? "This Watch" : "Settings");
    lv_label_set_text_static(lv_obj_get_child(back, 0), parent == ROOM_WATCH_LOCAL_DEVICE ? LV_SYMBOL_LEFT " This Watch" :
        parent == ROOM_WATCH_SETTINGS ? LV_SYMBOL_LEFT " Settings" :
        parent == ROOM_WATCH_HELP ? LV_SYMBOL_LEFT " Help" :
        parent == ROOM_WATCH_DISPLAY ? LV_SYMBOL_LEFT " Display" :
        parent == ROOM_WATCH_DEVICES ? LV_SYMBOL_LEFT " Devices" : LV_SYMBOL_LEFT " Menu");
    lv_obj_set_hidden(list, brightness_page || reading || density || permissions || help || topic != NULL);
    int row_height = openclaw_density_row_height(false);
    int row_step = row_height + openclaw_density_row_gap();
    for (unsigned i = 0; i < 5; ++i) {
        lv_obj_set_hidden(rows[i], display || brightness_page || reading || density || permissions || help || topic != NULL);
        lv_obj_set_pos(rows[i], 0, (int)i * row_step);
        lv_obj_set_height(rows[i], row_height);
    }
    for (unsigned i = 0; i < 3; ++i) {
        lv_obj_set_hidden(display_rows[i], !display);
        lv_obj_set_pos(display_rows[i], 0, (int)i * row_step);
        lv_obj_set_height(display_rows[i], row_height);
    }
    for (unsigned i = 0; i < 4; ++i) {
        lv_obj_set_pos(help_rows[i], 0, (int)i * row_step);
        lv_obj_set_height(help_rows[i], row_height);
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
        lv_label_set_text_static(detail, "Menus and reading / until restart");
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
    lv_obj_t *buttons[] = {back, rows[0], rows[1], rows[2], rows[3], rows[4],
        help_rows[0], help_rows[1], help_rows[2], help_rows[3],
        display_rows[0], display_rows[1], display_rows[2], reading_rows[0], reading_rows[1],
        density_rows[0], density_rows[1], adjust[0], adjust[1], messaging_action};
    const lv_font_t *font = openclaw_reading_font();
    for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
        lv_obj_t *text = lv_obj_get_child(buttons[i], 0);
        lv_obj_set_style_text_font(text, font, 0);
        lv_obj_set_y(text, (lv_obj_get_height(buttons[i]) - font->line_height) / 2);
    }
    if (!lv_obj_is_hidden(list)) {
        lv_obj_update_layout(list);
        lv_obj_readjust_scroll(list, LV_ANIM_OFF);
    }
    if (help) {
        lv_obj_update_layout(help_list);
        lv_obj_readjust_scroll(help_list, LV_ANIM_OFF);
    }
}
