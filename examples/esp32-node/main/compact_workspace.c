// SPDX-License-Identifier: Apache-2.0
#include "compact_workspace.h"

#include <stdio.h>
#include <string.h>

#include "esp_openclaw_avatar.h"
#include "esp_openclaw_reader.h"
#include "lvgl.h"
#include "openclaw_preferences.h"

typedef enum {
    HOME, MENU, AGENTS, SESSIONS, READ, READ_ACTIONS, REPLY_PICK, REPLY_REVIEW,
    CREATE, DEVICES, DEVICE, SETTINGS, DISPLAY, BRIGHTNESS, CONNECTION,
    MESSAGING, HELP, ACTIVITY, LOCAL_DEVICE, DEVICE_ACTIONS, IDENTIFY, IDENTIFY_CONFIRM, ACTIVITY_ACTIONS,
    DRAFT_MENU, DRAFT_CHARS, DRAFT_PREVIEW, DRAFT_CHOICE, REPLY_CONFIRM,
} view_t;

typedef enum {
    OPEN_VIEW, GO_BACK, OPEN_AGENT, OPEN_SESSION, REFRESH_AGENTS, PAGE_AGENTS,
    REFRESH_SESSIONS, PAGE_SESSIONS, OPEN_REPLY, NEW_THREAD, OPEN_DEVICE,
    REFRESH_DEVICES, TOGGLE_READING, TOGGLE_DENSITY, BEGIN_IDENTIFY, SEND_IDENTIFY, CONTROL_ACCESS, IDENTIFY_RESULT,
    BEGIN_DRAFT, DISCARD_DRAFT, DRAFT_GROUP, DRAFT_CHARACTER, DRAFT_DELETE, REVIEW_DRAFT, SEND_DRAFT, EDIT_DRAFT, CLOSE_DRAFT_REVIEW,
} action_t;

typedef struct { char name[97]; action_t action; int value; } row_t;

static struct {
    int width, height, focus, count, device_index, brightness, saved_brightness;
    unsigned revision, message_generation;
    uint32_t refreshed_at, brightness_changed_at;
    bool small, encoder, dirty, connected, reader_ready, large, comfortable, brightness_save_failed, message_large, has_message, identify_history;
    bool draft_present, custom_reply;
    view_t view, messaging_parent, simple_parent, display_parent, sessions_parent;
    int simple_parent_focus, display_parent_focus, sessions_parent_focus;
    compact_brightness_fn set_brightness;
    esp_openclaw_node_wifi_status_t wifi;
    esp_openclaw_reader_agents_t agents;
    esp_openclaw_reader_conversations_t conversations;
    esp_openclaw_reader_home_t home;
    esp_openclaw_devices_snapshot_t devices;
    esp_openclaw_reader_reply_snapshot_t reply;
    esp_openclaw_reader_creation_snapshot_t creation;
    esp_openclaw_reader_messaging_snapshot_t messaging;
    esp_openclaw_identify_snapshot_t identify, last_identify;
    esp_openclaw_agent_t draft_agent;
    unsigned draft_preview_revision, draft_group;
    char draft[ESP_OPENCLAW_READER_TEXT_BYTES], draft_title[97], draft_heading[64];
    row_t rows[24];
    char message_heading[26], message[101], note[128];
    char device_id[129];
    lv_image_dsc_t lobster;
    lv_obj_t *screen, *status, *heading, *body;
} ui;

static const char *const view_names[] = {
    "home", "menu", "agents", "conversations", "reader", "reader-actions", "reply-picker", "reply-review",
    "create", "devices", "device", "settings", "display", "brightness", "connection",
    "messaging", "help", "activity", "local-device", "device-actions", "identify", "identify-confirm", "activity-actions",
    "draft", "draft-characters", "draft-preview", "draft-choice", "reply-confirm",
};

static const char *const draft_groups[] = {"a-m", "n-z", "A-M", "N-Z", "0-9", "Symbols"};
static const char *const draft_characters[] = {
    "abcdefghijklm", "nopqrstuvwxyz", "ABCDEFGHIJKLM", "NOPQRSTUVWXYZ", "0123456789", ".,!?'-:;()/@_+=&%\"",
};

typedef enum { CREATE_SUBMIT, CREATE_OPEN, CREATE_ACCESS, CREATE_BACK } create_action_t;

static create_action_t create_action(void)
{
    if (ui.creation.can_create) return CREATE_SUBMIT;
    if (ui.creation.can_open) return CREATE_OPEN;
    return ui.messaging.state != ESP_OPENCLAW_READER_MESSAGING_READY ? CREATE_ACCESS : CREATE_BACK;
}

static const lv_font_t *font(void)
{
    return ui.small ? (ui.large ? &lv_font_montserrat_14 : &lv_font_montserrat_12) :
        (ui.large ? &lv_font_montserrat_20 : &lv_font_montserrat_16);
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int width)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_label_set_text(object, text);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, width);
    lv_obj_set_style_text_font(object, font(), 0);
    lv_obj_set_style_text_color(object, lv_color_hex(0xf4efee), 0);
    return object;
}

static void open_view(view_t view)
{
    if (view == DEVICE || view == DEVICE_ACTIONS) {
        size_t index = 0;
        while (index < ui.devices.count && strcmp(ui.devices.devices[index].id, ui.device_id) != 0) ++index;
        if (ui.devices.state != ESP_OPENCLAW_DEVICES_READY || index == ui.devices.count) view = DEVICES;
        else ui.device_index = (int)index;
    }
    if (view == SESSIONS && (ui.view == HOME || ui.view == MENU || ui.view == AGENTS)) {
        ui.sessions_parent = ui.view;
        ui.sessions_parent_focus = ui.focus;
    }
    if (view == CONNECTION || view == HELP) {
        ui.simple_parent = ui.view;
        ui.simple_parent_focus = ui.focus;
    }
    ui.view = view;
    ui.focus = 0;
    ui.dirty = true;
    ++ui.revision;
}

static void failure(esp_err_t error)
{
    if (error != ESP_OK) {
        snprintf(ui.note, sizeof(ui.note), "Could not open: %s", esp_err_to_name(error));
        ui.dirty = true;
    } else ui.note[0] = '\0';
}

static void row(const char *name, action_t action, int value)
{
    if (ui.count == (int)(sizeof(ui.rows) / sizeof(ui.rows[0]))) return;
    row_t *item = &ui.rows[ui.count++];
    snprintf(item->name, sizeof(item->name), "%s", name);
    item->action = action;
    item->value = value;
}

static const char *read_state(esp_openclaw_reader_state_t state)
{
    switch (state) {
    case ESP_OPENCLAW_READER_LOADING: return "Loading...";
    case ESP_OPENCLAW_READER_OFFLINE: return "Gateway offline";
    case ESP_OPENCLAW_READER_CONNECTING: return "Connecting...";
    case ESP_OPENCLAW_READER_ACCESS_NEEDED: return "Reader access needed. Open Settings > Connection.";
    case ESP_OPENCLAW_READER_TOO_LARGE: return "Response too large";
    case ESP_OPENCLAW_READER_EMPTY: return "Nothing here yet";
    case ESP_OPENCLAW_READER_ERROR: return "Could not load. Refresh to retry.";
    default: return "Choose Refresh to load";
    }
}

static bool list_view(void)
{
    return ui.view == MENU || ui.view == AGENTS || ui.view == SESSIONS ||
        ui.view == READ_ACTIONS || ui.view == REPLY_PICK || ui.view == DEVICES ||
        ui.view == SETTINGS || ui.view == DISPLAY || ui.view == LOCAL_DEVICE ||
        ui.view == DEVICE_ACTIONS || ui.view == IDENTIFY_CONFIRM || ui.view == ACTIVITY_ACTIONS ||
        ui.view == DRAFT_MENU || ui.view == DRAFT_CHARS || ui.view == DRAFT_CHOICE || ui.view == REPLY_CONFIRM;
}

static bool draft_current(void)
{
    return ui.draft_present && ui.reader_ready && ui.conversations.state == ESP_OPENCLAW_READER_READY &&
        ui.conversations.preview_state == ESP_OPENCLAW_READER_READY &&
        ui.draft_preview_revision == ui.conversations.preview_revision &&
        strcmp(ui.draft_agent.id, ui.conversations.agent.id) == 0;
}

static bool start_draft(void)
{
    if (!ui.reader_ready || ui.conversations.state != ESP_OPENCLAW_READER_READY ||
        ui.conversations.preview_state != ESP_OPENCLAW_READER_READY ||
        ui.conversations.selected >= ui.conversations.page.count) {
        failure(ESP_ERR_INVALID_STATE);
        return false;
    }
    ui.draft[0] = '\0';
    ui.draft_present = true;
    ui.draft_preview_revision = ui.conversations.preview_revision;
    ui.draft_agent = ui.conversations.agent;
    snprintf(ui.draft_title, sizeof(ui.draft_title), "%s", ui.conversations.page.sessions[ui.conversations.selected].title);
    open_view(DRAFT_MENU);
    return true;
}

static void append_draft(char character)
{
    size_t length = strlen(ui.draft);
    if (length + 1 == sizeof(ui.draft)) snprintf(ui.note, sizeof(ui.note), "Message full (159 bytes)");
    else { ui.draft[length] = character; ui.draft[length + 1] = '\0'; }
    ++ui.revision;
    ui.dirty = true;
}

static const char *identify_state(const esp_openclaw_identify_snapshot_t *identify)
{
    static const char *states[] = {"No action", "Review before sending", "Sending...",
        "Accepted; render not confirmed", "Rendered on target", "Outcome unknown. Check the target before retrying.",
        "Not sent. Reopen device actions."};
    return states[identify->state];
}

static bool has_identify_result(void)
{
    return ui.last_identify.state >= ESP_OPENCLAW_IDENTIFY_SENDING;
}

static void remember_identify(void)
{
    bool same = strcmp(ui.identify.target.id, ui.last_identify.target.id) == 0 &&
        strcmp(ui.identify.text, ui.last_identify.text) == 0;
    if (ui.identify.state == ESP_OPENCLAW_IDENTIFY_SENDING ||
        (same && has_identify_result() && ui.identify.state >= ESP_OPENCLAW_IDENTIFY_ACCEPTED))
        ui.last_identify = ui.identify;
}

static bool identifiable(void)
{
    if (ui.devices.state != ESP_OPENCLAW_DEVICES_READY || ui.device_index < 0 ||
        (size_t)ui.device_index >= ui.devices.count) return false;
    const esp_openclaw_device_t *device = &ui.devices.devices[ui.device_index];
    return strcmp(device->id, ui.device_id) == 0 && device->connected_known && device->connected &&
        device->paired_known && device->paired && device->commands_known && device->display_show &&
        (strcmp(device->model, "m5stack-m5stickc") == 0 || strcmp(device->model, "lilygo-t-embed") == 0);
}

static void rows(void)
{
    ui.count = 0;
    switch (ui.view) {
    case MENU:
        row("Agents", OPEN_VIEW, AGENTS); row("Conversations", OPEN_VIEW, SESSIONS);
        row("Devices", OPEN_VIEW, DEVICES); row("Activity", OPEN_VIEW, ACTIVITY);
        row("Settings", OPEN_VIEW, SETTINGS); row("Home", OPEN_VIEW, HOME);
        break;
    case AGENTS:
        if (ui.agents.state == ESP_OPENCLAW_READER_READY) {
            for (size_t i = 0; i < ui.agents.page.count; ++i) {
                const esp_openclaw_agent_t *a = &ui.agents.page.agents[i];
                row(a->name[0] ? a->name : a->id, OPEN_AGENT, (int)i);
            }
        }
        if (ui.agents.page.offset + ui.agents.page.count < ui.agents.page.total)
            row("Next agents", PAGE_AGENTS, (int)(ui.agents.page.offset + ui.agents.page.count));
        if (ui.agents.page.offset) row("Previous agents", PAGE_AGENTS, (int)(ui.agents.page.offset > 8 ? ui.agents.page.offset - 8 : 0));
        row("Refresh", REFRESH_AGENTS, 0); row("Back", OPEN_VIEW, MENU);
        break;
    case SESSIONS:
        if (ui.conversations.state == ESP_OPENCLAW_READER_READY) {
            for (size_t i = 0; i < ui.conversations.page.count; ++i) {
                const esp_openclaw_session_t *s = &ui.conversations.page.sessions[i];
                row(s->title[0] ? s->title : "Untitled conversation", OPEN_SESSION, (int)i);
            }
        }
        if (ui.conversations.agent.id[0] && (ui.conversations.state == ESP_OPENCLAW_READER_READY ||
            ui.conversations.state == ESP_OPENCLAW_READER_EMPTY)) row("New conversation", NEW_THREAD, 0);
        if (ui.conversations.page.has_more) row("Next conversations", PAGE_SESSIONS, (int)ui.conversations.page.next_offset);
        if (ui.conversations.page.offset) row("Previous conversations", PAGE_SESSIONS, (int)(ui.conversations.page.offset > 8 ? ui.conversations.page.offset - 8 : 0));
        row("Refresh", REFRESH_SESSIONS, 0); row("Change agent", OPEN_VIEW, AGENTS);
        row("Back", GO_BACK, ui.sessions_parent);
        break;
    case READ_ACTIONS:
        if (ui.conversations.preview_state == ESP_OPENCLAW_READER_READY &&
            (ui.conversations.preview.status == ESP_OPENCLAW_PREVIEW_OK ||
             ui.conversations.preview.status == ESP_OPENCLAW_PREVIEW_EMPTY)) row("Reply", OPEN_VIEW, REPLY_PICK);
        row("Refresh messages", OPEN_SESSION, (int)ui.conversations.selected);
        row("Back to messages", OPEN_VIEW, READ); row("Conversations", OPEN_VIEW, SESSIONS);
        break;
    case REPLY_PICK:
        row("Continue", OPEN_REPLY, ESP_OPENCLAW_READER_REPLY_CONTINUE);
        row("Summarize", OPEN_REPLY, ESP_OPENCLAW_READER_REPLY_SUMMARIZE);
        row("Explain", OPEN_REPLY, ESP_OPENCLAW_READER_REPLY_EXPLAIN);
        row("Write message", BEGIN_DRAFT, 0);
        row("Back", OPEN_VIEW, READ);
        break;
    case DRAFT_MENU:
        row("Preview", OPEN_VIEW, DRAFT_PREVIEW);
        for (unsigned i = 0; i < sizeof(draft_groups) / sizeof(draft_groups[0]); ++i)
            row(draft_groups[i], DRAFT_GROUP, (int)i);
        row("Space", DRAFT_CHARACTER, ' '); row("Delete", DRAFT_DELETE, 0);
        if (draft_current()) row("Review", REVIEW_DRAFT, 0);
        row("Back to messages", OPEN_VIEW, READ);
        break;
    case DRAFT_CHARS:
        for (const char *p = draft_characters[ui.draft_group]; *p; ++p) {
            char text[] = {*p, '\0'};
            row(text, DRAFT_CHARACTER, *p);
        }
        row("Back", OPEN_VIEW, DRAFT_MENU);
        break;
    case DRAFT_CHOICE:
        if (draft_current()) row("Resume draft", OPEN_VIEW, DRAFT_MENU);
        row("Preview saved draft", OPEN_VIEW, DRAFT_PREVIEW);
        row("Discard draft", DISCARD_DRAFT, 0);
        row("Back", OPEN_VIEW, REPLY_PICK);
        break;
    case REPLY_CONFIRM:
        if (ui.reply.can_send) row("Send", SEND_DRAFT, 0);
        if (ui.reply.state == ESP_OPENCLAW_READER_REPLY_REVIEW) row("Edit", EDIT_DRAFT, 0);
        row("Cancel", CLOSE_DRAFT_REVIEW, 0);
        break;
    case DEVICES:
        row("This device", OPEN_VIEW, LOCAL_DEVICE);
        if (ui.devices.state == ESP_OPENCLAW_DEVICES_READY) {
            for (size_t i = 0; i < ui.devices.count; ++i) {
                row(ui.devices.devices[i].name[0] ? ui.devices.devices[i].name : ui.devices.devices[i].id, OPEN_DEVICE, (int)i);
            }
        }
        row("Refresh", REFRESH_DEVICES, 0); row("Back", OPEN_VIEW, MENU);
        break;
    case DEVICE_ACTIONS:
        if (identifiable()) {
            if (ui.messaging.state == ESP_OPENCLAW_READER_MESSAGING_READY) row("Identify", BEGIN_IDENTIFY, 0);
            else row("Messaging access", CONTROL_ACCESS, 0);
        }
        row("Details", OPEN_VIEW, DEVICE); row("Back to devices", OPEN_VIEW, DEVICES);
        break;
    case IDENTIFY_CONFIRM:
        if (ui.identify.can_send) row("Show code", SEND_IDENTIFY, 0);
        row("Back", OPEN_VIEW, IDENTIFY);
        row("Cancel", OPEN_VIEW, DEVICE_ACTIONS);
        break;
    case ACTIVITY_ACTIONS:
        if (has_identify_result()) row("Device result", IDENTIFY_RESULT, 0);
        row("Back to activity", OPEN_VIEW, ACTIVITY); row("Menu", OPEN_VIEW, MENU);
        break;
    case SETTINGS:
        row("Display", OPEN_VIEW, DISPLAY); row("Connection", OPEN_VIEW, CONNECTION);
        row("Messaging access", OPEN_VIEW, MESSAGING); row("Help", OPEN_VIEW, HELP);
        row("Back", OPEN_VIEW, MENU);
        break;
    case LOCAL_DEVICE:
        row("Display", OPEN_VIEW, DISPLAY); row("Connection", OPEN_VIEW, CONNECTION);
        row("Help", OPEN_VIEW, HELP); row("Back", OPEN_VIEW, DEVICES);
        break;
    case DISPLAY:
        row("Brightness", OPEN_VIEW, BRIGHTNESS);
        row(ui.large ? "Text: larger" : "Text: standard", TOGGLE_READING, 0);
        row(ui.comfortable ? "Spacing: comfortable" : "Spacing: compact", TOGGLE_DENSITY, 0);
        row("Connection", OPEN_VIEW, CONNECTION); row("Back", OPEN_VIEW, ui.display_parent);
        break;
    default: break;
    }
    if (ui.focus >= ui.count) ui.focus = ui.count ? ui.count - 1 : 0;
}

static const char *title(void)
{
    switch (ui.view) {
    case HOME: return ui.home.available ? (ui.home.agent.name[0] ? ui.home.agent.name : ui.home.agent.id) : "OpenClaw";
    case MENU: return "Menu";
    case AGENTS: return "Agents";
    case SESSIONS: return ui.conversations.agent.name[0] ? ui.conversations.agent.name : "Conversations";
    case READ: return ui.conversations.selected < ui.conversations.page.count ?
        ui.conversations.page.sessions[ui.conversations.selected].title : "Messages";
    case READ_ACTIONS: return "Message actions";
    case REPLY_PICK: return "Quick reply";
    case REPLY_REVIEW: return "Review reply";
    case CREATE: return "New conversation";
    case DEVICES: return "Devices";
    case DEVICE: return "Device details";
    case SETTINGS: return "Settings";
    case DISPLAY: return "Display";
    case BRIGHTNESS: return "Brightness";
    case CONNECTION: return "Connection";
    case MESSAGING: return "Messaging access";
    case HELP: return "Help";
    case ACTIVITY: return ui.message_heading[0] ? ui.message_heading : "Activity";
    case LOCAL_DEVICE: return "This device";
    case DEVICE_ACTIONS: return "Device actions";
    case IDENTIFY: return "Identify device";
    case IDENTIFY_CONFIRM: return "Replace display?";
    case ACTIVITY_ACTIONS: return "Activity actions";
    case DRAFT_MENU: case DRAFT_CHARS:
        if (!draft_current()) return "Draft: selection changed";
        {
            size_t length = strlen(ui.draft);
            snprintf(ui.draft_heading, sizeof(ui.draft_heading), "%zu/159 %.12s", length, ui.draft + (length > 12 ? length - 12 : 0));
            return ui.draft_heading;
        }
    case DRAFT_CHOICE: return "Saved draft";
    case DRAFT_PREVIEW: return "Draft preview";
    case REPLY_CONFIRM: return "Send this message?";
    }
    return "OpenClaw";
}

static void reading(const char *text)
{
    lv_obj_t *object = label(ui.body, text, 0, 0, ui.width - (ui.small ? 8 : 20));
    lv_label_set_long_mode(object, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(object, LV_SIZE_CONTENT);
}

static void render(void)
{
    lv_obj_clean(ui.body);
    lv_obj_scroll_to_y(ui.body, 0, LV_ANIM_OFF);
    lv_label_set_text(ui.heading, title());
    lv_obj_set_style_text_font(ui.heading, ui.small ? &lv_font_montserrat_14 : &lv_font_montserrat_20, 0);
    char status[64];
    snprintf(status, sizeof(status), "%s %s   OC %s   --",
        LV_SYMBOL_WIFI, ui.wifi.connected ? "on" : "off", ui.connected ? "on" : "off");
    lv_label_set_text(ui.status, status);
    if (list_view()) {
        rows();
        char position[120];
        snprintf(position, sizeof(position), "%s %d/%d", title(), ui.focus + 1, ui.count);
        lv_label_set_text(ui.heading, position);
        int height = ui.small ? (ui.large || ui.comfortable ? 25 : 20) : (ui.large || ui.comfortable ? 40 : 31);
        int visible = lv_obj_get_height(ui.body) / height;
        if (visible < 1) visible = 1;
        int first = ui.focus >= visible ? ui.focus - visible + 1 : 0;
        for (int i = first; i < ui.count && i < first + visible; ++i) {
            lv_obj_t *item = label(ui.body, ui.rows[i].name, 3, (i - first) * height + 2, ui.width - (ui.small ? 15 : 28));
            lv_label_set_long_mode(item, LV_LABEL_LONG_DOT);
            lv_obj_set_height(item, height - 2);
            if (i == ui.focus) {
                lv_obj_set_style_bg_color(item, lv_color_hex(0x663d39), 0);
                lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(item, 4, 0);
            }
        }
        if ((ui.view == AGENTS && ui.agents.state != ESP_OPENCLAW_READER_READY) ||
            (ui.view == SESSIONS && ui.conversations.state != ESP_OPENCLAW_READER_READY)) {
            const char *state = read_state(ui.view == AGENTS ? ui.agents.state : ui.conversations.state);
            lv_label_set_text(ui.heading, state);
        }
    } else if (ui.view == HOME) {
        lv_obj_t *presence = lv_image_create(ui.body);
        lv_image_set_src(presence, &ui.lobster);
        lv_obj_set_pos(presence, 3, ui.small ? 3 : 10);
        lv_obj_set_size(presence, ui.small ? 27 : 68, ui.small ? 35 : 80);
        lv_image_set_inner_align(presence, LV_IMAGE_ALIGN_CONTAIN);
        const char *headline = !ui.connected ? "Let's reconnect" : !ui.reader_ready ? "Set up access" :
            ui.home.available || ui.small ? "Ready for you" : "Ready when you are";
        lv_obj_t *headline_label = label(ui.body, headline, ui.small ? 34 : 82, ui.small ? 0 : 16, ui.small ? 118 : 210);
        lv_label_set_long_mode(headline_label, LV_LABEL_LONG_DOT);
        lv_obj_t *action = label(ui.body, !ui.connected || !ui.reader_ready ? "Connection" : ui.home.available ? "Continue" : "Choose agent",
            ui.small ? 34 : 82, ui.small ? 21 : 58, ui.small ? 118 : 210);
        lv_obj_set_style_bg_color(action, lv_color_hex(0x663d39), 0);
        lv_obj_set_style_bg_opa(action, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(action, 5, 0);
        lv_obj_t *hint = label(ui.body, ui.encoder ? "Press: select   Turn: menu" : "A: select   B: menu",
            ui.small ? 2 : 82, ui.small ? 39 : 95, ui.small ? 148 : 210);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
    } else if (ui.view == READ) {
        if (ui.conversations.preview_state != ESP_OPENCLAW_READER_READY) reading(read_state(ui.conversations.preview_state));
        else if (ui.conversations.preview.status == ESP_OPENCLAW_PREVIEW_MISSING) reading("Conversation missing. Press for actions.");
        else if (!ui.conversations.preview.count) reading("No messages yet. Press for actions.");
        else {
            static const char *roles[] = {"You", "Assistant", "Tool", "System", "Message"};
            char text[1200] = "";
            size_t used = 0;
            for (size_t i = 0; i < ui.conversations.preview.count; ++i) {
                const esp_openclaw_preview_item_t *item = &ui.conversations.preview.items[i];
                int count = snprintf(text + used, sizeof(text) - used, "%s\n%s\n\n", roles[item->role], item->text);
                if (count < 0 || (size_t)count >= sizeof(text) - used) break;
                used += (size_t)count;
            }
            reading(text);
        }
    } else if (ui.view == REPLY_REVIEW) {
        char text[640];
        bool pending = ui.custom_reply && (ui.reply.state == ESP_OPENCLAW_READER_REPLY_PREPARING ||
            ui.reply.state == ESP_OPENCLAW_READER_REPLY_SENDING || ui.reply.state == ESP_OPENCLAW_READER_REPLY_ACCEPTED);
        snprintf(text, sizeof(text), "%s%s%s%s%s\n\n%s\n\n%s",
            ui.custom_reply ? ui.reply.agent.name : "", ui.custom_reply ? "\n" : "", ui.custom_reply ? ui.reply.title : "",
            ui.custom_reply ? "\n\n" : "",
            ui.reply.text, ui.reply.detail,
            ui.reply.can_send ? ui.custom_reply ? "Press for Send / Edit / Cancel" : "Press to send" : pending ? "Hold to go Home" : ui.reply.can_read ? "Press to read reply" :
            ui.messaging.state != ESP_OPENCLAW_READER_MESSAGING_READY ? "Press for messaging access" : "Hold to go Home");
        reading(text);
    } else if (ui.view == DRAFT_PREVIEW) {
        char text[450];
        snprintf(text, sizeof(text), "%s\n%s\n\n%s\n\n%s", ui.draft_agent.name, ui.draft_title,
            ui.draft[0] ? ui.draft : "(empty)", draft_current() ? "Press to edit." :
                "Selection changed. The saved draft will not be sent. Press to return.");
        reading(text);
    } else if (ui.view == CREATE) {
        char text[300];
        snprintf(text, sizeof(text), "%s\n\n%s", ui.creation.detail,
            ui.creation.can_create ? "Press to create" : ui.creation.can_open ? "Press to open" :
            ui.messaging.state != ESP_OPENCLAW_READER_MESSAGING_READY ? "Press for messaging access" : "Hold to go Home");
        reading(text);
    } else if (ui.view == CONNECTION) {
        char text[350];
        snprintf(text, sizeof(text), "Wi-Fi: %s\nNetwork: %s\nIP: %s\nOpenClaw: %s\nReader: %s\n\nUSB console: reader setup-code\nApprove reader access on the Gateway.\n\nPress to return.",
            ui.wifi.connected ? "connected" : "offline", ui.wifi.ssid,
            ui.wifi.connected ? ui.wifi.ip : "unavailable", ui.connected ? "connected" : "offline",
            ui.reader_ready ? "ready" : "access needed or connecting");
        reading(text);
    } else if (ui.view == BRIGHTNESS) {
        char text[100];
        snprintf(text, sizeof(text), "%d%%\n\n%s\nPress to return", ui.brightness,
            ui.encoder ? "Turn to adjust" : "B: brighter, hold B: dimmer");
        reading(text);
    } else if (ui.view == MESSAGING) {
        static const char *states[] = {"Read only", "Requesting access", "Waiting for approval", "Checking access", "Messaging ready", "Request rejected", "Request expired", "Request failed"};
        char text[250];
        snprintf(text, sizeof(text), "%s\n\n%s", states[ui.messaging.state],
            ui.messaging.state == ESP_OPENCLAW_READER_MESSAGING_READY ? "Press to return" :
            ui.messaging.state == ESP_OPENCLAW_READER_MESSAGING_WAITING || ui.messaging.state == ESP_OPENCLAW_READER_MESSAGING_REQUESTING ||
            ui.messaging.state == ESP_OPENCLAW_READER_MESSAGING_CONNECTING ? "Approve on your Gateway. Press to cancel." : "Press to request messaging access.");
        reading(text);
    } else if (ui.view == DEVICE) {
        const esp_openclaw_device_t *device = &ui.devices.devices[ui.device_index];
        char text[400];
        snprintf(text, sizeof(text), "%s\n%s\n\nConnection: %s\nPaired: %s\n\n%s\n\nPress for actions", device->name, device->model,
            device->connected_known ? device->connected ? "online" : "offline" : "unknown",
            device->paired_known ? device->paired ? "yes" : "no" : "unknown", device->id);
        reading(text);
    } else if (ui.view == IDENTIFY) {
        const esp_openclaw_identify_snapshot_t *identify = ui.identify_history ? &ui.last_identify : &ui.identify;
        char text[420];
        snprintf(text, sizeof(text), "%s\n%s\n\n%s\n\n%s\n\n%s", identify->target.name,
            identify->target.id, identify->text, identify_state(identify),
            identify->state == ESP_OPENCLAW_IDENTIFY_REVIEW ?
                "Replaces the target's displayed message. Press for Show / Cancel." :
                identify->state == ESP_OPENCLAW_IDENTIFY_SENDING ? "Hold for Home." : "Press for device actions.");
        reading(text);
    } else if (ui.view == ACTIVITY) {
        char text[512];
        snprintf(text, sizeof(text), "%s\n\n%s\n\n%s%s%s\n\nPress for actions.",
            ui.has_message ? ui.message : "No incoming messages", ui.reply.detail,
            has_identify_result() ? ui.last_identify.target.name : "", has_identify_result() ? "\n" : "",
            has_identify_result() ? identify_state(&ui.last_identify) : "");
        reading(text);
        if (ui.has_message) {
            lv_obj_t *message = lv_obj_get_child(ui.body, 0);
            lv_obj_set_style_text_font(message, ui.message_large ?
                (ui.small ? &lv_font_montserrat_20 : &lv_font_montserrat_28) :
                (ui.small ? &lv_font_montserrat_12 : &lv_font_montserrat_16), 0);
        }
    } else if (ui.view == HELP) {
        reading(ui.encoder ? "Turn to move or scroll. Press to select or go back. Hold to go Home.\n\nMenu: Agents, Conversations, Devices, Activity, Settings.\n\nReview messages before Send. Unknown means check the conversation before retrying." :
            "A: select. Hold A: Home.\nB: next or scroll. Hold B: previous.\n\nUse Back rows to return.\n\nMenu: Agents, Conversations, Devices, Activity, Settings.\n\nReview messages before Send.");
    }
    if (ui.note[0]) lv_label_set_text(ui.heading, ui.note);
    ui.dirty = false;
}

void compact_workspace_start(int width, int height, bool encoder, compact_brightness_fn brightness)
{
    ui.width = width; ui.height = height; ui.small = width < 200; ui.encoder = encoder;
    ui.set_brightness = brightness; ui.revision = 1; ui.dirty = true;
    ui.large = openclaw_preferences_load(OPENCLAW_PREFERENCE_READING, 0);
    ui.comfortable = openclaw_preferences_load(OPENCLAW_PREFERENCE_DENSITY, 0);
    ui.brightness = openclaw_preferences_load(OPENCLAW_PREFERENCE_BRIGHTNESS, 80);
    ui.saved_brightness = ui.brightness;
    if (brightness(ui.brightness) != ESP_OK) snprintf(ui.note, sizeof(ui.note), "Brightness unavailable");
    ui.screen = lv_screen_active();
    ui.lobster = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_ARGB8888, .w = 64, .h = 64, .stride = 64 * 4},
        .data_size = 64 * 64 * 4, .data = esp_openclaw_avatar_pixels(ESP_OPENCLAW_AVATAR_LOBSTER)};
    lv_obj_set_style_bg_color(ui.screen, lv_color_hex(0x11131b), 0);
    lv_obj_set_style_pad_all(ui.screen, 0, 0);
    ui.status = label(ui.screen, "", 4, 0, width - 8);
    lv_obj_set_style_text_font(ui.status, &lv_font_montserrat_10, 0);
    ui.heading = label(ui.screen, "OpenClaw", 4, ui.small ? 11 : 17, width - 8);
    lv_label_set_long_mode(ui.heading, LV_LABEL_LONG_DOT);
    ui.body = lv_obj_create(ui.screen);
    lv_obj_set_pos(ui.body, ui.small ? 4 : 10, ui.small ? 29 : 46);
    lv_obj_set_size(ui.body, width - (ui.small ? 8 : 20), height - (ui.small ? 30 : 48));
    lv_obj_set_style_bg_opa(ui.body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui.body, 0, 0);
    lv_obj_set_style_pad_all(ui.body, 0, 0);
    lv_obj_set_scroll_dir(ui.body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui.body, LV_SCROLLBAR_MODE_OFF);
    render();
}

void compact_workspace_poll(const esp_openclaw_node_wifi_status_t *wifi, bool connected, uint32_t now)
{
    if (now - ui.refreshed_at >= 250) {
        bool changed = ui.connected != connected || ui.wifi.connected != wifi->connected;
        bool old_connected = ui.connected, old_reader_ready = ui.reader_ready;
        bool old_home_available = ui.home.available, old_home_preview = ui.home.preview;
        unsigned old_agents = ui.agents.revision, old_sessions = ui.conversations.revision;
        unsigned old_preview = ui.conversations.preview_revision, old_reply = ui.reply.revision;
        unsigned old_creation = ui.creation.revision, old_messaging = ui.messaging.revision;
        unsigned old_devices = ui.devices.revision, old_identify = ui.identify.revision;
        esp_openclaw_reader_state_t old_agents_state = ui.agents.state;
        esp_openclaw_reader_state_t old_sessions_state = ui.conversations.state;
        esp_openclaw_reader_state_t old_preview_state = ui.conversations.preview_state;
        esp_openclaw_devices_state_t old_devices_state = ui.devices.state;
        bool old_identify_send = ui.identify.can_send;
        create_action_t old_create_action = create_action();
        ui.wifi = *wifi; ui.connected = connected;
        esp_openclaw_reader_agents_copy(&ui.agents);
        esp_openclaw_reader_conversations_copy(&ui.conversations);
        esp_openclaw_reader_home_copy(&ui.home);
        esp_openclaw_reader_reply_copy(&ui.reply);
        esp_openclaw_reader_creation_copy(&ui.creation);
        esp_openclaw_reader_messaging_copy(&ui.messaging);
        esp_openclaw_reader_devices_copy(&ui.devices);
        esp_openclaw_reader_identify_copy(&ui.identify);
        remember_identify();
        cJSON *status = esp_openclaw_reader_status();
        ui.reader_ready = status != NULL && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(status, "readConnected"));
        cJSON_Delete(status);
        bool agents_changed = old_agents != ui.agents.revision || old_agents_state != ui.agents.state;
        bool sessions_changed = old_sessions != ui.conversations.revision || old_sessions_state != ui.conversations.state;
        bool preview_changed = old_preview != ui.conversations.preview_revision || old_preview_state != ui.conversations.preview_state;
        bool devices_changed = old_devices != ui.devices.revision || old_devices_state != ui.devices.state;
        bool home_action_changed = ui.view == HOME && (old_connected != ui.connected ||
            old_reader_ready != ui.reader_ready || old_home_available != ui.home.available ||
            old_home_preview != ui.home.preview);
        bool relevant = home_action_changed || (ui.view == CONNECTION && old_reader_ready != ui.reader_ready) ||
            (ui.view == AGENTS && agents_changed) ||
            ((ui.view == HOME || ui.view == SESSIONS || ui.view == READ || ui.view == READ_ACTIONS || ui.view == REPLY_PICK) &&
                (sessions_changed || preview_changed)) ||
            ((ui.view == REPLY_REVIEW || ui.view == REPLY_CONFIRM || ui.view == ACTIVITY) && old_reply != ui.reply.revision) ||
            ((ui.view == DRAFT_MENU || ui.view == DRAFT_CHARS || ui.view == DRAFT_PREVIEW || ui.view == DRAFT_CHOICE) &&
                (sessions_changed || preview_changed || old_reader_ready != ui.reader_ready)) ||
            (ui.view == CREATE && (old_creation != ui.creation.revision || old_create_action != create_action())) ||
            (ui.view == MESSAGING && old_messaging != ui.messaging.revision) ||
            (ui.view == DEVICE_ACTIONS && old_messaging != ui.messaging.revision) ||
            ((ui.view == DEVICES || ui.view == DEVICE || ui.view == DEVICE_ACTIONS) && devices_changed) ||
            ((ui.view == IDENTIFY || ui.view == IDENTIFY_CONFIRM || ui.view == ACTIVITY || ui.view == ACTIVITY_ACTIONS) &&
                (old_identify != ui.identify.revision || old_identify_send != ui.identify.can_send));
        if ((ui.view == DEVICE || ui.view == DEVICE_ACTIONS) && devices_changed) {
            size_t index = 0;
            while (index < ui.devices.count && strcmp(ui.devices.devices[index].id, ui.device_id) != 0) ++index;
            if (index < ui.devices.count) ui.device_index = (int)index;
            else open_view(DEVICES);
        }
        if (relevant) ++ui.revision;
        ui.dirty |= changed || relevant;
        ui.refreshed_at = now;
    }
    if (!ui.brightness_save_failed && ui.brightness != ui.saved_brightness && now - ui.brightness_changed_at >= 750) {
        if (openclaw_preferences_save(OPENCLAW_PREFERENCE_BRIGHTNESS, ui.brightness) == ESP_OK) ui.saved_brightness = ui.brightness;
        else { snprintf(ui.note, sizeof(ui.note), "Brightness not saved"); ui.brightness_save_failed = true; ui.dirty = true; }
    }
    if (ui.dirty) render();
}

void compact_workspace_input(compact_input_event_t event)
{
    if (event.home) { open_view(HOME); return; }
    if (event.move) {
        event.select = false;
        if (ui.view == HOME) open_view(MENU);
        else if (list_view()) {
            rows();
            if (ui.count) ui.focus = ((ui.focus + event.move) % ui.count + ui.count) % ui.count;
            ++ui.revision; ui.dirty = true;
        } else if (ui.view == BRIGHTNESS) {
            int value = ui.brightness + event.move * 5;
            if (value < 10) value = 10;
            if (value > 100) value = 100;
            if (ui.set_brightness(value) == ESP_OK) { ui.brightness = value; ui.brightness_changed_at = lv_tick_get(); ui.brightness_save_failed = false; }
            else snprintf(ui.note, sizeof(ui.note), "Brightness failed");
            ui.dirty = true;
        } else lv_obj_scroll_by(ui.body, 0, -event.move * (ui.small ? 16 : 28), LV_ANIM_OFF);
    }
    if (!event.select) return;
    ui.note[0] = '\0';
    if (ui.view == HOME) {
        if (!ui.connected || !ui.reader_ready) open_view(CONNECTION);
        else if (ui.home.available) {
            ui.sessions_parent = HOME;
            ui.sessions_parent_focus = 0;
            open_view(ui.home.preview ? READ : SESSIONS);
        }
        else { failure(esp_openclaw_reader_agents_refresh(0)); open_view(AGENTS); }
    } else if (list_view()) {
        rows();
        if (!ui.count) return;
        row_t item = ui.rows[ui.focus];
        switch (item.action) {
        case GO_BACK:
        case OPEN_VIEW:
            if (item.value == DISPLAY) { ui.display_parent = ui.view; ui.display_parent_focus = ui.focus; }
            if (item.value == AGENTS && item.action != GO_BACK) failure(esp_openclaw_reader_agents_refresh(0));
            if (item.value == DEVICES) failure(esp_openclaw_reader_devices_refresh());
            if (item.value == SESSIONS && !ui.conversations.agent.id[0]) {
                failure(esp_openclaw_reader_agents_refresh(0)); item.value = AGENTS;
            }
            ui.messaging_parent = SETTINGS;
            {
                bool display_back = ui.view == DISPLAY && item.value == (int)ui.display_parent;
                bool sessions_back = item.action == GO_BACK;
                bool draft_back = ui.view == DRAFT_CHARS && item.value == DRAFT_MENU;
                open_view((view_t)item.value);
                if (display_back) ui.focus = ui.display_parent_focus;
                if (sessions_back) ui.focus = ui.sessions_parent_focus;
                if (draft_back) ui.focus = (int)ui.draft_group + 1;
            }
            break;
        case OPEN_AGENT:
            failure(esp_openclaw_reader_conversations_open(ui.agents.page.agents[item.value].id, ui.agents.revision));
            if (!ui.note[0]) open_view(SESSIONS);
            break;
        case OPEN_SESSION:
            failure(esp_openclaw_reader_preview_open((size_t)item.value, ui.conversations.revision));
            if (!ui.note[0]) open_view(READ);
            break;
        case REFRESH_AGENTS: case PAGE_AGENTS: failure(esp_openclaw_reader_agents_refresh((size_t)item.value)); break;
        case REFRESH_SESSIONS: case PAGE_SESSIONS: failure(esp_openclaw_reader_conversations_refresh((size_t)item.value)); break;
        case REFRESH_DEVICES: failure(esp_openclaw_reader_devices_refresh()); break;
        case OPEN_REPLY:
            failure(esp_openclaw_reader_reply_begin((esp_openclaw_reader_reply_preset_t)item.value, ui.conversations.preview_revision));
            if (!ui.note[0]) { ui.custom_reply = false; open_view(REPLY_REVIEW); }
            break;
        case BEGIN_DRAFT:
            if (ui.draft_present) open_view(DRAFT_CHOICE);
            else start_draft();
            break;
        case DISCARD_DRAFT:
            ui.draft_present = false; ui.draft[0] = '\0'; open_view(REPLY_PICK); break;
        case DRAFT_GROUP:
            ui.draft_group = (unsigned)item.value; open_view(DRAFT_CHARS); break;
        case DRAFT_CHARACTER: append_draft((char)item.value); break;
        case DRAFT_DELETE:
            if (ui.draft[0]) ui.draft[strlen(ui.draft) - 1] = '\0';
            ++ui.revision; ui.dirty = true; break;
        case REVIEW_DRAFT:
            if (!draft_current()) failure(ESP_ERR_INVALID_STATE);
            else if (strspn(ui.draft, " ") == strlen(ui.draft)) {
                snprintf(ui.note, sizeof(ui.note), "Write a message first"); ui.dirty = true;
            } else {
                failure(esp_openclaw_reader_reply_begin_text(ui.draft, strlen(ui.draft), ui.draft_preview_revision));
                if (!ui.note[0]) { ui.custom_reply = true; esp_openclaw_reader_reply_copy(&ui.reply); open_view(REPLY_REVIEW); }
            }
            break;
        case SEND_DRAFT:
            failure(esp_openclaw_reader_reply_send(ui.reply.revision));
            if (!ui.note[0]) {
                ui.draft_present = false; ui.draft[0] = '\0';
                esp_openclaw_reader_reply_copy(&ui.reply);
                open_view(REPLY_REVIEW);
            }
            break;
        case EDIT_DRAFT:
            if (ui.reply.state == ESP_OPENCLAW_READER_REPLY_REVIEW) {
                snprintf(ui.draft, sizeof(ui.draft), "%s", ui.reply.text);
                ui.draft_agent = ui.reply.agent;
                snprintf(ui.draft_title, sizeof(ui.draft_title), "%s", ui.reply.title);
                ui.draft_preview_revision = ui.reply.preview_revision;
                ui.draft_present = true;
                esp_openclaw_reader_reply_close(ui.reply.revision);
                open_view(DRAFT_MENU);
            }
            break;
        case CLOSE_DRAFT_REVIEW:
            esp_openclaw_reader_reply_close(ui.reply.revision); open_view(READ); break;
        case NEW_THREAD:
            failure(esp_openclaw_reader_creation_begin(ui.conversations.revision));
            if (!ui.note[0]) open_view(CREATE);
            break;
        case OPEN_DEVICE:
            ui.device_index = item.value;
            snprintf(ui.device_id, sizeof(ui.device_id), "%s", ui.devices.devices[item.value].id);
            open_view(DEVICE); break;
        case BEGIN_IDENTIFY:
            failure(esp_openclaw_reader_identify_begin(ui.device_id, ui.devices.revision));
            if (!ui.note[0]) { ui.identify_history = false; esp_openclaw_reader_identify_copy(&ui.identify); open_view(IDENTIFY); }
            break;
        case SEND_IDENTIFY:
            failure(esp_openclaw_reader_identify_send(ui.identify.revision));
            if (!ui.note[0]) {
                /* The worker can finish before copy observes SENDING. Retain
                 * the admitted review first, then apply its current result. */
                ui.last_identify = ui.identify;
                ui.last_identify.state = ESP_OPENCLAW_IDENTIFY_SENDING;
                ui.last_identify.can_send = false;
                esp_openclaw_reader_identify_copy(&ui.identify);
                remember_identify();
                open_view(IDENTIFY);
            }
            break;
        case CONTROL_ACCESS:
            ui.messaging_parent = DEVICE_ACTIONS; open_view(MESSAGING); break;
        case IDENTIFY_RESULT:
            snprintf(ui.device_id, sizeof(ui.device_id), "%s", ui.last_identify.target.id);
            ui.identify_history = true;
            open_view(IDENTIFY); break;
        case TOGGLE_READING:
            if (openclaw_preferences_save(OPENCLAW_PREFERENCE_READING, !ui.large) == ESP_OK) ui.large = !ui.large;
            else snprintf(ui.note, sizeof(ui.note), "Text size not saved");
            ++ui.revision; ui.dirty = true; break;
        case TOGGLE_DENSITY:
            if (openclaw_preferences_save(OPENCLAW_PREFERENCE_DENSITY, !ui.comfortable) == ESP_OK) ui.comfortable = !ui.comfortable;
            else snprintf(ui.note, sizeof(ui.note), "Spacing not saved");
            ++ui.revision; ui.dirty = true; break;
        }
    } else if (ui.view == READ) open_view(READ_ACTIONS);
    else if (ui.view == REPLY_REVIEW) {
        if (ui.custom_reply && ui.reply.state == ESP_OPENCLAW_READER_REPLY_REVIEW) open_view(REPLY_CONFIRM);
        else if (ui.custom_reply && (ui.reply.state == ESP_OPENCLAW_READER_REPLY_PREPARING ||
            ui.reply.state == ESP_OPENCLAW_READER_REPLY_SENDING || ui.reply.state == ESP_OPENCLAW_READER_REPLY_ACCEPTED)) return;
        else if (ui.reply.can_send) failure(esp_openclaw_reader_reply_send(ui.reply.revision));
        else if (ui.reply.can_read) { failure(esp_openclaw_reader_reply_read(ui.reply.revision)); if (!ui.note[0]) open_view(READ); }
        else if (ui.messaging.state != ESP_OPENCLAW_READER_MESSAGING_READY) { ui.messaging_parent = REPLY_REVIEW; open_view(MESSAGING); }
        else open_view(READ);
    } else if (ui.view == CREATE) {
        if (create_action() == CREATE_SUBMIT) failure(esp_openclaw_reader_creation_submit(ui.creation.revision));
        else if (create_action() == CREATE_OPEN) { failure(esp_openclaw_reader_creation_open(ui.creation.revision)); if (!ui.note[0]) open_view(READ); }
        else if (create_action() == CREATE_ACCESS) { ui.messaging_parent = CREATE; open_view(MESSAGING); }
        else open_view(SESSIONS);
    } else if (ui.view == MESSAGING) {
        if (ui.messaging.state == ESP_OPENCLAW_READER_MESSAGING_READY) open_view(ui.messaging_parent);
        else if (ui.messaging.state == ESP_OPENCLAW_READER_MESSAGING_WAITING || ui.messaging.state == ESP_OPENCLAW_READER_MESSAGING_REQUESTING ||
                 ui.messaging.state == ESP_OPENCLAW_READER_MESSAGING_CONNECTING) failure(esp_openclaw_reader_messaging_cancel(ui.messaging.revision));
        else failure(esp_openclaw_reader_messaging_request(ui.messaging.revision));
    } else if (ui.view == BRIGHTNESS) open_view(DISPLAY);
    else if (ui.view == DRAFT_PREVIEW) open_view(draft_current() ? DRAFT_MENU : DRAFT_CHOICE);
    else if (ui.view == DEVICE) open_view(DEVICE_ACTIONS);
    else if (ui.view == IDENTIFY) {
        if (ui.identify_history) open_view(DEVICE_ACTIONS);
        else if (ui.identify.state == ESP_OPENCLAW_IDENTIFY_REVIEW && ui.identify.can_send) open_view(IDENTIFY_CONFIRM);
        else if (ui.identify.state != ESP_OPENCLAW_IDENTIFY_SENDING) open_view(DEVICE_ACTIONS);
    }
    else if (ui.view == ACTIVITY) open_view(ACTIVITY_ACTIONS);
    else if (ui.view == CONNECTION || ui.view == HELP) {
        view_t parent = ui.simple_parent;
        int focus = ui.simple_parent_focus;
        open_view(parent); ui.focus = focus;
    }
    else open_view(SETTINGS);
}

void compact_workspace_message(const char *heading, const char *text, bool large, uint32_t generation)
{
    if (ui.has_message && ui.message_generation == generation) return;
    snprintf(ui.message_heading, sizeof(ui.message_heading), "%s", heading);
    snprintf(ui.message, sizeof(ui.message), "%s", text);
    ui.message_generation = generation;
    ui.has_message = true;
    ui.message_large = large;
    if (ui.view != REPLY_REVIEW && ui.view != CREATE && ui.view != MESSAGING) open_view(ACTIVITY);
}

unsigned compact_workspace_revision(void) { return ui.revision; }
bool compact_workspace_message_visible(void) { return ui.view == ACTIVITY && ui.has_message; }
const char *compact_workspace_view(void) { return view_names[ui.view]; }
const char *compact_workspace_selection(void)
{
    return list_view() && ui.focus >= 0 && ui.focus < ui.count ? ui.rows[ui.focus].name : "";
}
const char *compact_workspace_primary_action(void)
{
    static const char *const names[] = {"create", "open", "messaging", "back"};
    return ui.view == CREATE ? names[create_action()] : "";
}
int compact_workspace_message_line_height(void)
{
    if (ui.has_message) return ui.message_large ?
        (ui.small ? lv_font_montserrat_20.line_height : lv_font_montserrat_28.line_height) :
        (ui.small ? lv_font_montserrat_12.line_height : lv_font_montserrat_16.line_height);
    return font()->line_height;
}
