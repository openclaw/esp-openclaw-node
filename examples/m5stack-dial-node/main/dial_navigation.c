#include "dial_navigation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void render(dial_navigation_t *nav);

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
        if (nav->view == DIAL_NAV_PREVIEW) {
            nav->view = DIAL_NAV_SESSIONS;
            nav->focus = nav->session_focus;
            changed(nav);
            return;
        }
        if (nav->view == DIAL_NAV_SESSIONS) {
            nav->view = DIAL_NAV_DETAIL;
            nav->focus = nav->agent_focus;
            nav->handler(DIAL_NAV_CLOSE, 0, nav->context);
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
            if (nav->view != DIAL_NAV_DETAIL) nav->focus = nav->first + i;
            dial_navigation_activate(nav);
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
        nav->names[i] = text(nav->rows[i], "", 40, 9, 112);
    }
    nav->hint = text(nav->root, "Turn + press", 74, 198, 92);
    lv_obj_set_style_text_align(nav->hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(nav->hint, lv_color_hex(0xb8b0bd), 0);
    nav->description = text(nav->root, "", 44, 80, 152);
    lv_obj_set_size(nav->description, 152, 104);
    lv_label_set_long_mode(nav->description, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(nav->description, LV_TEXT_ALIGN_CENTER, 0);
    nav->id_caption = text(nav->root, "Agent ID", 64, 116, 112);
    lv_obj_set_style_text_align(nav->id_caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(nav->id_caption, lv_color_hex(0xb8b0bd), 0);
    nav->identity = text(nav->root, "", 44, 142, 152);
    lv_obj_set_style_text_align(nav->identity, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(nav->identity, LV_LABEL_LONG_SCROLL_CIRCULAR);
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
        const char *names[] = {"Agents", "Controls", "Home"};
        const char *icons[] = {LV_SYMBOL_DIRECTORY, LV_SYMBOL_SETTINGS, LV_SYMBOL_HOME};
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

static void render(dial_navigation_t *nav)
{
    bool detail = nav->view == DIAL_NAV_DETAIL;
    bool sessions = nav->view == DIAL_NAV_SESSIONS;
    bool preview = nav->view == DIAL_NAV_PREVIEW;
    bool message = (nav->view == DIAL_NAV_AGENTS || sessions) && agent_count(nav) == 0;
    nav->count = detail || preview ? 1 : nav->view == DIAL_NAV_MENU ? 3 :
        agent_count(nav) + has_next(nav) + has_previous(nav) + 2;
    if (!detail && !preview && nav->focus >= nav->count) nav->focus = 0;
    unsigned visible = message ? 2 : 3;
    nav->first = nav->focus / visible * visible;
    lv_label_set_text_static(nav->heading, preview ? "Preview" : sessions ? "Chats" : nav->view == DIAL_NAV_MENU ? "Menu" : "Agents");
    if (sessions && nav->conversations != NULL && nav->conversations->agent.name[0] != '\0')
        lv_label_set_text(nav->heading, nav->conversations->agent.name);
    else if (preview && nav->conversations != NULL && nav->conversations->selected < nav->conversations->page.count)
        lv_label_set_text(nav->heading, nav->conversations->page.sessions[nav->conversations->selected].title);
    lv_obj_set_hidden(nav->preview_body, !preview);
    lv_obj_set_hidden(nav->description, !detail && !message);
    lv_obj_set_hidden(nav->id_caption, !detail);
    lv_obj_set_hidden(nav->identity, !detail);
    lv_obj_set_hidden(nav->hint, message);
    bool readable = preview && nav->conversations != NULL && nav->preview_state == DIAL_AGENTS_READY &&
        nav->conversations->preview.status == ESP_OPENCLAW_PREVIEW_OK && nav->conversations->preview.count != 0;
    lv_label_set_text_static(nav->hint, preview ? readable ? "Turn to read" : "Press: back" : detail ? "Press: open" : sessions ? "Chats / turn" : "Turn + press");
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
        lv_obj_set_height(nav->description, 20);
        lv_obj_set_style_text_font(nav->description, &lv_font_montserrat_16, 0);
        lv_label_set_long_mode(nav->description, LV_LABEL_LONG_SCROLL_CIRCULAR);
        const esp_openclaw_agent_t *agent = &nav->snapshot->page.agents[nav->focus];
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
            char initial[] = {nav->snapshot->page.agents[index].id[0], '\0'};
            if (initial[0] >= 'a' && initial[0] <= 'z') initial[0] -= 'a' - 'A';
            lv_label_set_text(nav->icons[i], initial);
        } else lv_label_set_text_static(nav->icons[i], icon);
        bool focused = detail || index == nav->focus;
        lv_obj_set_style_bg_color(nav->rows[i], lv_color_hex(focused ? 0xf4efee : 0x302535), 0);
        lv_color_t color = lv_color_hex(focused ? 0x302131 : 0xf4efee);
        lv_obj_set_style_text_color(nav->names[i], color, 0);
        lv_obj_set_style_text_color(nav->icons[i], color, 0);
    }
}

void dial_navigation_open(dial_navigation_t *nav, bool agents)
{
    if (nav->view == DIAL_NAV_SESSIONS || nav->view == DIAL_NAV_PREVIEW) nav->handler(DIAL_NAV_CLOSE, 0, nav->context);
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
    if (nav->view != DIAL_NAV_MENU) {
        nav->view = DIAL_NAV_AGENTS;
        nav->focus = 0;
        changed(nav);
    }
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
    }
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
    nav->focus = preview ? view->selected : 0;
    changed(nav);
    return true;
}

void dial_navigation_move(dial_navigation_t *nav, int direction)
{
    if (nav->view == DIAL_NAV_PREVIEW) {
        lv_obj_scroll_by(nav->preview_body, 0, direction > 0 ? -24 : 24, LV_ANIM_OFF);
        return;
    }
    if (nav->view == DIAL_NAV_DETAIL) return;
    if (direction > 0 && nav->focus + 1 < nav->count) ++nav->focus;
    else if (direction < 0 && nav->focus > 0) --nav->focus;
    render(nav);
}

void dial_navigation_activate(dial_navigation_t *nav)
{
    if (nav->view == DIAL_NAV_PREVIEW) { back(nav); return; }
    if (nav->view == DIAL_NAV_DETAIL) {
        nav->agent_focus = nav->focus;
        nav->view = DIAL_NAV_SESSIONS;
        nav->focus = 0;
        nav->handler(DIAL_NAV_CONVERSATIONS, nav->agent_focus, nav->context);
        changed(nav);
        return;
    }
    if (nav->view == DIAL_NAV_MENU) {
        if (nav->focus == 1) nav->handler(DIAL_NAV_CONTROL, 0, nav->context);
        else if (nav->focus == 2) nav->handler(DIAL_NAV_HOME, 0, nav->context);
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
