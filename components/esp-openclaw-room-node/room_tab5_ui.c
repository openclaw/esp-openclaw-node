#include "room_tab5_ui.h"

#include "esp_openclaw_avatar.h"

enum { INK = 0xf1ede5, MUTED = 0xb1aaa0, BACKGROUND = 0x1c1b19, SURFACE = 0x292723 };
static lv_obj_t *root, *heading, *status, *body, *back, *identity, *avatar, *reconnect;
static lv_obj_t *menu_rows[5];
static lv_image_dsc_t avatar_sources[3];
static lv_obj_t *menu_list;

static const lv_font_t *body_font(void)
{
#if LV_FONT_MONTSERRAT_28
    return &lv_font_montserrat_28;
#else
    return &lv_font_montserrat_14;
#endif
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int width)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, width);
    lv_obj_set_style_text_font(obj, body_font(), 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(INK), 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
    lv_label_set_text(obj, text);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *button(const char *text, int x, int y, int width,
    room_watch_page_t page, lv_event_cb_t handler)
{
    lv_obj_t *obj = lv_obj_create(root);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    /* 96 native pixels = 8.3 mm on the nominal 110.69 mm wide display. */
    lv_obj_set_size(obj, width, 96);
    lv_obj_set_style_bg_color(obj, lv_color_hex(SURFACE), 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x3c3933), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 20, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, handler, LV_EVENT_ALL, (void *)(intptr_t)page);
    lv_obj_t *text_obj = label(obj, text, 24, 0, width - 48);
    lv_obj_align(text_obj, LV_ALIGN_LEFT_MID, 24, 0);
    return obj;
}

void room_tab5_ui_create(lv_obj_t *parent, lv_event_cb_t navigation)
{
    root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, lv_color_hex(BACKGROUND), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    status = label(parent, "", 64, 24, 1152);
    lv_obj_set_style_text_color(status, lv_color_hex(MUTED), 0);
    back = button(LV_SYMBOL_LEFT " Home", 64, 80, 256, ROOM_WATCH_HOME, navigation);
    button(LV_SYMBOL_BARS " Menu", 944, 80, 272, ROOM_WATCH_MENU, navigation);
    reconnect = button(LV_SYMBOL_WIFI "  Connection", 320, 552, 640, ROOM_WATCH_CONNECTION, navigation);
    lv_obj_set_height(reconnect, 104);
    identity = label(root, "Default agent", 64, 108, 820);
    avatar = lv_image_create(root);
    lv_obj_set_pos(avatar, 560, 184);
    lv_obj_set_size(avatar, 160, 160);
    lv_image_set_inner_align(avatar, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_remove_flag(avatar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    for (int kind = ESP_OPENCLAW_AVATAR_FOX; kind <= ESP_OPENCLAW_AVATAR_LOBSTER; ++kind) {
        avatar_sources[kind] = (lv_image_dsc_t){
            .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888,
                .w = ESP_OPENCLAW_AVATAR_SIZE, .h = ESP_OPENCLAW_AVATAR_SIZE,
                .stride = ESP_OPENCLAW_AVATAR_SIZE * 4},
            .data_size = ESP_OPENCLAW_AVATAR_SIZE * ESP_OPENCLAW_AVATAR_SIZE * 4,
            .data = esp_openclaw_avatar_pixels(kind),
        };
    }
    heading = label(root, "Ready when you are.", 160, 368, 960);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    body = label(root, "", 160, 424, 960);
    lv_obj_set_style_text_color(body, lv_color_hex(MUTED), 0);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    menu_list = lv_obj_create(root);
    lv_obj_remove_style_all(menu_list);
    lv_obj_set_pos(menu_list, 160, 208);
    lv_obj_set_size(menu_list, 960, 448);
    lv_obj_set_scroll_dir(menu_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(menu_list, LV_SCROLLBAR_MODE_AUTO);
    menu_rows[0] = button(LV_SYMBOL_CALL "  Talk to agent", 160, 208, 960, ROOM_WATCH_CALL, navigation);
    menu_rows[1] = button(LV_SYMBOL_WIFI "  Connection", 160, 320, 960, ROOM_WATCH_CONNECTION, navigation);
    menu_rows[2] = button(LV_SYMBOL_IMAGE "  Canvas", 160, 432, 960, ROOM_WATCH_CANVAS, navigation);
    menu_rows[3] = button(LV_SYMBOL_LIST "  Agents", 160, 544, 960, ROOM_WATCH_AGENTS, navigation);
    menu_rows[4] = button(LV_SYMBOL_ENVELOPE "  Conversations", 160, 656, 960, ROOM_WATCH_CONVERSATIONS, navigation);
    for (unsigned i = 0; i < 5; ++i) {
        lv_obj_set_parent(menu_rows[i], menu_list);
        lv_obj_set_pos(menu_rows[i], 0, (int)i * 112);
    }
}

bool room_tab5_ui_supports(room_watch_page_t page)
{
    return page == ROOM_WATCH_HOME || page == ROOM_WATCH_MENU || page == ROOM_WATCH_CALL ||
        page == ROOM_WATCH_CONNECTION || page == ROOM_WATCH_CANVAS ||
        page == ROOM_WATCH_AGENTS || page == ROOM_WATCH_CONVERSATIONS;
}

void room_tab5_ui_render(room_watch_page_t page, const room_ui_facts_t *facts,
    const room_ui_talk_target_t *target, room_ui_state_t state, const char *detail, bool hidden)
{
    if (root == NULL) return;
    lv_obj_set_hidden(root, hidden || page == ROOM_WATCH_AGENTS);
    lv_obj_set_hidden(status, hidden);
    if (hidden) return;
    lv_obj_move_foreground(status);
    bool home = page == ROOM_WATCH_HOME;
    bool menu = page == ROOM_WATCH_MENU;
    bool call = page == ROOM_WATCH_CALL;
    bool connection = page == ROOM_WATCH_CONNECTION;
    bool active = facts->talk == ROOM_UI_TALK_ACTIVE;
    const char *network = facts->wifi == ROOM_UI_WIFI_CONNECTED ? LV_SYMBOL_WIFI " Wi-Fi" : LV_SYMBOL_WARNING " Wi-Fi offline";
    const char *gateway = facts->gateway == ROOM_UI_GATEWAY_CONNECTED ? "Connected" :
        facts->gateway == ROOM_UI_GATEWAY_NO_SESSION ? "Pairing needed" : "Gateway offline";
    lv_label_set_text_fmt(status, "%s    /    %s", network, gateway);
    lv_obj_set_hidden(back, home);
    lv_obj_set_hidden(identity, !home || active || facts->talk == ROOM_UI_TALK_CONNECTING || facts->talk == ROOM_UI_TALK_STOPPING);
    bool unavailable = facts->talk != ROOM_UI_TALK_READY && facts->talk != ROOM_UI_TALK_ACTIVE &&
        facts->talk != ROOM_UI_TALK_CONNECTING && facts->talk != ROOM_UI_TALK_STOPPING;
    lv_obj_set_hidden(reconnect, !home || !unavailable);
    lv_label_set_text(identity, target->expired ? "Choose an agent in OpenClaw" :
        target->selected ? target->name : "Default agent");
    /* The target is the next call's recipient, not the active call's owner. */
    bool call_open = active || facts->talk == ROOM_UI_TALK_CONNECTING || facts->talk == ROOM_UI_TALK_STOPPING;
    esp_openclaw_avatar_kind_t kind = !call_open && target->selected && !target->expired
        ? target->avatar.kind : ESP_OPENCLAW_AVATAR_LOBSTER;
    bool picture = kind == ESP_OPENCLAW_AVATAR_FOX || kind == ESP_OPENCLAW_AVATAR_LOBSTER;
    lv_obj_set_hidden(avatar, !(home || call) || !picture);
    if (picture) lv_image_set_src(avatar, &avatar_sources[kind]);
    lv_obj_set_hidden(menu_list, !menu);
    lv_obj_set_height(menu_list, active || facts->talk == ROOM_UI_TALK_CONNECTING ? 312 : 448);
    lv_obj_set_hidden(heading, menu);
    lv_obj_set_hidden(body, menu);
    lv_obj_set_y(heading, home || call ? 368 : 220);
    lv_obj_set_y(body, home || call ? 424 : 284);
    const char *title = page == ROOM_WATCH_CONVERSATIONS ? "Opening your conversation..." : connection ? "Connection" : page == ROOM_WATCH_CANVAS ? "No canvas yet" :
        facts->talk == ROOM_UI_TALK_CONNECTING ? "Connecting your call..." :
        facts->talk == ROOM_UI_TALK_STOPPING ? "Ending your call..." : active ? "Your call is open." :
        state == ROOM_UI_ERROR ? "Something needs attention." : state == ROOM_UI_SETUP ? "Let's get connected." :
        facts->gateway != ROOM_UI_GATEWAY_CONNECTED ? "Let's reconnect." : "Ready when you are.";
    lv_label_set_text(heading, title);
    const char *message = detail[0] != '\0' && (state == ROOM_UI_ERROR || state == ROOM_UI_SETUP) ? detail :
        connection ? "Use the USB console to configure Wi-Fi and pair this device with your Gateway." :
        page == ROOM_WATCH_CANVAS ? "Ask your agent to show a canvas on this device." :
        active ? "The microphone stays open until you end the call." :
        facts->talk == ROOM_UI_TALK_UNAVAILABLE ? "Audio unavailable. Check device diagnostics." :
        "Start a live conversation with your agent.";
    lv_label_set_text(body, message);
}
