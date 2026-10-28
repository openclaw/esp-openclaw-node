#include "room_watch_home.h"
#include "openclaw_reading.h"
#include "esp_openclaw_node_wifi.h"
#include "openclaw_status_bar.h"

#include <string.h>
#include <stdio.h>

static lv_obj_t *root, *title_label, *detail_label;
static lv_obj_t *agent_button, *agent_name, *agent_initials, *agent_image;
static lv_image_dsc_t agent_images[3];
static lv_obj_t *continue_card, *reader_label;
static lv_obj_t *voice_button, *voice_label, *discard_button, *send_button, *send_label;
static lv_obj_t *review_panel, *review_heading, *review_recipient, *review_notice, *review_body, *review_text;
static uint32_t review_generation;
static room_voice_state_t review_state;
static openclaw_status_bar_t status_bar;
static bool wifi_connected, gateway_connected;
static bool display_awake;
static lv_timer_t *status_timer;

static void refresh_status(lv_timer_t *timer)
{
    (void)timer;
    if (!display_awake || !lv_obj_is_visible(root)) return;
    esp_openclaw_node_wifi_status_t wifi = {0};
    esp_openclaw_node_wifi_get_status(&wifi);
    const openclaw_status_facts_t status = {
        .wifi_connected = wifi_connected && wifi.connected,
        .signal_known = wifi.ap_info_valid,
        .rssi = wifi.rssi,
        .gateway_connected = gateway_connected,
    };
    openclaw_status_bar_set(&status_bar, &status);
}

void room_watch_home_set_awake(bool awake)
{
    if (display_awake == awake) return;
    display_awake = awake;
    if (status_timer == NULL) return;
    if (awake) {
        lv_timer_resume(status_timer);
        lv_timer_reset(status_timer);
        refresh_status(NULL);
    } else {
        lv_timer_pause(status_timer);
    }
}

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

static lv_obj_t *action(lv_obj_t *parent, const char *text, lv_event_cb_t handler, lv_obj_t **text_label)
{
    lv_obj_t *button = lv_obj_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_pos(button, 26, 376);
    lv_obj_set_size(button, 316, 52);
    lv_obj_set_style_radius(button, 20, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xf4efee), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xcbbdcf), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(button, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(button, handler, LV_EVENT_ALL, NULL);
    lv_obj_t *text_obj = label(button, text, 0, 0, 200, 0x251b2b);
    lv_obj_set_style_text_align(text_obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(text_obj);
    if (text_label != NULL) *text_label = text_obj;
    return button;
}

void room_watch_home_create(lv_obj_t *parent, lv_event_cb_t menu_handler, lv_event_cb_t agent_handler, lv_event_cb_t continue_handler,
                            lv_event_cb_t voice_handler, lv_event_cb_t discard_handler, lv_event_cb_t send_handler)
{
    root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(root, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *menu = lv_obj_create(root);
    lv_obj_remove_style_all(menu);
    lv_obj_set_pos(menu, 298, 39);
    lv_obj_set_size(menu, 44, 44);
    lv_obj_set_style_bg_color(menu, lv_color_hex(0x302535), 0);
    lv_obj_set_style_bg_opa(menu, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(menu, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_clickable(menu, true);
    lv_obj_set_scrollable(menu, false);
    lv_obj_add_event_cb(menu, menu_handler, LV_EVENT_ALL, NULL);
    lv_obj_t *menu_label = lv_label_create(menu);
    lv_label_set_text_static(menu_label, LV_SYMBOL_BARS);
    lv_obj_set_style_text_color(menu_label, lv_color_hex(0xf4efee), 0);
    lv_obj_set_style_text_font(menu_label, &lv_font_montserrat_14, 0);
    lv_obj_remove_flag(menu_label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(menu_label);
    agent_button = lv_obj_create(root);
    lv_obj_remove_style_all(agent_button);
    lv_obj_set_pos(agent_button, 26, 39);
    lv_obj_set_size(agent_button, 244, 44);
    lv_obj_set_style_bg_color(agent_button, lv_color_hex(0x302535), 0);
    lv_obj_set_style_bg_opa(agent_button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(agent_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_clickable(agent_button, true);
    lv_obj_set_scrollable(agent_button, false);
    lv_obj_add_event_cb(agent_button, agent_handler, LV_EVENT_ALL, NULL);
    agent_name = label(agent_button, "Automatic", 44, 14, 174, 0xf4efee);
    lv_obj_set_height(agent_name, 20);
    label(agent_button, LV_SYMBOL_DOWN, 224, 15, 14, 0xb8b0bd);
    agent_initials = label(agent_button, LV_SYMBOL_SHUFFLE, 8, 8, 28, 0xf4efee);
    lv_obj_set_height(agent_initials, 28);
    lv_obj_set_style_text_align(agent_initials, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(agent_initials, 6, 0);
    agent_image = lv_image_create(agent_button);
    lv_obj_set_pos(agent_image, 8, 8);
    lv_obj_set_size(agent_image, 28, 28);
    lv_image_set_inner_align(agent_image, LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_antialias(agent_image, true);
    lv_obj_set_clickable(agent_image, false);
    lv_obj_set_scrollable(agent_image, false);
    for (int kind = ESP_OPENCLAW_AVATAR_FOX; kind <= ESP_OPENCLAW_AVATAR_LOBSTER; ++kind) {
        agent_images[kind] = (lv_image_dsc_t){
            .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888,
                .w = ESP_OPENCLAW_AVATAR_SIZE, .h = ESP_OPENCLAW_AVATAR_SIZE,
                .stride = ESP_OPENCLAW_AVATAR_SIZE * 4},
            .data_size = ESP_OPENCLAW_AVATAR_SIZE * ESP_OPENCLAW_AVATAR_SIZE * 4,
            .data = esp_openclaw_avatar_pixels(kind),
        };
    }
    openclaw_status_bar_create(&status_bar, root);
    lv_obj_set_pos(status_bar.root, 248, 10);
    status_timer = lv_timer_create(refresh_status, 1000, NULL);
    if (!display_awake) lv_timer_pause(status_timer);
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
    review_panel = lv_obj_create(root);
    lv_obj_remove_style_all(review_panel);
    lv_obj_set_pos(review_panel, 18, 68);
    lv_obj_set_size(review_panel, 332, 298);
    lv_obj_remove_flag(review_panel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(review_panel, lv_color_hex(0x15101b), 0);
    lv_obj_set_style_bg_opa(review_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(review_panel, 20, 0);
    review_heading = label(review_panel, "Text draft", 8, 8, 316, 0xf4efee);
#if LV_FONT_MONTSERRAT_20
    lv_obj_set_style_text_font(review_heading, &lv_font_montserrat_20, 0);
#endif
    review_recipient = label(review_panel, "", 8, 40, 316, 0xb8b0bd);
    review_notice = label(review_panel, "", 8, 64, 316, 0xb8b0bd);
    lv_label_set_long_mode(review_notice, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(review_notice, 40);
    review_body = lv_obj_create(review_panel);
    lv_obj_remove_style_all(review_body);
    lv_obj_set_pos(review_body, 8, 72);
    lv_obj_set_size(review_body, 316, 216);
    lv_obj_set_scroll_dir(review_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(review_body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(review_body, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(review_body, lv_color_hex(0xb8b0bd), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(review_body, LV_OPA_60, LV_PART_SCROLLBAR);
    review_text = label(review_body, "", 0, 0, 306, 0xf4efee);
    lv_label_set_long_mode(review_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_hidden(review_panel, true);
    voice_button = action(root, "Hold to record", voice_handler, &voice_label);
    discard_button = action(root, "Discard", discard_handler, NULL);
    lv_obj_set_size(discard_button, 104, 52);
    lv_obj_set_style_bg_color(discard_button, lv_color_hex(0x382b40), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(discard_button, 0), lv_color_hex(0xf4efee), 0);
    lv_obj_set_width(lv_obj_get_child(discard_button, 0), 100);
    lv_obj_center(lv_obj_get_child(discard_button, 0));
    lv_obj_set_hidden(voice_button, true);
    lv_obj_set_hidden(discard_button, true);
    send_button = action(root, "Send", send_handler, &send_label);
    lv_obj_set_pos(send_button, 138, 376);
    lv_obj_set_size(send_button, 204, 52);
    lv_label_set_text_static(send_label, LV_SYMBOL_UP "  Send");
    lv_obj_center(send_label);
    lv_obj_set_hidden(send_button, true);
}

bool room_watch_home_review_visible(void)
{
    return review_panel != NULL && lv_obj_is_visible(review_panel);
}

bool room_watch_home_render(const room_ui_facts_t *facts, const room_ui_talk_target_t *target, room_ui_state_t state,
                           const char *detail, const char *reader_agent, const room_voice_snapshot_t *voice,
                           bool voice_enabled, bool holding, bool hidden)
{
    if (root == NULL) return false;
    bool was_hidden = !lv_obj_is_visible(root);
    lv_obj_set_flag(root, LV_OBJ_FLAG_HIDDEN, hidden);
    if (hidden) return false;
    bool wifi = facts->wifi == ROOM_UI_WIFI_CONNECTED;
    bool connected = facts->gateway == ROOM_UI_GATEWAY_CONNECTED;
    bool connection_changed = wifi_connected != wifi || gateway_connected != connected;
    wifi_connected = wifi;
    gateway_connected = connected;
    if (was_hidden || connection_changed) refresh_status(NULL);

    bool voice_busy = voice != NULL && voice->state >= ROOM_VOICE_STARTING;
    bool draft = voice != NULL && voice->state == ROOM_VOICE_DRAFT;
    bool canceling = voice != NULL && voice->state == ROOM_VOICE_CANCELING;
    bool transcribing = voice != NULL && (voice->state == ROOM_VOICE_TRANSCRIBING || canceling);
    bool transcription_failed = voice != NULL && voice->state == ROOM_VOICE_TRANSCRIPTION_ERROR;
    bool submitted = voice != NULL && voice->state >= ROOM_VOICE_SENDING;
    bool sending = voice != NULL && voice->state == ROOM_VOICE_SENDING;
    bool review = (draft || transcribing || transcription_failed || submitted) && !holding;
    bool retry = voice != NULL && (voice->state == ROOM_VOICE_ERROR || transcription_failed);
    bool split = (draft || transcription_failed) && !holding;
    bool call = facts->talk == ROOM_UI_TALK_CONNECTING || facts->talk == ROOM_UI_TALK_ACTIVE ||
        facts->talk == ROOM_UI_TALK_STOPPING;
    lv_obj_set_hidden(agent_button, call || voice_busy);
    lv_label_set_text(agent_name, target->expired ? "Choose agent" : target->selected ? target->name : "Automatic");
    bool picture = target->selected && !target->expired &&
        (target->avatar.kind == ESP_OPENCLAW_AVATAR_FOX || target->avatar.kind == ESP_OPENCLAW_AVATAR_LOBSTER);
    lv_obj_set_hidden(agent_image, !picture);
    lv_obj_set_hidden(agent_initials, picture);
    if (picture) lv_image_set_src(agent_image, &agent_images[target->avatar.kind]);
    else lv_label_set_text(agent_initials, target->expired ? LV_SYMBOL_REFRESH : !target->selected ? LV_SYMBOL_SHUFFLE :
        target->avatar.text[0] ? target->avatar.text : "?");
    lv_obj_set_hidden(review_panel, !review);
    lv_obj_set_hidden(voice_button, voice == NULL || ((transcribing || draft || submitted) && !holding));
    lv_obj_set_hidden(send_button, !(draft || submitted) || holding);
    lv_label_set_text_static(send_label, sending ? "Sending..." : submitted ? "Conversations" : LV_SYMBOL_UP "  Send");
    lv_obj_center(send_label);
    lv_obj_set_state(send_button, LV_STATE_DISABLED, sending || (draft && !voice->can_send));
    lv_obj_set_hidden(discard_button, !review);
    lv_obj_set_size(discard_button, transcribing ? 316 : 104, 52);
    lv_label_set_text_static(lv_obj_get_child(discard_button, 0), canceling ? "Canceling..." : transcribing ? "Cancel" : submitted ? "Done" : "Discard");
    lv_obj_center(lv_obj_get_child(discard_button, 0));
    lv_obj_set_state(discard_button, LV_STATE_DISABLED, canceling);
    lv_obj_set_pos(voice_button, split ? 138 : 26, 376);
    lv_obj_set_width(voice_button, split ? 204 : 316);
    lv_obj_center(voice_label);
    lv_label_set_text_static(voice_label, split || retry ? "Hold to record again" :
        voice != NULL && voice->state == ROOM_VOICE_FINISHING ? "Finishing..." :
        voice_busy && !retry ? "Release to keep" : "Hold to record");
    /* Keep a pressed recording target enabled through the duration cap so its
     * release cannot activate the replacement draft controls. */
    lv_obj_set_state(voice_button, LV_STATE_DISABLED, !voice_enabled);
    bool reading = !voice_busy && reader_agent != NULL && reader_agent[0] != '\0' && wifi && connected &&
        state != ROOM_UI_ERROR && state != ROOM_UI_SETUP &&
        facts->talk != ROOM_UI_TALK_CONNECTING && facts->talk != ROOM_UI_TALK_ACTIVE &&
        facts->talk != ROOM_UI_TALK_STOPPING;
    lv_obj_set_hidden(continue_card, !reading);
    lv_obj_set_hidden(title_label, reading);
    lv_obj_set_hidden(detail_label, reading);
    if (review) {
        lv_obj_set_style_text_font(review_text, openclaw_reading_font(), 0);
        lv_obj_set_hidden(title_label, true);
        lv_obj_set_hidden(detail_label, true);
        lv_label_set_text_static(review_heading, sending ? "Sending" : submitted ?
            voice->state == ROOM_VOICE_ACCEPTED ? "Accepted" : voice->state == ROOM_VOICE_SENT ? "Sent" :
            voice->state == ROOM_VOICE_SEND_UNKNOWN ? "Check delivery" : "Turn stopped" :
            canceling ? "Canceling" : transcribing ? "Transcribing" : transcription_failed ? "Couldn't transcribe" : "Text draft");
        char recipient[96];
        snprintf(recipient, sizeof(recipient), "%s%s", voice->recipient, submitted ? "" : " / Not sent");
        bool instruction = draft && voice->detail[0] != '\0';
        lv_obj_set_hidden(review_notice, !instruction);
        if (instruction) lv_label_set_text(review_notice, voice->detail);
        lv_obj_set_y(review_body, instruction ? 112 : 72);
        lv_obj_set_height(review_body, instruction ? 176 : 216);
        lv_label_set_text(review_recipient, recipient);
        const char *text = draft ? voice->text : submitted || transcription_failed ? voice->detail : canceling ? "Discarding this recording." :
            "Turning your recording into text.\n\nNothing is sent to your agent. Review the text when it is ready.";
        bool changed = review_generation != voice->generation || review_state != voice->state ||
            strcmp(lv_label_get_text(review_text), text) != 0;
        if (changed) {
            lv_label_set_text(review_text, text);
            lv_obj_scroll_to_y(review_body, 0, LV_ANIM_OFF);
        }
        review_generation = voice->generation;
        review_state = voice->state;
        return false;
    }
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
    if (voice != NULL) {
        if (voice->state == ROOM_VOICE_STARTING) {
            title = "Opening microphone";
            subtitle = "Release to keep / slide up to discard";
        } else if (voice->state == ROOM_VOICE_RECORDING || draft) {
            snprintf(next_agent, sizeof(next_agent), "%s: %u.%u s", draft ? "Audio draft" : "Recording",
                (unsigned)(voice->duration_ms / 1000), (unsigned)(voice->duration_ms / 100 % 10));
            title = next_agent;
            subtitle = draft ? "Not sent. Discarded when you leave." : "Release to keep / slide up to discard";
        } else if (voice->state == ROOM_VOICE_FINISHING) {
            title = "Finishing recording";
            subtitle = "Keeping your audio on this device";
        } else if (retry) {
            title = "Couldn't record";
            subtitle = voice->detail;
        } else if (voice_enabled) {
            snprintf(next_agent, sizeof(next_agent), "For %s", target->selected ? target->name : "default agent");
            title = "Ready when you are";
            subtitle = next_agent;
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
