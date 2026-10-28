#include "room_watch_navigation.h"
#include "openclaw_reading.h"
#include "openclaw_density.h"
#include "openclaw_connection_details.h"

#include <string.h>

#include <stdint.h>

static const struct { const char *label; room_watch_page_t page; } menu[] = {
    {LV_SYMBOL_LIST "  Agents", ROOM_WATCH_AGENTS},
    {LV_SYMBOL_ENVELOPE "  Conversations", ROOM_WATCH_CONVERSATIONS},
    {LV_SYMBOL_CALL "  Live call", ROOM_WATCH_CALL},
    {LV_SYMBOL_IMAGE "  Canvas", ROOM_WATCH_CANVAS},
    {LV_SYMBOL_CHARGE "  Devices", ROOM_WATCH_DEVICES},
    {LV_SYMBOL_SETTINGS "  Settings", ROOM_WATCH_SETTINGS},
};
enum { MENU_COUNT = sizeof(menu) / sizeof(menu[0]) };
static lv_obj_t *root, *heading, *back_label, *rows[MENU_COUNT], *values[MENU_COUNT], *empty;
static lv_obj_t *connection_body, *connection_text, *connection_status[2], *menu_list;
static lv_timer_t *connection_timer;
static bool display_awake, connection_active;

static void refresh_connection(lv_timer_t *timer)
{
    (void)timer;
    if (!display_awake || !connection_active || !lv_obj_is_visible(root)) return;
    esp_openclaw_node_wifi_status_t wifi;
    esp_openclaw_node_wifi_get_status(&wifi);
    char details[384];
    openclaw_wifi_details_format(details, sizeof(details), &wifi);
    if (strcmp(lv_label_get_text(connection_text), details) != 0)
        lv_label_set_text(connection_text, details);
}

void room_watch_navigation_set_awake(bool awake)
{
    if (display_awake == awake) return;
    display_awake = awake;
    if (connection_timer == NULL) return;
    if (awake) {
        lv_timer_resume(connection_timer);
        lv_timer_reset(connection_timer);
        refresh_connection(NULL);
    } else lv_timer_pause(connection_timer);
}

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
    lv_obj_set_size(back, 164, 44);
    lv_obj_set_clickable(back, true);
    lv_obj_set_scrollable(back, false);
    back_label = label(back, LV_SYMBOL_LEFT " Home", 0, 14, 164);
    lv_obj_add_event_cb(back, handler, LV_EVENT_ALL, (void *)(intptr_t)ROOM_WATCH_BACK);
    heading = label(root, "Menu", 26, 72, 316);
#if LV_FONT_MONTSERRAT_28
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_28, 0);
#endif
    menu_list = lv_obj_create(root);
    lv_obj_remove_style_all(menu_list);
    lv_obj_set_pos(menu_list, 26, 116);
    lv_obj_set_size(menu_list, 316, 248);
    lv_obj_set_scroll_dir(menu_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(menu_list, LV_SCROLLBAR_MODE_AUTO);
    for (unsigned i = 0; i < 2; ++i)
        connection_status[i] = label(root, "", 26, 112 + (int)i * 24, 316);
    for (unsigned i = 0; i < MENU_COUNT; ++i) {
        rows[i] = lv_obj_create(menu_list);
        lv_obj_remove_style_all(rows[i]);
        lv_obj_set_pos(rows[i], 0, (int)i * 48);
        lv_obj_set_size(rows[i], 316, 44);
        lv_obj_set_style_bg_color(rows[i], lv_color_hex(0x302535), 0);
        lv_obj_set_style_bg_opa(rows[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(rows[i], 16, 0);
        lv_obj_set_scrollable(rows[i], false);
        lv_obj_set_clickable(rows[i], true);
        values[i] = label(rows[i], menu[i].label, 18, 14, 280);
        lv_obj_add_event_cb(rows[i], handler, LV_EVENT_ALL, (void *)(intptr_t)menu[i].page);
    }
    connection_body = lv_obj_create(root);
    lv_obj_remove_style_all(connection_body);
    lv_obj_set_pos(connection_body, 26, 164);
    lv_obj_set_size(connection_body, 316, 194);
    lv_obj_set_scroll_dir(connection_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(connection_body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(connection_body, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(connection_body, lv_color_hex(0xb8b0bd), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(connection_body, LV_OPA_60, LV_PART_SCROLLBAR);
    connection_text = label(connection_body, "Reading network...", 0, 0, 306);
    lv_label_set_long_mode(connection_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(connection_text, LV_SIZE_CONTENT);
    connection_timer = lv_timer_create(refresh_connection, 1000, NULL);
    if (!display_awake) lv_timer_pause(connection_timer);
    lv_obj_set_hidden(connection_body, true);
    empty = label(root, "No card yet\n\nAsk your assistant to show\na card on this watch.", 26, 156, 316);
    lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_hidden(root, true);
}

static const char *call_summary(const room_ui_facts_t *facts)
{
    switch (facts->talk) {
    case ROOM_UI_TALK_STARTING: return "Preparing audio...\n\nNo microphone is open.";
    case ROOM_UI_TALK_UNAVAILABLE: return "Live audio is unavailable.\n\nOpen device diagnostics to check the microphone and speaker.";
    case ROOM_UI_TALK_NO_SESSION: return "Pair this watch with OpenClaw before calling.\n\nOpen Settings > Connection for setup.";
    case ROOM_UI_TALK_READY: return "Start a live audio conversation.\n\nThe microphone stays open until you end the call.";
    case ROOM_UI_TALK_CONNECTING: return "Connecting your call...\n\nYou can stop before it connects.";
    case ROOM_UI_TALK_ACTIVE: return "Your call is active.\n\nYou can browse without ending it.";
    case ROOM_UI_TALK_STOPPING: return "Ending your call...\n\nWait for the microphone to close.";
    default: return facts->gateway == ROOM_UI_GATEWAY_CONNECTED
        ? "Waiting for call access.\n\nCheck the connection or pairing in Settings."
        : "Connect to your Gateway.\n\nOpen Settings > Connection to check setup.";
    }
}

void room_watch_navigation_render(room_watch_page_t page, const room_ui_facts_t *facts,
                                  bool hidden, bool resume_failed, room_watch_page_t parent, int content_bottom)
{
    if (root == NULL) return;
    bool entering_connection = !connection_active && !hidden && page == ROOM_WATCH_CONNECTION;
    connection_active = !hidden && page == ROOM_WATCH_CONNECTION;
    lv_obj_set_hidden(root, hidden);
    if (hidden) return;
    lv_obj_set_height(menu_list, content_bottom - 116);
    lv_obj_set_height(connection_body, content_bottom - 164);
    lv_obj_set_style_text_font(connection_text, openclaw_reading_font(), 0);
    lv_obj_set_hidden(connection_body, !connection_active);
    if (entering_connection) {
        refresh_connection(NULL);
        lv_obj_scroll_to_y(connection_body, 0, LV_ANIM_OFF);
    }
    lv_label_set_text_static(back_label, parent == ROOM_WATCH_LOCAL_DEVICE ? LV_SYMBOL_LEFT " This Watch" :
        parent == ROOM_WATCH_SETTINGS ? LV_SYMBOL_LEFT " Settings" :
        parent == ROOM_WATCH_MENU ? LV_SYMBOL_LEFT " Menu" : LV_SYMBOL_LEFT " Home");
    bool connection = page == ROOM_WATCH_CONNECTION;
    bool canvas = page == ROOM_WATCH_CANVAS;
    bool resuming = page == ROOM_WATCH_CONVERSATIONS;
    bool call = page == ROOM_WATCH_CALL;
    lv_label_set_text_static(heading, call ? "Live call" : connection ? "Connection" : canvas ? "Canvas" : resuming ? "Conversations" : "Menu");
    lv_label_set_text_static(empty, call ? call_summary(facts) : resuming ? resume_failed ? "Reader unavailable.\n\nRetrying...\nYou can return Home."
        : "Opening your last conversation..."
        : "No card yet\n\nAsk your assistant to show\na card on this watch.");
    lv_obj_set_style_text_font(empty, call ? openclaw_reading_font() : &lv_font_montserrat_14, 0);
    lv_obj_set_height(empty, call ? content_bottom - 156 : LV_SIZE_CONTENT);
    lv_obj_set_hidden(empty, !canvas && !resuming && !call);
    const char *gateway[] = {"Starting", "Connecting", "Connected", "Offline", "Pairing needed"};
    const char *talk[] = {"Starting", "Unavailable", "Waiting for access", "Pairing needed", "Ready", "Connecting", "Active", "Stopping"};
    const char *states[] = {
        (unsigned)facts->gateway < sizeof(gateway) / sizeof(*gateway) ? gateway[facts->gateway] : "Unknown",
        (unsigned)facts->talk < sizeof(talk) / sizeof(*talk) ? talk[facts->talk] : "Unknown",
    };
    const char *names[] = {"Gateway", "Talk"};
    lv_obj_set_hidden(menu_list, canvas || resuming || connection || call);
    for (unsigned i = 0; i < 2; ++i) {
        lv_obj_set_hidden(connection_status[i], !connection);
        if (connection) lv_label_set_text_fmt(connection_status[i], "%s: %s", names[i], states[i]);
    }
    int row_height = openclaw_density_row_height(false);
    const lv_font_t *font = openclaw_reading_font();
    lv_obj_set_style_text_font(back_label, font, 0);
    lv_obj_set_y(back_label, (44 - font->line_height) / 2);
    for (unsigned i = 0; i < MENU_COUNT; ++i) {
        lv_obj_set_y(rows[i], (int)i * (row_height + openclaw_density_row_gap()));
        lv_obj_set_height(rows[i], row_height);
        lv_obj_set_style_text_font(values[i], font, 0);
        lv_obj_set_y(values[i], (row_height - font->line_height) / 2);
    }
    if (!lv_obj_is_hidden(menu_list)) {
        lv_obj_update_layout(menu_list);
        lv_obj_readjust_scroll(menu_list, LV_ANIM_OFF);
    }
}
