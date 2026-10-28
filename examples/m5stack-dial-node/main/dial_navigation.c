#include "dial_navigation.h"
#include "esp_openclaw_avatar.h"
#include "openclaw_connection_details.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void render(dial_navigation_t *nav);
static lv_image_dsc_t avatar_sources[3];

static bool reply_page(dial_navigation_view_t view)
{
    return view == DIAL_NAV_REPLY_PICKER || view == DIAL_NAV_REPLY_REVIEW || view == DIAL_NAV_REPLY_ACTIONS;
}

static bool reply_busy(const dial_navigation_t *nav)
{
    return nav->reply.state == DIAL_REPLY_SENDING || nav->reply.state == DIAL_REPLY_ACCEPTED;
}

static bool reply_available(const dial_navigation_t *nav)
{
    return nav->conversations != NULL && nav->conversation_state == DIAL_AGENTS_READY &&
        nav->preview_state == DIAL_AGENTS_READY && nav->conversations->selected < nav->conversations->page.count &&
        (nav->conversations->preview.status == ESP_OPENCLAW_PREVIEW_OK ||
         nav->conversations->preview.status == ESP_OPENCLAW_PREVIEW_EMPTY);
}

static lv_obj_t *avatar_image(lv_obj_t *parent, int x, int y, int size)
{
    lv_obj_t *image = lv_image_create(parent);
    lv_obj_set_pos(image, x, y);
    lv_obj_set_size(image, size, size);
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_antialias(image, true);
    lv_obj_set_clickable(image, false);
    lv_obj_set_scrollable(image, false);
    lv_obj_set_hidden(image, true);
    return image;
}

static void render_avatar(lv_obj_t *text, lv_obj_t *image, const esp_openclaw_agent_t *agent)
{
    esp_openclaw_avatar_t avatar = esp_openclaw_agent_avatar(agent);
    bool picture = avatar.kind != ESP_OPENCLAW_AVATAR_TEXT;
    lv_obj_set_hidden(text, picture);
    lv_obj_set_hidden(image, !picture);
    if (picture) lv_image_set_src(image, &avatar_sources[avatar.kind]);
    else lv_label_set_text(text, avatar.text);
}

static lv_obj_t *text(lv_obj_t *parent, const char *value, int x, int y, int width)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, 20);
    lv_obj_set_clickable(label, false);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, value);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xf4efee), 0);
    return label;
}

static void changed(dial_navigation_t *nav)
{
    nav->handler(DIAL_NAV_CHANGED, 0, nav->context);
    render(nav);
}

static void back(dial_navigation_t *nav)
{
    if (nav->view == DIAL_NAV_MENU) nav->handler(DIAL_NAV_HOME, 0, nav->context);
    else {
        if (nav->view == DIAL_NAV_REPLY_ACTIONS) {
            nav->view = DIAL_NAV_REPLY_REVIEW;
            changed(nav);
            return;
        }
        if (nav->view == DIAL_NAV_REPLY_REVIEW) {
            bool draft = nav->reply.state == DIAL_REPLY_PREPARING || nav->reply.state == DIAL_REPLY_REVIEW;
            nav->handler(DIAL_NAV_REPLY_CLOSE, nav->reply.revision, nav->context);
            nav->view = draft ? DIAL_NAV_REPLY_PICKER : DIAL_NAV_PREVIEW;
            nav->focus = 0;
            changed(nav);
            return;
        }
        if (nav->view == DIAL_NAV_REPLY_PICKER) {
            nav->view = DIAL_NAV_PREVIEW;
            changed(nav);
            return;
        }
        if (nav->view == DIAL_NAV_MESSAGING) {
            bool reply = reply_page(nav->settings_parent);
            nav->view = reply ? nav->settings_parent : DIAL_NAV_SETTINGS;
            nav->focus = reply ? 0 : 2;
            changed(nav);
            return;
        }
        if (nav->view == DIAL_NAV_DEVICE_DETAIL || nav->view == DIAL_NAV_LOCAL_DEVICE) {
            bool local = nav->view == DIAL_NAV_LOCAL_DEVICE;
            nav->view = DIAL_NAV_DEVICES;
            nav->focus = local ? 0 : nav->device_focus;
            changed(nav);
            if (local) nav->handler(DIAL_NAV_DEVICES_REFRESH, 0, nav->context);
            return;
        }
        if (nav->view == DIAL_NAV_DEVICES) {
            nav->view = DIAL_NAV_MENU;
            nav->focus = 2;
            changed(nav);
            return;
        }
        if (nav->view == DIAL_NAV_ACTIVITY) {
            nav->view = DIAL_NAV_MENU;
            nav->focus = 3;
            changed(nav);
            return;
        }
        if (nav->view == DIAL_NAV_DISPLAY || nav->view == DIAL_NAV_CONNECTION) {
            bool local = nav->settings_parent == DIAL_NAV_LOCAL_DEVICE;
            nav->focus = (nav->view == DIAL_NAV_CONNECTION ? 1 : 0) + (local ? 1 : 0);
            nav->view = local ? DIAL_NAV_LOCAL_DEVICE : DIAL_NAV_SETTINGS;
            changed(nav);
            return;
        }
        if (nav->view == DIAL_NAV_PREVIEW) {
            nav->view = DIAL_NAV_SESSIONS;
            nav->focus = nav->session_focus;
            changed(nav);
            return;
        }
        if (nav->view == DIAL_NAV_SESSIONS) {
            nav->view = DIAL_NAV_DETAIL;
            nav->focus = nav->agent_focus;
            changed(nav);
            return;
        }
        nav->view = nav->view == DIAL_NAV_DETAIL ? DIAL_NAV_AGENTS : DIAL_NAV_MENU;
        if (nav->view == DIAL_NAV_MENU) nav->focus = 0;
        changed(nav);
    }
}

static void touch(lv_event_t *event)
{
    dial_navigation_t *nav = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *indev = lv_indev_active();
    if (indev != NULL && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
        lv_point_t point;
        lv_indev_get_point(indev, &point);
        if (code == LV_EVENT_PRESSED) {
            nav->press_point = point;
            nav->moved = false;
        } else if (abs(point.x - nav->press_point.x) > LV_INDEV_DEF_SCROLL_LIMIT ||
                   abs(point.y - nav->press_point.y) > LV_INDEV_DEF_SCROLL_LIMIT ||
                   code == LV_EVENT_PRESS_LOST) nav->moved = true;
    }
    if (code != LV_EVENT_SHORT_CLICKED && code != LV_EVENT_LONG_PRESSED) return;
    if (nav->moved) return;
    if (code == LV_EVENT_LONG_PRESSED) {
        nav->handler(DIAL_NAV_HOME, 0, nav->context);
        return;
    }
    lv_obj_t *target = lv_event_get_target(event);
    if (target == nav->back) {
        back(nav);
        return;
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (target == nav->rows[i]) {
            if (nav->view == DIAL_NAV_CONNECTION) return;
            if (nav->view != DIAL_NAV_DETAIL) nav->focus = nav->first + i;
            if (nav->view == DIAL_NAV_DISPLAY) dial_navigation_move(nav, i == 0 ? -1 : 1);
            else dial_navigation_activate(nav);
            return;
        }
    }
}

static lv_obj_t *button(dial_navigation_t *nav, lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *button = lv_obj_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, w, h);
    lv_obj_set_clickable(button, true);
    lv_obj_set_scrollable(button, false);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(button, touch, LV_EVENT_ALL, nav);
    return button;
}

void dial_navigation_create(dial_navigation_t *nav, lv_obj_t *parent,
                            dial_navigation_handler_t handler, void *context)
{
    *nav = (dial_navigation_t){.handler = handler, .context = context};
    for (int kind = ESP_OPENCLAW_AVATAR_FOX; kind <= ESP_OPENCLAW_AVATAR_LOBSTER; ++kind)
        avatar_sources[kind] = (lv_image_dsc_t){
            .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888,
                .w = ESP_OPENCLAW_AVATAR_SIZE, .h = ESP_OPENCLAW_AVATAR_SIZE,
                .stride = ESP_OPENCLAW_AVATAR_SIZE * 4},
            .data_size = ESP_OPENCLAW_AVATAR_SIZE * ESP_OPENCLAW_AVATAR_SIZE * 4,
            .data = esp_openclaw_avatar_pixels(kind),
        };
    nav->root = lv_obj_create(parent);
    lv_obj_remove_style_all(nav->root);
    lv_obj_set_size(nav->root, 240, 240);
    lv_obj_set_clickable(nav->root, false);
    lv_obj_set_scrollable(nav->root, false);
    nav->back = button(nav, nav->root, 54, 30, 28, 28);
    lv_obj_set_style_bg_color(nav->back, lv_color_hex(0x302535), 0);
    text(nav->back, LV_SYMBOL_LEFT, 9, 5, 14);
    nav->heading = text(nav->root, "Menu", 88, 34, 96);
    lv_obj_set_style_text_font(nav->heading, &lv_font_montserrat_16, 0);
    for (unsigned i = 0; i < 3; ++i) {
        nav->rows[i] = button(nav, nav->root, 38, 64 + (int)i * 44, 164, 36);
        nav->icons[i] = text(nav->rows[i], "", 12, 9, 20);
        lv_obj_set_width(nav->icons[i], 24);
        lv_obj_set_style_text_align(nav->icons[i], LV_TEXT_ALIGN_CENTER, 0);
        nav->avatars[i] = avatar_image(nav->rows[i], 8, 6, 24);
        nav->names[i] = text(nav->rows[i], "", 40, 9, 112);
    }
    nav->hint = text(nav->root, "Turn + press", 68, 198, 104);
    lv_obj_set_style_text_align(nav->hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(nav->hint, lv_color_hex(0xb8b0bd), 0);
    nav->description = text(nav->root, "", 44, 80, 152);
    lv_obj_set_size(nav->description, 152, 104);
    lv_label_set_long_mode(nav->description, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(nav->description, LV_TEXT_ALIGN_CENTER, 0);
    nav->identity = text(nav->root, "", 44, 142, 152);
    lv_obj_set_style_text_align(nav->identity, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(nav->identity, LV_LABEL_LONG_SCROLL_CIRCULAR);
    nav->detail_avatar = avatar_image(nav->root, 100, 64, 40);
    nav->detail_initials = text(nav->root, "", 100, 64, 40);
    lv_obj_set_height(nav->detail_initials, 40);
    lv_obj_set_style_text_align(nav->detail_initials, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(nav->detail_initials, 8, 0);
    lv_obj_set_style_bg_color(nav->detail_initials, lv_color_hex(0x655074), 0);
    lv_obj_set_style_bg_opa(nav->detail_initials, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nav->detail_initials, LV_RADIUS_CIRCLE, 0);
#if LV_FONT_MONTSERRAT_24
    lv_obj_set_style_text_font(nav->detail_initials, &lv_font_montserrat_24, 0);
#endif
    lv_obj_set_hidden(nav->detail_initials, true);
    nav->messaging_icon = text(nav->root, LV_SYMBOL_ENVELOPE, 108, 68, 24);
    nav->messaging_status = text(nav->root, "", 38, 96, 164);
    lv_obj_set_style_text_font(nav->messaging_status, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(nav->messaging_status, LV_TEXT_ALIGN_CENTER, 0);
    nav->messaging_detail = text(nav->root, "", 38, 122, 164);
    lv_obj_set_height(nav->messaging_detail, 40);
    lv_label_set_long_mode(nav->messaging_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(nav->messaging_detail, LV_TEXT_ALIGN_CENTER, 0);
    nav->preview_body = lv_obj_create(nav->root);
    lv_obj_remove_style_all(nav->preview_body);
    lv_obj_set_pos(nav->preview_body, 44, 70);
    lv_obj_set_size(nav->preview_body, 152, 112);
    lv_obj_set_style_width(nav->preview_body, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(nav->preview_body, lv_color_hex(0xb8b0bd), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(nav->preview_body, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_scroll_dir(nav->preview_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(nav->preview_body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(nav->preview_body, touch, LV_EVENT_ALL, nav);
    nav->preview_text = text(nav->preview_body, "", 0, 0, 144);
    lv_obj_set_height(nav->preview_text, LV_SIZE_CONTENT);
    lv_label_set_long_mode(nav->preview_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_hidden(nav->preview_body, true);
    lv_obj_set_hidden(nav->root, true);
}

static unsigned agent_count(const dial_navigation_t *nav)
{
    if (nav->view == DIAL_NAV_SESSIONS)
        return nav->conversations != NULL && nav->conversation_state == DIAL_AGENTS_READY ? nav->conversations->page.count : 0;
    return nav->snapshot != NULL && nav->state == DIAL_AGENTS_READY ? nav->snapshot->page.count : 0;
}

static bool has_next(const dial_navigation_t *nav)
{
    if (nav->view == DIAL_NAV_SESSIONS)
        return nav->conversations != NULL && nav->conversation_state == DIAL_AGENTS_READY && nav->conversations->page.has_more;
    if (nav->snapshot == NULL || nav->state != DIAL_AGENTS_READY) return false;
    const esp_openclaw_agent_page_t *page = &nav->snapshot->page;
    return page->offset + page->count < page->total;
}

static bool has_previous(const dial_navigation_t *nav)
{
    if (nav->view == DIAL_NAV_SESSIONS)
        return nav->conversations != NULL && nav->conversations->page.offset > 0 &&
            (nav->conversation_state == DIAL_AGENTS_READY || nav->conversation_state == DIAL_AGENTS_EMPTY);
    return nav->snapshot != NULL && nav->snapshot->page.offset > 0 &&
        (nav->state == DIAL_AGENTS_READY || nav->state == DIAL_AGENTS_EMPTY);
}

static const char *state_message(dial_agents_state_t state, bool conversations)
{
    switch (state) {
    case DIAL_AGENTS_IDLE: return conversations ? "Choose an agent again" : "Open the agent list";
    case DIAL_AGENTS_LOADING: return conversations ? "Loading conversations..." : "Loading agents...";
    case DIAL_AGENTS_CONNECTING: return "Connecting...";
    case DIAL_AGENTS_OFFLINE: return "Gateway offline\nControls still work";
    case DIAL_AGENTS_ACCESS_NEEDED: return "Read access needed\nPair with OpenClaw";
    case DIAL_AGENTS_EMPTY: return conversations ? "No conversations yet" : "No agents on this page";
    case DIAL_AGENTS_TOO_LARGE: return conversations ? "Conversation too large" : "Agent list too large";
    default: return conversations ? "Couldn't load conversations" : "Couldn't load agents";
    }
}

static void item(dial_navigation_t *nav, unsigned index, const char **name, const char **icon)
{
    *icon = LV_SYMBOL_RIGHT;
    if (nav->view == DIAL_NAV_MENU) {
        const char *names[] = {"Agents", "Conversations", "Devices", "Activity", "Settings"};
        const char *icons[] = {LV_SYMBOL_DIRECTORY, LV_SYMBOL_LIST, LV_SYMBOL_HOME, LV_SYMBOL_LIST, LV_SYMBOL_SETTINGS};
        *name = names[index];
        *icon = icons[index];
        return;
    }
    if (nav->view == DIAL_NAV_SETTINGS) {
        const char *names[] = {"Display", "Connection", "Messaging", "Back"};
        const char *icons[] = {LV_SYMBOL_IMAGE, LV_SYMBOL_WIFI, LV_SYMBOL_ENVELOPE, LV_SYMBOL_LEFT};
        *name = names[index];
        *icon = icons[index];
        return;
    }
    if (index < agent_count(nav)) {
        *name = nav->view == DIAL_NAV_SESSIONS ? nav->conversations->page.sessions[index].title : nav->snapshot->page.agents[index].name;
        *icon = nav->view == DIAL_NAV_SESSIONS ? LV_SYMBOL_LIST : LV_SYMBOL_DIRECTORY;
        return;
    }
    index -= agent_count(nav);
    if (has_next(nav)) {
        if (index == 0) { *name = nav->view == DIAL_NAV_SESSIONS ? "Next page" : "Next agents"; return; }
        --index;
    }
    if (has_previous(nav)) {
        if (index == 0) { *name = nav->view == DIAL_NAV_SESSIONS ? "Previous page" : "Previous agents"; *icon = LV_SYMBOL_LEFT; return; }
        --index;
    }
    *name = index == 0 ? "Refresh" : "Back";
    *icon = index == 0 ? LV_SYMBOL_REFRESH : LV_SYMBOL_LEFT;
}

static void render_display(dial_navigation_t *nav)
{
    lv_label_set_text_static(nav->heading, "Display");
    lv_obj_set_hidden(nav->preview_body, true);
    lv_obj_set_hidden(nav->description, false);
    lv_obj_set_height(nav->description, 32);
#if LV_FONT_MONTSERRAT_24
    lv_obj_set_style_text_font(nav->description, &lv_font_montserrat_24, 0);
#endif
    lv_label_set_text_fmt(nav->description, "%d%%", nav->brightness);
    lv_obj_set_hidden(nav->identity, false);
    lv_obj_set_y(nav->identity, 116);
    lv_label_set_text_static(nav->identity, nav->brightness_failed ? "Couldn't change" : "Until restart");
    lv_obj_set_hidden(nav->hint, false);
    lv_label_set_text_static(nav->hint, nav->brightness_failed ? "Turn to retry" : "Turn to adjust");
    for (unsigned i = 0; i < 3; ++i) {
        lv_obj_set_hidden(nav->rows[i], i == 2);
        if (i == 2) continue;
        lv_obj_set_pos(nav->rows[i], i == 0 ? 38 : 126, 150);
        lv_obj_set_size(nav->rows[i], 76, 36);
        lv_obj_set_pos(nav->icons[i], 12, 9);
        lv_obj_set_pos(nav->names[i], 34, 9);
        lv_obj_set_width(nav->names[i], 32);
        lv_label_set_text_static(nav->icons[i], i == 0 ? LV_SYMBOL_MINUS : LV_SYMBOL_PLUS);
        lv_label_set_text_static(nav->names[i], "5%");
        lv_obj_set_style_bg_color(nav->rows[i], lv_color_hex(0x302535), 0);
        lv_obj_set_style_text_color(nav->names[i], lv_color_hex(0xf4efee), 0);
        lv_obj_set_style_text_color(nav->icons[i], lv_color_hex(0xf4efee), 0);
    }
}

static bool messaging_pending(dial_messaging_state_t state)
{
    return state == DIAL_MESSAGING_REQUESTING || state == DIAL_MESSAGING_WAITING ||
        state == DIAL_MESSAGING_CONNECTING;
}

static void render_messaging(dial_navigation_t *nav)
{
    const char *status, *detail;
    switch (nav->messaging.state) {
    case DIAL_MESSAGING_OFF:
        status = "Access off"; detail = "Read only.\nRequest access below."; break;
    case DIAL_MESSAGING_REQUESTING:
        status = "Requesting"; detail = "Contacting Gateway.\nStill read only."; break;
    case DIAL_MESSAGING_WAITING:
        status = "Approval needed"; detail = "Approve on Gateway.\nStill read only."; break;
    case DIAL_MESSAGING_CONNECTING:
        status = "Reconnecting"; detail = "Checking access.\nPlease wait."; break;
    case DIAL_MESSAGING_READY:
        status = "Access ready"; detail = "Turn off only here.\nGateway grant stays."; break;
    case DIAL_MESSAGING_REJECTED:
        status = "Request rejected"; detail = "Still read only.\nRequest again."; break;
    case DIAL_MESSAGING_EXPIRED:
        status = "Request expired"; detail = "Still read only.\nRequest again."; break;
    default:
        status = "Request failed"; detail = "Check connection.\nThen request again."; break;
    }
    if (nav->messaging_action_failed) detail = "Connect to Gateway.\nThen try again.";
    nav->count = 1;
    nav->focus = nav->first = 0;
    lv_label_set_text_static(nav->heading, "Messaging");
    lv_obj_set_hidden(nav->description, true);
    lv_obj_set_hidden(nav->identity, true);
    lv_obj_set_hidden(nav->preview_body, true);
    lv_obj_set_hidden(nav->messaging_icon, false);
    lv_obj_set_hidden(nav->messaging_status, false);
    lv_obj_set_hidden(nav->messaging_detail, false);
    lv_label_set_text_static(nav->messaging_status, status);
    lv_label_set_text_static(nav->messaging_detail, detail);
    lv_obj_set_hidden(nav->hint, false);
    lv_label_set_text_static(nav->hint, "Press or tap");
    for (unsigned i = 0; i < 3; ++i) {
        lv_obj_set_hidden(nav->rows[i], i != 0);
        if (i != 0) continue;
        bool ready = nav->messaging.state == DIAL_MESSAGING_READY;
        bool cancel = ready || messaging_pending(nav->messaging.state);
        lv_obj_set_pos(nav->rows[i], 44, 168);
        lv_obj_set_size(nav->rows[i], 152, 32);
        lv_obj_set_pos(nav->icons[i], 12, 7);
        lv_obj_set_pos(nav->names[i], 40, 7);
        lv_obj_set_width(nav->names[i], 100);
        lv_label_set_text_static(nav->icons[i], cancel ? LV_SYMBOL_CLOSE : LV_SYMBOL_PLUS);
        lv_label_set_text_static(nav->names[i], ready ? "Turn off" : cancel ? "Cancel" : "Request");
        lv_obj_set_style_bg_color(nav->rows[i], lv_color_hex(0xf4efee), 0);
        lv_obj_set_style_text_color(nav->names[i], lv_color_hex(0x302131), 0);
        lv_obj_set_style_text_color(nav->icons[i], lv_color_hex(0x302131), 0);
    }
}

typedef enum { REPLY_SEND, REPLY_PRESETS, REPLY_READ, REPLY_ACCESS, REPLY_REVIEW } reply_action_t;

static unsigned reply_actions(const dial_navigation_t *nav, reply_action_t actions[3])
{
    unsigned count = 0;
    if (nav->reply.state == DIAL_REPLY_REVIEW) {
        if (nav->messaging.state != DIAL_MESSAGING_READY) actions[count++] = REPLY_ACCESS;
        else if (nav->reply.can_send) actions[count++] = REPLY_SEND;
    } else if (nav->reply.can_read) actions[count++] = REPLY_READ;
    if (!reply_busy(nav)) actions[count++] = REPLY_PRESETS;
    actions[count++] = REPLY_REVIEW;
    return count;
}

static void reply_button(dial_navigation_t *nav, unsigned row, const char *name, const char *icon, bool bottom)
{
    lv_obj_set_hidden(nav->rows[row], false);
    lv_obj_set_pos(nav->rows[row], bottom ? 52 : 38, bottom ? 176 : 64 + (int)row * 44);
    lv_obj_set_size(nav->rows[row], bottom ? 136 : 164, bottom ? 28 : 36);
    lv_obj_set_pos(nav->icons[row], 12, bottom ? 5 : 9);
    lv_obj_set_pos(nav->names[row], 40, bottom ? 5 : 9);
    lv_obj_set_width(nav->names[row], bottom ? 88 : 112);
    lv_label_set_text_static(nav->names[row], name);
    lv_label_set_text_static(nav->icons[row], icon);
    bool focus = bottom || nav->focus == row;
    lv_obj_set_style_bg_color(nav->rows[row], lv_color_hex(focus ? 0xf4efee : 0x302535), 0);
    lv_color_t color = lv_color_hex(focus ? 0x302131 : 0xf4efee);
    lv_obj_set_style_text_color(nav->names[row], color, 0);
    lv_obj_set_style_text_color(nav->icons[row], color, 0);
}

static void render_reply(dial_navigation_t *nav)
{
    lv_obj_set_hidden(nav->description, true);
    lv_obj_set_hidden(nav->identity, true);
    lv_obj_set_hidden(nav->preview_body, true);
    for (unsigned i = 0; i < 3; ++i) lv_obj_set_hidden(nav->rows[i], true);
    lv_obj_set_hidden(nav->hint, false);
    nav->first = 0;
    if (nav->view == DIAL_NAV_REPLY_REVIEW) {
        const char *titles[] = {"Reply", "Preparing", "Review", "Sending", "Accepted", "Sent", "Check reply", "Reply failed"};
        lv_label_set_text_static(nav->heading, titles[nav->reply.state]);
        lv_obj_set_hidden(nav->preview_body, false);
        lv_obj_set_pos(nav->preview_body, 44, 70);
        lv_obj_set_height(nav->preview_body, 100);
        char body[768];
        const char *detail = nav->reply_action_failed ? "Action unavailable. Go back to the conversation and try again." :
            nav->reply.state == DIAL_REPLY_UNKNOWN ? "Check this conversation before trying again." : nav->reply.detail;
        snprintf(body, sizeof(body), "%s\n%s\n\n%s\n\n%s\n\nAgent: %s", nav->reply.agent.name,
            nav->reply.title, nav->reply.text, detail, nav->reply.agent.id);
        if (strcmp(lv_label_get_text(nav->preview_text), body) != 0) {
            lv_label_set_text(nav->preview_text, body);
            lv_obj_scroll_to_y(nav->preview_body, 0, LV_ANIM_OFF);
        }
        nav->count = 1;
        nav->focus = 0;
        reply_button(nav, 0, "Actions", LV_SYMBOL_LIST, true);
        lv_obj_set_hidden(nav->hint, true);
        return;
    }
    lv_label_set_text_static(nav->heading, nav->view == DIAL_NAV_REPLY_PICKER ? "Quick reply" : "Actions");
    lv_label_set_text_static(nav->hint, nav->reply_action_failed ? "Back to retry" : "Turn + press");
    if (nav->view == DIAL_NAV_REPLY_PICKER && nav->messaging.state != DIAL_MESSAGING_READY) {
        nav->count = 1;
        nav->focus = 0;
        lv_obj_set_hidden(nav->description, false);
        lv_obj_set_pos(nav->description, 44, 76);
        lv_obj_set_size(nav->description, 152, 80);
        lv_obj_set_style_text_font(nav->description, &lv_font_montserrat_14, 0);
        lv_label_set_long_mode(nav->description, LV_LABEL_LONG_WRAP);
        lv_label_set_text_static(nav->description, "Messaging access needed.\n\nRequest approval in Settings.");
        reply_button(nav, 0, "Messaging", LV_SYMBOL_SETTINGS, true);
        lv_obj_set_hidden(nav->hint, true);
        return;
    }
    const char *presets[] = {"Continue", "Summarize", "Explain more"};
    const char *names[] = {"Send", "Presets", "Read reply", "Messaging", "Review"};
    const char *icons[] = {LV_SYMBOL_UP, LV_SYMBOL_LIST, LV_SYMBOL_LIST, LV_SYMBOL_SETTINGS, LV_SYMBOL_LEFT};
    reply_action_t actions[3];
    nav->count = nav->view == DIAL_NAV_REPLY_PICKER ? DIAL_REPLY_PRESET_COUNT : reply_actions(nav, actions);
    if (nav->focus >= nav->count) nav->focus = 0;
    for (unsigned i = 0; i < nav->count; ++i) {
        const char *name = nav->view == DIAL_NAV_REPLY_PICKER ? presets[i] : names[actions[i]];
        if (nav->view == DIAL_NAV_REPLY_ACTIONS && actions[i] == REPLY_PRESETS && nav->reply.state >= DIAL_REPLY_SENT)
            name = "New reply";
        reply_button(nav, i, name, nav->view == DIAL_NAV_REPLY_PICKER ? LV_SYMBOL_RIGHT : icons[actions[i]], false);
    }
}

bool dial_navigation_has_devices(const dial_navigation_t *nav)
{
    return nav->view == DIAL_NAV_DEVICES || nav->view == DIAL_NAV_DEVICE_DETAIL;
}

static unsigned device_count(const dial_navigation_t *nav)
{
    return nav->devices != NULL && nav->device_state == ESP_OPENCLAW_DEVICES_READY ? nav->devices->count : 0;
}

static void render_devices(dial_navigation_t *nav)
{
    bool detail = nav->view == DIAL_NAV_DEVICE_DETAIL;
    bool local = nav->view == DIAL_NAV_LOCAL_DEVICE;
    unsigned count = device_count(nav);
    nav->count = local ? 3 : count + 3;
    if (nav->focus >= nav->count) nav->focus = 0;
    nav->first = nav->focus / 3 * 3;
    lv_obj_set_hidden(nav->description, true);
    lv_obj_set_hidden(nav->identity, true);
    lv_obj_set_hidden(nav->preview_body, !detail);
    lv_obj_set_pos(nav->preview_body, 44, 70);
    lv_obj_set_height(nav->preview_body, 112);
    lv_obj_set_hidden(nav->hint, false);
    lv_label_set_text_static(nav->heading, local ? "This Dial" : "Devices");
    const char *hint = "Turn + press";
    if (!local) {
        switch (nav->device_state) {
        case ESP_OPENCLAW_DEVICES_IDLE: hint = "Open or refresh"; break;
        case ESP_OPENCLAW_DEVICES_LOADING: hint = "Loading..."; break;
        case ESP_OPENCLAW_DEVICES_OFFLINE: hint = "Offline / local"; break;
        case ESP_OPENCLAW_DEVICES_EMPTY: hint = "No other devices"; break;
        case ESP_OPENCLAW_DEVICES_READY: hint = "Read only"; break;
        case ESP_OPENCLAW_DEVICES_TOO_LARGE: hint = "List too large"; break;
        case ESP_OPENCLAW_DEVICES_TIMEOUT: hint = "Timed out"; break;
        default: hint = "Try Refresh"; break;
        }
    }
    lv_label_set_text_static(nav->hint, hint);
    if (detail && nav->device_focus > 0 && nav->device_focus <= count) {
        const esp_openclaw_device_t *device = &nav->devices->devices[nav->device_focus - 1];
        lv_label_set_text(nav->heading, device->name);
        lv_label_set_text_fmt(nav->preview_text, "%s\n\nModel\n%s\n\nConnection: %s\nPairing: %s",
            device->name,
            device->model[0] != '\0' ? device->model : "Unknown",
            !device->connected_known ? "Unknown" : device->connected ? "Online" : "Offline",
            !device->paired_known ? "Unknown" : device->paired ? "Paired" : "Not paired");
        lv_label_set_text_static(nav->hint, "Read only");
    }
    for (unsigned i = 0; i < 3; ++i) {
        unsigned index = nav->first + i;
        lv_obj_set_hidden(nav->rows[i], detail || index >= nav->count);
        if (detail || index >= nav->count) continue;
        lv_obj_set_pos(nav->rows[i], 38, 64 + (int)i * 44);
        lv_obj_set_size(nav->rows[i], 164, 36);
        lv_obj_set_pos(nav->icons[i], 12, 9);
        lv_obj_set_pos(nav->names[i], 40, 9);
        lv_obj_set_width(nav->names[i], 112);
        const char *name, *icon;
        if (local) {
            const char *names[] = {"Control", "Display", "Connection"};
            const char *icons[] = {LV_SYMBOL_SETTINGS, LV_SYMBOL_IMAGE, LV_SYMBOL_WIFI};
            name = names[index]; icon = icons[index];
        } else if (index == 0) { name = "This Dial"; icon = LV_SYMBOL_HOME; }
        else if (index <= count) {
            const esp_openclaw_device_t *device = &nav->devices->devices[index - 1];
            name = device->name;
            icon = !device->connected_known ? "?" : device->connected ? LV_SYMBOL_OK : LV_SYMBOL_MINUS;
        } else { name = index == count + 1 ? "Refresh" : "Back"; icon = index == count + 1 ? LV_SYMBOL_REFRESH : LV_SYMBOL_LEFT; }
        lv_label_set_text(nav->names[i], name);
        lv_label_set_text_static(nav->icons[i], icon);
        bool selected = index == nav->focus;
        lv_obj_set_style_bg_color(nav->rows[i], lv_color_hex(selected ? 0xf4efee : 0x302535), 0);
        lv_color_t color = lv_color_hex(selected ? 0x302131 : 0xf4efee);
        lv_obj_set_style_text_color(nav->names[i], color, 0);
        lv_obj_set_style_text_color(nav->icons[i], color, 0);
    }
}

static void render(dial_navigation_t *nav)
{
    lv_obj_set_y(nav->description, 80);
    lv_obj_set_hidden(nav->detail_avatar, true);
    lv_obj_set_hidden(nav->detail_initials, true);
    lv_obj_set_hidden(nav->messaging_icon, true);
    lv_obj_set_hidden(nav->messaging_status, true);
    lv_obj_set_hidden(nav->messaging_detail, true);
    if (nav->view != DIAL_NAV_AGENTS) {
        for (unsigned i = 0; i < 3; ++i) {
            lv_obj_set_hidden(nav->avatars[i], true);
            lv_obj_set_hidden(nav->icons[i], false);
        }
    }
    if (dial_navigation_has_devices(nav) || nav->view == DIAL_NAV_LOCAL_DEVICE) {
        render_devices(nav);
        return;
    }
    if (nav->view == DIAL_NAV_DISPLAY) {
        render_display(nav);
        return;
    }
    if (nav->view == DIAL_NAV_MESSAGING) {
        render_messaging(nav);
        return;
    }
    if (reply_page(nav->view)) {
        render_reply(nav);
        return;
    }
    if (nav->view == DIAL_NAV_ACTIVITY) {
        lv_label_set_text_static(nav->heading, "Activity");
        lv_obj_set_hidden(nav->description, false);
        lv_obj_set_y(nav->description, 64);
        lv_obj_set_height(nav->description, 20);
        lv_obj_set_style_text_font(nav->description, &lv_font_montserrat_14, 0);
        lv_label_set_long_mode(nav->description, LV_LABEL_LONG_DOT);
        lv_label_set_text_static(nav->description, dial_activity_title(nav->activity.stage));
        lv_obj_set_hidden(nav->identity, true);
        for (unsigned i = 0; i < 3; ++i) lv_obj_set_hidden(nav->rows[i], true);
        lv_obj_set_hidden(nav->preview_body, false);
        lv_obj_set_pos(nav->preview_body, 44, 90);
        lv_obj_set_height(nav->preview_body, 96);
        lv_obj_set_hidden(nav->hint, false);
        lv_label_set_text_static(nav->hint, "Turn to read");
        if (nav->activity.stage == DIAL_ACTIVITY_EMPTY)
            lv_label_set_text_static(nav->preview_text, "Control requests appear here.\n\nOnly the latest request is kept until restart.");
        else {
            const char *message = nav->activity.message[0] != '\0' ? nav->activity.message :
                nav->activity.stage == DIAL_ACTIVITY_SENDING ? "Sending to the configured control." :
                nav->activity.stage == DIAL_ACTIVITY_COMPLETED ? "The agent reported success." :
                nav->activity.stage == DIAL_ACTIVITY_FAILED ? "The agent reported failure." :
                dial_activity_title(nav->activity.stage);
            lv_label_set_text_fmt(nav->preview_text, "%s\n%d%s\n\n%s\n\nLatest request\nUntil restart",
                nav->activity.label, nav->activity.value, nav->activity.unit, message);
        }
        return;
    }
    if (nav->view == DIAL_NAV_CONNECTION) {
        lv_label_set_text_static(nav->heading, "Connection");
        lv_obj_set_hidden(nav->preview_body, false);
        lv_obj_set_pos(nav->preview_body, 44, 70);
        lv_obj_set_height(nav->preview_body, 116);
        lv_obj_set_hidden(nav->description, true);
        lv_obj_set_hidden(nav->identity, true);
        lv_obj_set_hidden(nav->hint, false);
        lv_label_set_text_static(nav->hint, "Turn to read");
        for (unsigned i = 0; i < 3; ++i) lv_obj_set_hidden(nav->rows[i], true);
        char wifi[384], details[640];
        openclaw_wifi_details_format(wifi, sizeof(wifi), &nav->wifi);
        snprintf(details, sizeof(details), "%s\n\nGateway %s\n\n%s", wifi,
            nav->gateway_connected ? "connected" : "offline",
            nav->gateway_connected ? "This Dial has an active OpenClaw connection." :
            "After Wi-Fi connects, check that the Gateway is reachable. Use USB setup if the destination or access has changed.");
        const char *value = nav->connection_known ? details : "Reading connection...";
        if (strcmp(lv_label_get_text(nav->preview_text), value) != 0)
            lv_label_set_text(nav->preview_text, value);
        return;
    }
    lv_obj_set_y(nav->identity, 142);
    bool detail = nav->view == DIAL_NAV_DETAIL;
    bool sessions = nav->view == DIAL_NAV_SESSIONS;
    bool preview = nav->view == DIAL_NAV_PREVIEW;
    bool message = (nav->view == DIAL_NAV_AGENTS || sessions) && agent_count(nav) == 0;
    nav->count = detail || preview ? 1 : nav->view == DIAL_NAV_MENU ? 5 : nav->view == DIAL_NAV_SETTINGS ? 4 :
        agent_count(nav) + has_next(nav) + has_previous(nav) + 2;
    if (!detail && !preview && nav->focus >= nav->count) nav->focus = 0;
    unsigned visible = message ? 2 : 3;
    nav->first = nav->focus / visible * visible;
    if ((nav->view == DIAL_NAV_MENU || nav->view == DIAL_NAV_SETTINGS) && nav->first + visible > nav->count)
        nav->first = nav->count - visible;
    lv_label_set_text_static(nav->heading, preview ? "Preview" : sessions ? "Chats" : nav->view == DIAL_NAV_MENU ? "Menu" : nav->view == DIAL_NAV_SETTINGS ? "Settings" : "Agents");
    if (sessions && nav->conversations != NULL && nav->conversations->agent.name[0] != '\0')
        lv_label_set_text(nav->heading, nav->conversations->agent.name);
    else if (preview && nav->conversations != NULL && nav->conversations->selected < nav->conversations->page.count)
        lv_label_set_text(nav->heading, nav->conversations->page.sessions[nav->conversations->selected].title);
    lv_obj_set_hidden(nav->preview_body, !preview);
    lv_obj_set_pos(nav->preview_body, 44, 70);
    lv_obj_set_height(nav->preview_body, preview && reply_available(nav) ? 100 : 112);
    lv_obj_set_hidden(nav->description, !detail && !message);
    lv_obj_set_hidden(nav->identity, !detail);
    lv_obj_set_hidden(nav->hint, message || (preview && reply_available(nav)));
    bool readable = preview && nav->conversations != NULL && nav->preview_state == DIAL_AGENTS_READY &&
        nav->conversations->preview.status == ESP_OPENCLAW_PREVIEW_OK && nav->conversations->preview.count != 0;
    lv_label_set_text_static(nav->hint, preview ? readable ? "Turn to read" : "Press: back" : detail ? "Press: open" : sessions ? "Chats / turn" : "Turn + press");
    if (nav->view == DIAL_NAV_MENU)
        lv_label_set_text_fmt(nav->hint, "Turn  %u / %u", nav->focus + 1, nav->count);
    if (preview) {
        const dial_conversations_snapshot_t *view = nav->conversations;
        const char *roles[] = {"You", "Assistant", "Tool", "System", "Other"};
        char content[1100] = {0};
        size_t used = 0;
        if (view != NULL && nav->preview_state == DIAL_AGENTS_READY) {
            if (view->preview.status == ESP_OPENCLAW_PREVIEW_OK) {
                for (size_t i = 0; i < view->preview.count; ++i) {
                    const esp_openclaw_preview_item_t *entry = &view->preview.items[i];
                    size_t remaining = sizeof(content) - used;
                    int written = snprintf(content + used, remaining, "%s\n%s\n\n", roles[entry->role], entry->text);
                    if (written < 0 || (size_t)written >= remaining) break;
                    used += (size_t)written;
                }
            } else {
                snprintf(content, sizeof(content), "%s", view->preview.status == ESP_OPENCLAW_PREVIEW_EMPTY ? "No messages yet" :
                    view->preview.status == ESP_OPENCLAW_PREVIEW_MISSING ? "Conversation unavailable" : "Couldn't read this conversation");
            }
        } else snprintf(content, sizeof(content), "%s", state_message(nav->preview_state, true));
        lv_label_set_text(nav->preview_text, content);
        lv_obj_scroll_to_y(nav->preview_body, 0, LV_ANIM_OFF);
    }
    if (detail) {
        lv_obj_set_y(nav->description, 110);
        lv_obj_set_y(nav->identity, 132);
        lv_obj_set_height(nav->description, 20);
        lv_obj_set_style_text_font(nav->description, &lv_font_montserrat_16, 0);
        lv_label_set_long_mode(nav->description, LV_LABEL_LONG_SCROLL_CIRCULAR);
        const esp_openclaw_agent_t *agent = &nav->snapshot->page.agents[nav->focus];
        render_avatar(nav->detail_initials, nav->detail_avatar, agent);
        lv_label_set_text(nav->description, agent->name);
        lv_label_set_text(nav->identity, agent->id);
    } else if (message) {
        lv_obj_set_style_text_font(nav->description, &lv_font_montserrat_14, 0);
        lv_label_set_long_mode(nav->description, LV_LABEL_LONG_WRAP);
        lv_label_set_text_static(nav->description,
            nav->state == DIAL_AGENTS_EMPTY && nav->snapshot->page.total == 0
            && !sessions ? "No agents configured" : state_message(sessions ? nav->conversation_state : nav->state, sessions));
        lv_obj_set_height(nav->description, 68);
    }
    for (unsigned i = 0; i < 3; ++i) {
        unsigned index = nav->first + i;
        bool hidden = preview || (detail ? i != 0 : index >= nav->count || (message && i == 2));
        lv_obj_set_hidden(nav->rows[i], hidden);
        if (hidden) continue;
        lv_obj_set_pos(nav->rows[i], message ? 58 : 38, detail ? 156 : message ? 150 + (int)i * 34 : 64 + (int)i * 44);
        lv_obj_set_size(nav->rows[i], message ? 124 : 164, detail ? 32 : message ? 28 : 36);
        lv_obj_set_pos(nav->icons[i], 12, message ? 5 : 9);
        lv_obj_set_pos(nav->names[i], 40, message ? 5 : 9);
        lv_obj_set_width(nav->names[i], message ? 76 : 112);
        const char *name, *icon;
        if (detail) { name = "Conversations"; icon = LV_SYMBOL_LIST; }
        else item(nav, index, &name, &icon);
        lv_label_set_text(nav->names[i], name);
        if (nav->view == DIAL_NAV_AGENTS && index < agent_count(nav)) {
            render_avatar(nav->icons[i], nav->avatars[i], &nav->snapshot->page.agents[index]);
        } else {
            lv_obj_set_hidden(nav->avatars[i], true);
            lv_obj_set_hidden(nav->icons[i], false);
            lv_label_set_text_static(nav->icons[i], icon);
        }
        bool focused = detail || index == nav->focus;
        lv_obj_set_style_bg_color(nav->rows[i], lv_color_hex(focused ? 0xf4efee : 0x302535), 0);
        lv_color_t color = lv_color_hex(focused ? 0x302131 : 0xf4efee);
        lv_obj_set_style_text_color(nav->names[i], color, 0);
        lv_obj_set_style_text_color(nav->icons[i], color, 0);
    }
    if (preview && reply_available(nav)) reply_button(nav, 0, "Reply", LV_SYMBOL_UP, true);
}

void dial_navigation_open(dial_navigation_t *nav, bool agents)
{
    nav->view = agents ? DIAL_NAV_AGENTS : DIAL_NAV_MENU;
    nav->focus = 0;
    lv_obj_set_hidden(nav->root, false);
    changed(nav);
    if (agents) nav->handler(DIAL_NAV_REFRESH, 0, nav->context);
}

void dial_navigation_update(dial_navigation_t *nav, const dial_agents_snapshot_t *snapshot)
{
    nav->snapshot = snapshot;
    if (nav->state == snapshot->state && nav->revision == snapshot->revision) return;
    nav->state = snapshot->state;
    nav->revision = snapshot->revision;
    /* A replacement roster may reorder identities. Never keep a detail or a
     * held activation attached to the old row number. */
    if (nav->view == DIAL_NAV_AGENTS || nav->view == DIAL_NAV_DETAIL ||
        nav->view == DIAL_NAV_SESSIONS || nav->view == DIAL_NAV_PREVIEW) {
        nav->view = DIAL_NAV_AGENTS;
        nav->focus = 0;
        changed(nav);
    } else if (reply_page(nav->view)) changed(nav);
}

void dial_navigation_conversations_update(dial_navigation_t *nav, const dial_conversations_snapshot_t *snapshot)
{
    nav->conversations = snapshot;
    if (nav->conversation_revision == snapshot->revision && nav->preview_revision == snapshot->preview_revision &&
        nav->conversation_state == snapshot->state && nav->preview_state == snapshot->preview_state) return;
    bool replaced = nav->conversation_revision != snapshot->revision;
    nav->conversation_revision = snapshot->revision;
    nav->preview_revision = snapshot->preview_revision;
    nav->conversation_state = snapshot->state;
    nav->preview_state = snapshot->preview_state;
    if (nav->view == DIAL_NAV_SESSIONS || nav->view == DIAL_NAV_PREVIEW) {
        if (replaced) { nav->view = DIAL_NAV_SESSIONS; nav->focus = 0; }
        changed(nav);
    } else if (reply_page(nav->view)) changed(nav);
}

void dial_navigation_devices_update(dial_navigation_t *nav, const esp_openclaw_devices_snapshot_t *snapshot)
{
    nav->devices = snapshot;
    unsigned revision = snapshot != NULL ? snapshot->revision : 0;
    esp_openclaw_devices_state_t state = snapshot != NULL ? snapshot->state : ESP_OPENCLAW_DEVICES_ERROR;
    if (nav->device_revision == revision && nav->device_state == state) return;
    nav->device_revision = revision;
    nav->device_state = state;
    if (dial_navigation_has_devices(nav)) {
        nav->view = DIAL_NAV_DEVICES;
        nav->focus = 0;
        changed(nav);
    }
}

void dial_navigation_open_devices(dial_navigation_t *nav)
{
    nav->view = DIAL_NAV_DEVICES;
    nav->focus = 0;
    lv_obj_set_hidden(nav->root, false);
    changed(nav);
    nav->handler(DIAL_NAV_DEVICES_REFRESH, 0, nav->context);
}

void dial_navigation_open_local_device(dial_navigation_t *nav)
{
    nav->view = DIAL_NAV_LOCAL_DEVICE;
    nav->focus = 0;
    lv_obj_set_hidden(nav->root, false);
    changed(nav);
}

bool dial_navigation_show_reader(dial_navigation_t *nav, const dial_agents_snapshot_t *agents,
                                 const dial_conversations_snapshot_t *view, bool preview)
{
    if (view == NULL || agents == NULL || agents->state != DIAL_AGENTS_READY || view->agent.id[0] == '\0' ||
        (preview && (view->state != DIAL_AGENTS_READY || view->selected >= view->page.count ||
                     view->preview_state == DIAL_AGENTS_IDLE))) return false;
    unsigned agent = 0;
    while (agent < agents->page.count && strcmp(agents->page.agents[agent].id, view->agent.id) != 0) ++agent;
    if (agent == agents->page.count) return false;
    /* Validate before updates: a rejected command must not cancel control input. */
    dial_navigation_update(nav, agents);
    dial_navigation_conversations_update(nav, view);
    nav->agent_focus = agent;
    nav->session_focus = view->selected;
    nav->view = preview ? DIAL_NAV_PREVIEW : DIAL_NAV_SESSIONS;
    /* Selection is page-local; paging controls follow the session rows. */
    nav->focus = view->selected;
    changed(nav);
    return true;
}

void dial_navigation_move(dial_navigation_t *nav, int direction)
{
    if (nav->view == DIAL_NAV_MESSAGING) return;
    if (nav->view == DIAL_NAV_DISPLAY) {
        int percent = nav->brightness + (direction > 0 ? 5 : -5);
        if (percent < 10) percent = nav->brightness < 10 && direction < 0 ? nav->brightness : 10;
        if (percent > 100) percent = 100;
        if (percent != nav->brightness || nav->brightness_failed)
            nav->handler(DIAL_NAV_BRIGHTNESS, (size_t)percent, nav->context);
        return;
    }
    if (nav->view == DIAL_NAV_PREVIEW || nav->view == DIAL_NAV_ACTIVITY || nav->view == DIAL_NAV_REPLY_REVIEW ||
        nav->view == DIAL_NAV_DEVICE_DETAIL || nav->view == DIAL_NAV_CONNECTION) {
        lv_obj_scroll_by_bounded(nav->preview_body, 0, direction > 0 ? -24 : 24, LV_ANIM_OFF);
        return;
    }
    if (nav->view == DIAL_NAV_DETAIL) return;
    if (direction > 0 && nav->focus + 1 < nav->count) ++nav->focus;
    else if (direction < 0 && nav->focus > 0) --nav->focus;
    render(nav);
}

void dial_navigation_activate(dial_navigation_t *nav)
{
    if (nav->view == DIAL_NAV_REPLY_REVIEW) {
        nav->view = DIAL_NAV_REPLY_ACTIONS;
        nav->focus = 0;
        changed(nav);
        return;
    }
    if (nav->view == DIAL_NAV_REPLY_PICKER) {
        if (nav->messaging.state != DIAL_MESSAGING_READY) {
            dial_navigation_open_settings(nav, DIAL_NAV_MESSAGING);
        } else {
            nav->handler(DIAL_NAV_REPLY_BEGIN, nav->focus, nav->context);
            if (reply_busy(nav) || (nav->reply.state != DIAL_REPLY_IDLE &&
                nav->reply.preview_revision == nav->reply_preview_revision))
                nav->view = DIAL_NAV_REPLY_REVIEW;
            nav->focus = 0;
            changed(nav);
        }
        return;
    }
    if (nav->view == DIAL_NAV_REPLY_ACTIONS) {
        reply_action_t actions[3];
        unsigned count = reply_actions(nav, actions);
        if (nav->focus >= count) return;
        switch (actions[nav->focus]) {
        case REPLY_SEND:
            nav->handler(DIAL_NAV_REPLY_SEND, nav->reply.revision, nav->context);
            nav->view = DIAL_NAV_REPLY_REVIEW;
            break;
        case REPLY_PRESETS:
            nav->handler(DIAL_NAV_REPLY_CLOSE, nav->reply.revision, nav->context);
            nav->view = DIAL_NAV_REPLY_PICKER;
            break;
        case REPLY_READ:
            nav->handler(DIAL_NAV_REPLY_READ, nav->reply.revision, nav->context);
            return;
        case REPLY_ACCESS: dial_navigation_open_settings(nav, DIAL_NAV_MESSAGING); return;
        case REPLY_REVIEW: nav->view = DIAL_NAV_REPLY_REVIEW; break;
        }
        nav->focus = 0;
        changed(nav);
        return;
    }
    if (nav->view == DIAL_NAV_PREVIEW && reply_available(nav)) {
        nav->reply_preview_revision = nav->preview_revision;
        nav->reply_action_failed = false;
        nav->view = reply_busy(nav) || (nav->reply.state != DIAL_REPLY_IDLE &&
            nav->reply.preview_revision == nav->preview_revision) ? DIAL_NAV_REPLY_REVIEW : DIAL_NAV_REPLY_PICKER;
        nav->focus = 0;
        changed(nav);
        return;
    }
    if (nav->view == DIAL_NAV_MESSAGING) {
        nav->handler((nav->messaging.state == DIAL_MESSAGING_READY || messaging_pending(nav->messaging.state))
            ? DIAL_NAV_MESSAGING_CANCEL : DIAL_NAV_MESSAGING_REQUEST, nav->messaging.revision, nav->context);
        return;
    }
    if (nav->view == DIAL_NAV_DEVICE_DETAIL) { back(nav); return; }
    if (nav->view == DIAL_NAV_LOCAL_DEVICE) {
        if (nav->focus == 0) nav->handler(DIAL_NAV_CONTROL, 0, nav->context);
        else dial_navigation_open_settings(nav, nav->focus == 1 ? DIAL_NAV_DISPLAY : DIAL_NAV_CONNECTION);
        return;
    }
    if (nav->view == DIAL_NAV_DEVICES) {
        if (nav->focus == 0) dial_navigation_open_local_device(nav);
        else if (nav->focus <= device_count(nav)) {
            nav->device_focus = nav->focus;
            nav->view = DIAL_NAV_DEVICE_DETAIL;
            lv_obj_scroll_to_y(nav->preview_body, 0, LV_ANIM_OFF);
            changed(nav);
        } else if (nav->focus == device_count(nav) + 1) nav->handler(DIAL_NAV_DEVICES_REFRESH, 0, nav->context);
        else back(nav);
        return;
    }
    if (nav->view == DIAL_NAV_DISPLAY || nav->view == DIAL_NAV_CONNECTION) { back(nav); return; }
    if (nav->view == DIAL_NAV_SETTINGS) {
        if (nav->focus < 3) dial_navigation_open_settings(nav, nav->focus == 0 ? DIAL_NAV_DISPLAY :
            nav->focus == 1 ? DIAL_NAV_CONNECTION : DIAL_NAV_MESSAGING);
        else back(nav);
        return;
    }
    if (nav->view == DIAL_NAV_PREVIEW || nav->view == DIAL_NAV_ACTIVITY) { back(nav); return; }
    if (nav->view == DIAL_NAV_DETAIL) {
        nav->agent_focus = nav->focus;
        nav->view = DIAL_NAV_SESSIONS;
        nav->focus = 0;
        nav->handler(DIAL_NAV_CONVERSATIONS, nav->agent_focus, nav->context);
        changed(nav);
        return;
    }
    if (nav->view == DIAL_NAV_MENU) {
        if (nav->focus == 1) nav->handler(DIAL_NAV_RESUME, 0, nav->context);
        else if (nav->focus == 2) dial_navigation_open_devices(nav);
        else if (nav->focus == 3) dial_navigation_open_activity(nav);
        else if (nav->focus == 4) dial_navigation_open_settings(nav, DIAL_NAV_SETTINGS);
        else dial_navigation_open(nav, true);
        return;
    }
    unsigned index = nav->focus;
    if (index < agent_count(nav)) {
        if (nav->view == DIAL_NAV_SESSIONS) {
            nav->session_focus = index;
            nav->view = DIAL_NAV_PREVIEW;
            nav->handler(DIAL_NAV_READ, index, nav->context);
        } else nav->view = DIAL_NAV_DETAIL;
        changed(nav);
        return;
    }
    index -= agent_count(nav);
    if (has_next(nav)) {
        if (index == 0) {
            if (nav->view == DIAL_NAV_SESSIONS)
                nav->handler(DIAL_NAV_SESSIONS_REFRESH, nav->conversations->page.next_offset, nav->context);
            else nav->handler(DIAL_NAV_REFRESH, nav->snapshot->page.offset + nav->snapshot->page.count, nav->context);
            return;
        }
        --index;
    }
    if (has_previous(nav)) {
        if (index == 0) {
            size_t offset = nav->view == DIAL_NAV_SESSIONS ? nav->conversations->page.offset : nav->snapshot->page.offset;
            nav->handler(nav->view == DIAL_NAV_SESSIONS ? DIAL_NAV_SESSIONS_REFRESH : DIAL_NAV_REFRESH, offset > ESP_OPENCLAW_DIRECTORY_PAGE_SIZE ? offset - ESP_OPENCLAW_DIRECTORY_PAGE_SIZE : 0, nav->context);
            return;
        }
        --index;
    }
    if (index == 0) nav->handler(nav->view == DIAL_NAV_SESSIONS ? DIAL_NAV_SESSIONS_REFRESH : DIAL_NAV_REFRESH, 0, nav->context);
    else back(nav);
}

void dial_navigation_open_settings(dial_navigation_t *nav, dial_navigation_view_t page)
{
    if (page != DIAL_NAV_SETTINGS && page != DIAL_NAV_DISPLAY && page != DIAL_NAV_CONNECTION && page != DIAL_NAV_MESSAGING) return;
    nav->settings_parent = reply_page(nav->view) ? nav->view :
        nav->view == DIAL_NAV_LOCAL_DEVICE ? DIAL_NAV_LOCAL_DEVICE : DIAL_NAV_SETTINGS;
    nav->view = page;
    nav->focus = 0;
    lv_obj_set_hidden(nav->root, false);
    changed(nav);
    if (page == DIAL_NAV_CONNECTION) lv_obj_scroll_to_y(nav->preview_body, 0, LV_ANIM_OFF);
}

void dial_navigation_set_brightness(dial_navigation_t *nav, int percent, bool failed)
{
    if (nav->brightness == percent && nav->brightness_failed == failed) return;
    nav->brightness = percent;
    nav->brightness_failed = failed;
    if (nav->view == DIAL_NAV_DISPLAY) changed(nav);
}

void dial_navigation_set_connection(dial_navigation_t *nav, const esp_openclaw_node_wifi_status_t *wifi, bool gateway)
{
    nav->connection_known = true;
    nav->wifi = *wifi;
    nav->gateway_connected = gateway;
    if (nav->view == DIAL_NAV_CONNECTION) render(nav);
}

void dial_navigation_messaging_update(dial_navigation_t *nav, const dial_messaging_snapshot_t *snapshot)
{
    if (nav->messaging.revision == snapshot->revision && nav->messaging.state == snapshot->state &&
        strcmp(nav->messaging.request_id, snapshot->request_id) == 0) return;
    nav->messaging = *snapshot;
    nav->messaging_action_failed = false;
    /* A held Request or Cancel belongs to the exact permission observation. */
    if (nav->view == DIAL_NAV_MESSAGING || reply_page(nav->view)) changed(nav);
}

void dial_navigation_messaging_result(dial_navigation_t *nav, bool accepted)
{
    nav->messaging_action_failed = !accepted;
    if (nav->view == DIAL_NAV_MESSAGING) render(nav);
}

void dial_navigation_reply_update(dial_navigation_t *nav, const dial_reply_snapshot_t *snapshot)
{
    if (memcmp(&nav->reply, snapshot, sizeof(*snapshot)) == 0) return;
    nav->reply = *snapshot;
    nav->reply_action_failed = false;
    if (reply_page(nav->view)) changed(nav);
}

void dial_navigation_reply_result(dial_navigation_t *nav, bool accepted)
{
    nav->reply_action_failed = !accepted;
    if (reply_page(nav->view)) render(nav);
}

void dial_navigation_open_activity(dial_navigation_t *nav)
{
    nav->view = DIAL_NAV_ACTIVITY;
    lv_obj_set_hidden(nav->root, false);
    changed(nav);
    lv_obj_scroll_to_y(nav->preview_body, 0, LV_ANIM_OFF);
}

void dial_navigation_activity_update(dial_navigation_t *nav, const dial_activity_t *activity)
{
    if (nav->activity.revision == activity->revision) return;
    nav->activity = *activity;
    if (nav->view == DIAL_NAV_ACTIVITY) changed(nav);
}
