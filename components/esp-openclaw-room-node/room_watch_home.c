#include "room_watch_home.h"

#include <string.h>
#include <stdio.h>

static lv_obj_t *root, *wifi_label, *gateway_label, *title_label, *detail_label;
static lv_obj_t *continue_card, *reader_label;

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int width, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, width);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(obj, &lv_font_montserrat_14, 0);
    lv_label_set_text_static(obj, text);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

void room_watch_home_create(lv_obj_t *parent, lv_event_cb_t menu_handler, lv_event_cb_t continue_handler)
{
    root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(root, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *menu = lv_obj_create(root);
    lv_obj_remove_style_all(menu);
    lv_obj_set_pos(menu, 26, 12);
    lv_obj_set_size(menu, 132, 44);
    lv_obj_set_clickable(menu, true);
    lv_obj_set_scrollable(menu, false);
    lv_obj_add_event_cb(menu, menu_handler, LV_EVENT_ALL, NULL);
    lv_obj_t *menu_label = lv_label_create(menu);
    lv_label_set_text_static(menu_label, LV_SYMBOL_BARS "  Menu");
    lv_obj_set_style_text_color(menu_label, lv_color_hex(0xf4efee), 0);
    lv_obj_set_style_text_font(menu_label, &lv_font_montserrat_14, 0);
    lv_obj_remove_flag(menu_label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(menu_label, LV_ALIGN_LEFT_MID, 0, 0);
    wifi_label = label(root, LV_SYMBOL_WIFI, 196, 22, 30, 0xa6a1ac);
    gateway_label = label(root, "Starting", 230, 22, 112, 0xa6a1ac);
    lv_obj_set_style_text_align(gateway_label, LV_TEXT_ALIGN_RIGHT, 0);
    title_label = label(root, "Your OpenClaw", 26, 312, 316, 0xf4efee);
    lv_obj_set_height(title_label, 28);
#if LV_FONT_MONTSERRAT_20
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, 0);
#endif
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
    detail_label = label(root, "Starting up", 26, 349, 316, 0xb8b0bd);
    lv_obj_set_height(detail_label, 18);
    lv_obj_set_style_text_align(detail_label, LV_TEXT_ALIGN_CENTER, 0);

    continue_card = lv_obj_create(root);
    lv_obj_remove_style_all(continue_card);
    lv_obj_set_pos(continue_card, 26, 304);
    lv_obj_set_size(continue_card, 316, 62);
    lv_obj_set_style_bg_color(continue_card, lv_color_hex(0x2a2233), 0);
    lv_obj_set_style_bg_color(continue_card, lv_color_hex(0x3a2e45), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(continue_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(continue_card, lv_color_hex(0x504054), 0);
    lv_obj_set_style_border_width(continue_card, 1, 0);
    lv_obj_set_style_radius(continue_card, 18, 0);
    lv_obj_set_clickable(continue_card, true);
    lv_obj_set_scrollable(continue_card, false);
    lv_obj_add_event_cb(continue_card, continue_handler, LV_EVENT_ALL, NULL);
    label(continue_card, LV_SYMBOL_ENVELOPE, 16, 23, 24, 0xc9b9db);
    reader_label = label(continue_card, "", 50, 8, 228, 0xf4efee);
    lv_obj_set_height(reader_label, 24);
#if LV_FONT_MONTSERRAT_20
    lv_obj_set_style_text_font(reader_label, &lv_font_montserrat_20, 0);
#endif
    label(continue_card, "Continue conversations", 50, 35, 228, 0xb8b0bd);
    label(continue_card, LV_SYMBOL_RIGHT, 288, 23, 14, 0xc9b9db);
    lv_obj_set_hidden(continue_card, true);
}

bool room_watch_home_render(const room_ui_facts_t *facts, const room_ui_talk_target_t *target, room_ui_state_t state,
                           const char *detail, const char *reader_agent, bool hidden)
{
    if (root == NULL) return false;
    lv_obj_set_flag(root, LV_OBJ_FLAG_HIDDEN, hidden);
    if (hidden) return false;
    bool wifi = facts->wifi == ROOM_UI_WIFI_CONNECTED;
    bool connected = facts->gateway == ROOM_UI_GATEWAY_CONNECTED;
    lv_obj_set_style_text_color(wifi_label, lv_color_hex(wifi ? 0xbfe5d9 : 0x746e7c), 0);
    const char *gateway = connected ? "Connected"
        : facts->gateway == ROOM_UI_GATEWAY_NO_SESSION ? "Pairing needed"
        : facts->gateway == ROOM_UI_GATEWAY_CONNECTING ? "Connecting"
        : facts->gateway == ROOM_UI_GATEWAY_STARTING ? "Starting" : "Offline";
    lv_label_set_text_static(gateway_label, gateway);

    bool reading = reader_agent != NULL && reader_agent[0] != '\0' && wifi && connected &&
        state != ROOM_UI_ERROR && state != ROOM_UI_SETUP &&
        facts->talk != ROOM_UI_TALK_CONNECTING && facts->talk != ROOM_UI_TALK_ACTIVE &&
        facts->talk != ROOM_UI_TALK_STOPPING;
    lv_obj_set_hidden(continue_card, !reading);
    lv_obj_set_hidden(title_label, reading);
    lv_obj_set_hidden(detail_label, reading);
    if (reading) {
        if (strcmp(lv_label_get_text(reader_label), reader_agent) != 0)
            lv_label_set_text(reader_label, reader_agent);
        return true;
    }

    const char *title = "Your OpenClaw";
    const char *subtitle = "Starting up";
    if (state == ROOM_UI_ERROR) {
        title = "Needs attention";
        subtitle = detail[0] != '\0' ? detail : "Hold the screen for device status";
    } else if (state == ROOM_UI_SETUP) {
        title = "Let's get connected";
        subtitle = detail[0] != '\0' ? detail : "Set up over USB";
    } else if (facts->talk == ROOM_UI_TALK_ACTIVE) {
        title = "Conversation live";
        subtitle = "Tap Stop Talk to finish";
    } else if (facts->talk == ROOM_UI_TALK_CONNECTING) {
        title = "Connecting";
        subtitle = "Starting your conversation";
    } else if (facts->talk == ROOM_UI_TALK_STOPPING) {
        title = "Finishing up";
        subtitle = "Ending your conversation";
    } else if (facts->talk == ROOM_UI_TALK_READY) {
        title = "Ready when you are";
        subtitle = "Tap to start a live conversation";
    } else if (facts->wifi == ROOM_UI_WIFI_STARTING || facts->gateway == ROOM_UI_GATEWAY_STARTING) {
        title = "Your OpenClaw";
        subtitle = "Starting up";
    } else if (!wifi || !connected) {
        title = "Let's reconnect";
        subtitle = "Hold the screen for device status";
    } else {
        title = "OpenClaw is here";
        subtitle = facts->talk == ROOM_UI_TALK_NO_SESSION ? "Talk needs an operator session"
            : facts->talk == ROOM_UI_TALK_WAITING ? "Waiting for the operator session"
            : "Talk is unavailable";
    }
    char next_agent[80];
    if (target->selected && state != ROOM_UI_ERROR && state != ROOM_UI_SETUP &&
        facts->talk == ROOM_UI_TALK_READY) {
        if (target->expired) {
            if (facts->talk == ROOM_UI_TALK_READY) title = "Choose an agent";
            subtitle = "Refresh Agents to choose again";
        } else {
            snprintf(next_agent, sizeof(next_agent), "Next Talk: %s", target->name);
            title = next_agent;
            subtitle = "Ready for your next conversation";
        }
    }
    if (strcmp(lv_label_get_text(title_label), title) != 0) lv_label_set_text(title_label, title);
    /* Setup details can contain line breaks; keep them above the action. */
    char line[96];
    size_t i = 0;
    for (; i < sizeof(line) - 1 && subtitle[i] != '\0'; ++i) {
        line[i] = subtitle[i] == '\n' || subtitle[i] == '\r' ? ' ' : subtitle[i];
    }
    line[i] = '\0';
    if (strcmp(lv_label_get_text(detail_label), line) != 0) lv_label_set_text(detail_label, line);
    return false;
}
