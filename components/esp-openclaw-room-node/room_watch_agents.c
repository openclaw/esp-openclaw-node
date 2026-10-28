#include "openclaw_density.h"
#include "openclaw_reading.h"
#include "room_watch_agents.h"
#include "esp_openclaw_avatar.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static lv_obj_t *root, *back_text, *heading, *list, *message, *counter;
static lv_obj_t *rows[ESP_OPENCLAW_DIRECTORY_PAGE_SIZE], *names[ESP_OPENCLAW_DIRECTORY_PAGE_SIZE];
static lv_obj_t *avatars[ESP_OPENCLAW_DIRECTORY_PAGE_SIZE], *ids[ESP_OPENCLAW_DIRECTORY_PAGE_SIZE];
static lv_obj_t *avatar_images[ESP_OPENCLAW_DIRECTORY_PAGE_SIZE], *detail_image;
static lv_image_dsc_t avatar_sources[3];
static lv_obj_t *previous, *next, *refresh, *detail_name, *detail_id, *detail_note, *detail_avatar, *choose, *choose_text;
static lv_obj_t *use_default, *default_text, *conversations;
static const room_agents_snapshot_t *agents;
static unsigned revision;
static room_agents_state_t state;
static bool detail, reset_scroll, can_reset;
static char selected_id[65];

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int width)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, width);
    lv_obj_set_style_text_font(obj, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(0xf4efee), 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
    lv_label_set_text(obj, text);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *button(lv_obj_t *parent, int x, int y, int width, int height,
                        int action, lv_event_cb_t handler)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x302535), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 14, 0);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(obj, handler, LV_EVENT_ALL, (void *)(intptr_t)action);
    return obj;
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

void room_watch_agents_create(lv_obj_t *parent, lv_event_cb_t handler)
{
    for (int kind = ESP_OPENCLAW_AVATAR_FOX; kind <= ESP_OPENCLAW_AVATAR_LOBSTER; ++kind)
        avatar_sources[kind] = (lv_image_dsc_t){
            .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888,
                .w = ESP_OPENCLAW_AVATAR_SIZE, .h = ESP_OPENCLAW_AVATAR_SIZE,
                .stride = ESP_OPENCLAW_AVATAR_SIZE * 4},
            .data_size = ESP_OPENCLAW_AVATAR_SIZE * ESP_OPENCLAW_AVATAR_SIZE * 4,
            .data = esp_openclaw_avatar_pixels(kind),
        };
    root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x15101b), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_t *back = button(root, 26, 18, 104, 44, ROOM_AGENT_BACK, handler);
    back_text = label(back, LV_SYMBOL_LEFT " Menu", 10, 14, 94);
    use_default = button(root, 218, 64, 124, 44, ROOM_AGENT_DEFAULT, handler);
    default_text = label(use_default, "Default", 8, 14, 108);
    lv_obj_set_style_text_align(default_text, LV_TEXT_ALIGN_CENTER, 0);
    heading = label(root, "Agents", 26, 72, 180);
#if LV_FONT_MONTSERRAT_28
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_28, 0);
#endif
    counter = label(root, "", 242, 44, 100);
    lv_obj_set_style_text_align(counter, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(counter, lv_color_hex(0xb9aabe), 0);
    list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 26, 116);
    lv_obj_set_size(list, 316, 204);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    for (unsigned i = 0; i < ESP_OPENCLAW_DIRECTORY_PAGE_SIZE; ++i) {
        rows[i] = button(list, 0, (int)i * 68, 310, 60, (int)i, handler);
        avatars[i] = label(rows[i], "", 12, 12, 36);
        lv_obj_set_height(avatars[i], 36);
        lv_obj_set_style_pad_top(avatars[i], 10, 0);
        lv_obj_set_style_bg_color(avatars[i], lv_color_hex(0x655074), 0);
        lv_obj_set_style_bg_opa(avatars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(avatars[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_text_align(avatars[i], LV_TEXT_ALIGN_CENTER, 0);
        avatar_images[i] = avatar_image(rows[i], 12, 12, 36);
        names[i] = label(rows[i], "", 62, 11, 220);
        ids[i] = label(rows[i], "", 62, 34, 220);
        lv_obj_set_style_text_color(ids[i], lv_color_hex(0xb9aabe), 0);
    }
    message = label(root, "", 32, 160, 304);
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    previous = button(root, 26, 326, 66, 44, ROOM_AGENT_PREVIOUS, handler);
    label(previous, LV_SYMBOL_LEFT, 26, 14, 24);
    refresh = button(root, 102, 326, 164, 44, ROOM_AGENT_REFRESH, handler);
    lv_obj_t *refresh_text = label(refresh, LV_SYMBOL_REFRESH "  Refresh", 14, 14, 136);
    lv_obj_set_style_text_align(refresh_text, LV_TEXT_ALIGN_CENTER, 0);
    next = button(root, 276, 326, 66, 44, ROOM_AGENT_NEXT, handler);
    label(next, LV_SYMBOL_RIGHT, 26, 14, 24);
    detail_avatar = label(root, "", 32, 112, 64);
    lv_obj_set_height(detail_avatar, 64);
    lv_obj_set_style_pad_top(detail_avatar, 23, 0);
    lv_obj_set_style_text_align(detail_avatar, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(detail_avatar, lv_color_hex(0x655074), 0);
    lv_obj_set_style_bg_opa(detail_avatar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(detail_avatar, LV_RADIUS_CIRCLE, 0);
    detail_image = avatar_image(root, 32, 112, 64);
    detail_name = label(root, "", 112, 118, 224);
    lv_label_set_long_mode(detail_name, LV_LABEL_LONG_WRAP);
#if LV_FONT_MONTSERRAT_20
    lv_obj_set_style_text_font(detail_name, &lv_font_montserrat_20, 0);
#endif
    lv_obj_set_height(detail_name, 72);
    detail_id = label(root, "", 32, 188, 304);
    lv_label_set_long_mode(detail_id, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(detail_id, 44);
    choose = button(root, 26, 234, 316, 44, ROOM_AGENT_CHOOSE, handler);
    choose_text = label(choose, "Use for next Talk", 12, 14, 292);
    lv_obj_set_style_text_align(choose_text, LV_TEXT_ALIGN_CENTER, 0);
    conversations = button(root, 26, 288, 316, 44, ROOM_AGENT_CONVERSATIONS, handler);
    lv_obj_t *chat_text = label(conversations, LV_SYMBOL_LIST "  Conversations", 12, 14, 292);
    lv_obj_set_style_text_align(chat_text, LV_TEXT_ALIGN_CENTER, 0);
    detail_note = label(root, "Applies to your next call.\nThe current call stays with its agent.", 32, 342, 304);
    lv_label_set_long_mode(detail_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(detail_note, lv_color_hex(0xb9aabe), 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
}

void room_watch_agents_reset(void)
{
    detail = false;
    reset_scroll = true;
    selected_id[0] = '\0';
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

void room_watch_agents_render(const room_agents_snapshot_t *snapshot, bool hidden,
    const char *target_id, bool expired, bool pending, bool failed, int content_bottom)
{
    if (root == NULL) return;
    lv_obj_set_hidden(root, hidden);
    if (hidden) return;
    lv_obj_set_height(list, content_bottom - 172);
    lv_obj_set_y(previous, content_bottom - 44);
    lv_obj_set_y(refresh, content_bottom - 44);
    lv_obj_set_y(next, content_bottom - 44);
    int row_height = openclaw_density_row_height(true);
    const lv_font_t *reading_font = openclaw_reading_font();
    int line_height = reading_font->line_height;
    lv_obj_set_style_text_font(message, reading_font, 0);
    for (unsigned i = 0; i < ESP_OPENCLAW_DIRECTORY_PAGE_SIZE; ++i) {
        lv_obj_set_y(rows[i], (int)i * (row_height + 8));
        lv_obj_set_height(rows[i], row_height);
        lv_obj_set_y(avatars[i], (row_height - 36) / 2);
        lv_obj_set_y(avatar_images[i], (row_height - 36) / 2);
        lv_obj_set_style_text_font(names[i], reading_font, 0);
        lv_obj_set_style_text_font(ids[i], reading_font, 0);
        int name_y = (row_height - 2 * line_height - 4) / 2;
        lv_obj_set_y(names[i], name_y);
        lv_obj_set_y(ids[i], name_y + line_height + 4);
    }
    can_reset = target_id[0] != '\0' && !pending;
    lv_obj_set_state(use_default, LV_STATE_DISABLED, !can_reset);
    lv_label_set_text_static(default_text, pending ? "Choosing..." : can_reset ? "Use default" : "Default");
    bool changed = agents == NULL || revision != snapshot->revision || state != snapshot->state;
    agents = snapshot;
    revision = snapshot->revision;
    state = snapshot->state;
    if (changed) reset_scroll = true;
    const esp_openclaw_agent_t *selected = NULL;
    for (size_t i = 0; detail && i < agents->page.count; ++i) {
        if (strcmp(selected_id, agents->page.agents[i].id) == 0) selected = &agents->page.agents[i];
    }
    /* A reconnect or replacement roster must never retarget an open detail. */
    if (detail && (selected == NULL || state != ROOM_AGENTS_READY)) detail = false;
    lv_label_set_text_static(back_text, detail ? LV_SYMBOL_LEFT " Agents" : LV_SYMBOL_LEFT " Menu");
    lv_label_set_text_static(heading, detail ? "Agent" : "Agents");
    bool ready = state == ROOM_AGENTS_READY && agents->page.count > 0;
    lv_obj_set_hidden(list, detail || !ready);
    lv_obj_set_hidden(message, detail || ready);
    lv_obj_set_hidden(counter, detail || !ready);
    lv_obj_set_hidden(previous, detail);
    lv_obj_set_hidden(next, detail);
    lv_obj_set_hidden(refresh, detail);
    lv_obj_set_hidden(detail_avatar, !detail);
    lv_obj_set_hidden(detail_image, !detail);
    lv_obj_set_hidden(detail_name, !detail);
    lv_obj_set_hidden(detail_id, !detail);
    lv_obj_set_hidden(detail_note, !detail);
    lv_obj_set_hidden(choose, !detail);
    lv_obj_set_hidden(conversations, !detail);
    if (detail) {
        render_avatar(detail_avatar, detail_image, selected);
        lv_label_set_text(detail_name, selected->name);
        lv_label_set_text_fmt(detail_id, "Agent ID\n%s", selected->id);
        bool chosen = !expired && strcmp(target_id, selected->id) == 0;
        bool available = agents->page.main_key[0] != '\0';
        lv_obj_set_state(choose, LV_STATE_DISABLED, pending || chosen || !available);
        lv_label_set_text_static(choose_text, pending ? "Choosing..." : chosen ? "Selected for next Talk" : "Use for next Talk");
        lv_label_set_text_static(detail_note, failed ? "Selection changed.\nRefresh agents and choose again."
            : !available ? "Routing unavailable.\nUpdate your Gateway to choose an agent."
            : "Applies to your next call.\nThe current call stays with its agent.");
        return;
    }
    bool loading = state == ROOM_AGENTS_LOADING;
    lv_obj_set_state(previous, LV_STATE_DISABLED, loading || agents->page.offset == 0);
    lv_obj_set_state(next, LV_STATE_DISABLED, !ready || agents->page.offset + agents->page.count >= agents->page.total);
    lv_obj_set_state(refresh, LV_STATE_DISABLED, loading);
    if (ready) {
        lv_label_set_text_fmt(counter, "%u-%u / %u", (unsigned)agents->page.offset + 1,
            (unsigned)(agents->page.offset + agents->page.count), (unsigned)agents->page.total);
        for (size_t i = 0; i < ESP_OPENCLAW_DIRECTORY_PAGE_SIZE; ++i) {
            lv_obj_set_hidden(rows[i], i >= agents->page.count);
            if (i < agents->page.count) {
                render_avatar(avatars[i], avatar_images[i], &agents->page.agents[i]);
                lv_label_set_text(names[i], agents->page.agents[i].name);
                lv_label_set_text(ids[i], agents->page.agents[i].id);
            }
        }
        if (reset_scroll) lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    } else {
        const char *text = state == ROOM_AGENTS_OFFLINE ? "Gateway offline\n\nReconnect to browse agents."
            : loading ? "Loading agents..."
            : state == ROOM_AGENTS_IDLE ? "Directory paused\n\nRefresh to load agents."
            : state == ROOM_AGENTS_EMPTY ? (agents->page.total ? "No agents on this page.\n\nGo back or refresh the list." : "No agents yet\n\nAdd an agent in OpenClaw.")
            : state == ROOM_AGENTS_TIMEOUT ? "Request timed out\n\nRefresh to try again."
            : state == ROOM_AGENTS_TOO_LARGE ? "Directory is too large\n\nOpen agents in the mobile app."
            : "Agents unavailable\n\nCheck Gateway access, then refresh.";
        lv_label_set_text(message, text);
    }
    reset_scroll = false;
    if (!lv_obj_is_hidden(list)) {
        lv_obj_update_layout(list);
        lv_obj_readjust_scroll(list, LV_ANIM_OFF);
    }
}

room_agent_action_t room_watch_agents_activate(int action, size_t *offset)
{
    if (action == ROOM_AGENT_DEFAULT) return can_reset ? ROOM_AGENT_RESET_TARGET : ROOM_AGENT_STAY;
    if (action == ROOM_AGENT_BACK) {
        if (!detail) return ROOM_AGENT_LEAVE;
        detail = false;
        return ROOM_AGENT_STAY;
    }
    if (action == ROOM_AGENT_CONVERSATIONS && detail && agents != NULL && state == ROOM_AGENTS_READY)
        return ROOM_AGENT_OPEN_CONVERSATIONS;
    if (action == ROOM_AGENT_CHOOSE && detail && agents != NULL &&
        state == ROOM_AGENTS_READY && agents->page.main_key[0] != '\0') return ROOM_AGENT_SELECT_TARGET;
    if (agents == NULL || detail) return ROOM_AGENT_STAY;
    if (action >= 0 && (size_t)action < agents->page.count && state == ROOM_AGENTS_READY) {
        snprintf(selected_id, sizeof(selected_id), "%s", agents->page.agents[action].id);
        detail = true;
    } else if (action == ROOM_AGENT_REFRESH && state != ROOM_AGENTS_LOADING) {
        *offset = agents->page.offset;
        return ROOM_AGENT_LOAD;
    } else if (action == ROOM_AGENT_PREVIOUS && agents->page.offset > 0 && state != ROOM_AGENTS_LOADING) {
        *offset = agents->page.offset > ESP_OPENCLAW_DIRECTORY_PAGE_SIZE
            ? agents->page.offset - ESP_OPENCLAW_DIRECTORY_PAGE_SIZE : 0;
        return ROOM_AGENT_LOAD;
    } else if (action == ROOM_AGENT_NEXT && state == ROOM_AGENTS_READY &&
               agents->page.offset + agents->page.count < agents->page.total) {
        *offset = agents->page.offset + agents->page.count;
        return ROOM_AGENT_LOAD;
    }
    return ROOM_AGENT_STAY;
}

bool room_watch_agents_target(char id[65], unsigned *source_revision)
{
    if (!detail || agents == NULL || state != ROOM_AGENTS_READY) return false;
    memcpy(id, selected_id, sizeof(selected_id));
    *source_revision = revision;
    return true;
}

void room_watch_agents_focus(const char *id)
{
    snprintf(selected_id, sizeof(selected_id), "%s", id);
    detail = true;
}
