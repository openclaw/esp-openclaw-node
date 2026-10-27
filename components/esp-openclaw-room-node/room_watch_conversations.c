#include "room_watch_conversations.h"

#include <stdint.h>
#include <stdio.h>

static lv_obj_t *root, *back_text, *heading, *list, *message, *body, *body_text;
static lv_obj_t *previous, *next, *refresh;
static lv_obj_t *rows[ESP_OPENCLAW_DIRECTORY_PAGE_SIZE], *titles[ESP_OPENCLAW_DIRECTORY_PAGE_SIZE];
static lv_obj_t *summaries[ESP_OPENCLAW_DIRECTORY_PAGE_SIZE];
static const room_conversations_snapshot_t *current;
static bool preview, painted_preview, reset_scroll;
static unsigned revision, preview_revision;
static room_agents_state_t state, preview_state;

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int width)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, width);
    lv_obj_set_style_text_font(object, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(0xf4efee), 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
    lv_label_set_text(object, text);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    return object;
}

static lv_obj_t *button(lv_obj_t *parent, int x, int y, int width, int height,
                        int action, lv_event_cb_t handler)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, lv_color_hex(0x302535), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(object, 14, 0);
    lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(object, handler, LV_EVENT_ALL, (void *)(intptr_t)action);
    return object;
}

void room_watch_conversations_create(lv_obj_t *parent, lv_event_cb_t handler)
{
    root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x15101b), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_t *back = button(root, 26, 18, 116, 44, ROOM_CHAT_BACK, handler);
    back_text = label(back, LV_SYMBOL_LEFT " Agent", 10, 14, 100);
    heading = label(root, "Conversations", 26, 76, 316);
#if LV_FONT_MONTSERRAT_20
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_20, 0);
#endif
    list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 26, 112);
    lv_obj_set_size(list, 316, 194);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(list, lv_color_hex(0xb9aabe), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_60, LV_PART_SCROLLBAR);
    for (unsigned i = 0; i < ESP_OPENCLAW_DIRECTORY_PAGE_SIZE; ++i) {
        rows[i] = button(list, 0, (int)i * 68, 310, 60, (int)i, handler);
        titles[i] = label(rows[i], "", 14, 10, 282);
        summaries[i] = label(rows[i], "", 14, 34, 282);
        lv_obj_set_style_text_color(summaries[i], lv_color_hex(0xb9aabe), 0);
    }
    message = label(root, "", 32, 160, 304);
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    previous = button(root, 26, 318, 66, 44, ROOM_CHAT_PREVIOUS, handler);
    label(previous, LV_SYMBOL_LEFT, 26, 14, 24);
    refresh = button(root, 102, 318, 164, 44, ROOM_CHAT_REFRESH, handler);
    lv_obj_t *caption = label(refresh, LV_SYMBOL_REFRESH "  Refresh", 14, 14, 136);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
    next = button(root, 276, 318, 66, 44, ROOM_CHAT_NEXT, handler);
    label(next, LV_SYMBOL_RIGHT, 26, 14, 24);
    body = lv_obj_create(root);
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, 26, 112);
    lv_obj_set_size(body, 316, 250);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(body, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(body, lv_color_hex(0xb9aabe), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(body, LV_OPA_60, LV_PART_SCROLLBAR);
    body_text = label(body, "", 0, 0, 306);
    lv_label_set_long_mode(body_text, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
}

void room_watch_conversations_reset(bool show_preview)
{
    preview = show_preview;
    reset_scroll = true;
    current = NULL;
}

static const char *failure_text(room_agents_state_t value)
{
    switch (value) {
    case ROOM_AGENTS_LOADING: return "Loading...";
    case ROOM_AGENTS_OFFLINE: return "Gateway offline\n\nReconnect, then choose the agent again.";
    case ROOM_AGENTS_EMPTY: return "No conversations yet.";
    case ROOM_AGENTS_TIMEOUT: return "Request timed out.\n\nGo back and refresh to try again.";
    case ROOM_AGENTS_TOO_LARGE: return "This conversation is too large.\n\nOpen it in the mobile app.";
    case ROOM_AGENTS_IDLE: return "Choose the agent again to browse conversations.";
    default: return "Couldn't load conversations.\n\nGo back and refresh to try again.";
    }
}

void room_watch_conversations_render(const room_conversations_snapshot_t *snapshot, bool hidden)
{
    if (root == NULL) return;
    lv_obj_set_hidden(root, hidden);
    if (hidden) { current = NULL; return; }
    if (snapshot == NULL) {
        painted_preview = preview;
        lv_label_set_text_static(back_text, LV_SYMBOL_LEFT " Agent");
        lv_label_set_text_static(heading, "Conversations");
        lv_label_set_text_static(message, "Reader unavailable.\n\nGo back and try again.");
        lv_obj_set_hidden(message, false);
        lv_obj_set_hidden(list, true);
        lv_obj_set_hidden(body, true);
        lv_obj_set_hidden(previous, true);
        lv_obj_set_hidden(next, true);
        lv_obj_set_hidden(refresh, true);
        return;
    }
    bool changed = current == NULL || revision != snapshot->revision || state != snapshot->state;
    bool content_changed = changed || preview_revision != snapshot->preview_revision || preview_state != snapshot->preview_state;
    if (current != NULL && revision != snapshot->revision) preview = false;
    current = snapshot;
    revision = snapshot->revision;
    state = snapshot->state;
    preview_revision = snapshot->preview_revision;
    preview_state = snapshot->preview_state;
    lv_label_set_text_static(back_text, preview ? LV_SYMBOL_LEFT " Chats" : LV_SYMBOL_LEFT " Agent");
    lv_label_set_text(heading, preview && snapshot->selected < snapshot->page.count
        ? snapshot->page.sessions[snapshot->selected].title : "Conversations");
    bool ready = state == ROOM_AGENTS_READY && snapshot->page.count > 0;
    lv_obj_set_hidden(list, preview || !ready);
    lv_obj_set_hidden(message, preview || ready);
    lv_obj_set_hidden(body, !preview);
    lv_obj_set_hidden(previous, preview);
    lv_obj_set_hidden(next, preview);
    lv_obj_set_hidden(refresh, preview);
    if (preview && (content_changed || reset_scroll)) {
        const char *roles[] = {"You", "Assistant", "Tool", "System", "Other"};
        char text[1100] = {0};
        size_t used = 0;
        if (preview_state != ROOM_AGENTS_READY) {
            snprintf(text, sizeof(text), "%s", failure_text(preview_state));
        } else if (snapshot->preview.status == ESP_OPENCLAW_PREVIEW_OK) {
            for (size_t i = 0; i < snapshot->preview.count; ++i) {
                const esp_openclaw_preview_item_t *item = &snapshot->preview.items[i];
                size_t remaining = sizeof(text) - used;
                int written = snprintf(text + used, remaining, "%s\n%s\n\n", roles[item->role], item->text);
                if (written < 0 || (size_t)written >= remaining) break;
                used += (size_t)written;
            }
        } else snprintf(text, sizeof(text), "%s", snapshot->preview.status == ESP_OPENCLAW_PREVIEW_EMPTY
            ? "No messages yet." : snapshot->preview.status == ESP_OPENCLAW_PREVIEW_MISSING
            ? "Conversation unavailable." : "Couldn't read this conversation.");
        lv_label_set_text(body_text, text);
        lv_obj_scroll_to_y(body, 0, LV_ANIM_OFF);
    } else if (!preview) {
        lv_obj_set_state(previous, LV_STATE_DISABLED, state == ROOM_AGENTS_LOADING || snapshot->page.offset == 0);
        lv_obj_set_state(next, LV_STATE_DISABLED, !ready || !snapshot->page.has_more);
        lv_obj_set_state(refresh, LV_STATE_DISABLED, state == ROOM_AGENTS_LOADING);
        if (!ready) lv_label_set_text_static(message, failure_text(state));
        for (size_t i = 0; i < ESP_OPENCLAW_DIRECTORY_PAGE_SIZE; ++i) {
            lv_obj_set_hidden(rows[i], i >= snapshot->page.count);
            if (i < snapshot->page.count) {
                lv_label_set_text(titles[i], snapshot->page.sessions[i].title);
                lv_label_set_text(summaries[i], snapshot->page.sessions[i].last_message);
            }
        }
        if (changed || reset_scroll) lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    }
    reset_scroll = false;
    painted_preview = preview;
}

bool room_watch_conversations_input_current(void)
{
    return preview == painted_preview;
}

room_chat_action_t room_watch_conversations_activate(int action, size_t *value)
{
    if (!room_watch_conversations_input_current()) return ROOM_CHAT_STAY;
    if (action == ROOM_CHAT_BACK) {
        if (!preview) return ROOM_CHAT_LEAVE;
        preview = false;
        return ROOM_CHAT_STAY;
    }
    if (current == NULL || preview) return ROOM_CHAT_STAY;
    if (action >= 0 && state == ROOM_AGENTS_READY && (size_t)action < current->page.count) {
        *value = (size_t)action;
        preview = true;
        reset_scroll = true;
        return ROOM_CHAT_READ;
    }
    if (state == ROOM_AGENTS_LOADING) return ROOM_CHAT_STAY;
    if (action == ROOM_CHAT_REFRESH) *value = current->page.offset;
    else if (action == ROOM_CHAT_PREVIOUS && current->page.offset > 0)
        *value = current->page.offset > ESP_OPENCLAW_DIRECTORY_PAGE_SIZE ? current->page.offset - ESP_OPENCLAW_DIRECTORY_PAGE_SIZE : 0;
    else if (action == ROOM_CHAT_NEXT && state == ROOM_AGENTS_READY && current->page.has_more)
        *value = current->page.next_offset;
    else return ROOM_CHAT_STAY;
    return ROOM_CHAT_LOAD;
}
