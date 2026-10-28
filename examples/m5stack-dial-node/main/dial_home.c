#include "dial_home.h"
#include "esp_openclaw_avatar.h"

#include <string.h>

static lv_image_dsc_t avatar_sources[3];

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int width)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, width);
    lv_obj_set_height(object, 20);
    lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(object, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(0xf4efee), 0);
    lv_obj_set_clickable(object, false);
    lv_label_set_text_static(object, text);
    return object;
}

static lv_obj_t *button(lv_obj_t *parent, int x, int y, int width, int height, uint32_t color)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(0x63546b), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_clickable(object, true);
    lv_obj_set_scrollable(object, false);
    return object;
}

void dial_home_create(dial_home_t *home, lv_obj_t *parent)
{
    for (int kind = ESP_OPENCLAW_AVATAR_FOX; kind <= ESP_OPENCLAW_AVATAR_LOBSTER; ++kind)
        avatar_sources[kind] = (lv_image_dsc_t){
            .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888,
                .w = ESP_OPENCLAW_AVATAR_SIZE, .h = ESP_OPENCLAW_AVATAR_SIZE,
                .stride = ESP_OPENCLAW_AVATAR_SIZE * 4},
            .data_size = ESP_OPENCLAW_AVATAR_SIZE * ESP_OPENCLAW_AVATAR_SIZE * 4,
            .data = esp_openclaw_avatar_pixels(kind),
        };
    home->root = lv_obj_create(parent);
    lv_obj_remove_style_all(home->root);
    lv_obj_set_size(home->root, 240, 240);
    lv_obj_set_clickable(home->root, false);
    lv_obj_set_scrollable(home->root, false);
    openclaw_status_bar_create(&home->status, home->root);
    lv_obj_set_pos(home->status.root, 73, 20);
    home->agent_button = button(home->root, 42, 40, 110, 44, 0x302535);
    home->agent_avatar = lv_image_create(home->agent_button);
    lv_obj_set_pos(home->agent_avatar, 8, 12);
    lv_obj_set_size(home->agent_avatar, 20, 20);
    lv_image_set_inner_align(home->agent_avatar, LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_antialias(home->agent_avatar, true);
    lv_obj_set_clickable(home->agent_avatar, false);
    lv_obj_set_scrollable(home->agent_avatar, false);
    lv_obj_set_hidden(home->agent_avatar, true);
    home->agent_initial = label(home->agent_button, LV_SYMBOL_DIRECTORY, 8, 12, 20);
    home->title = label(home->agent_button, "Agents", 30, 12, 61);
    lv_obj_set_style_text_align(home->title, LV_TEXT_ALIGN_LEFT, 0);
    label(home->agent_button, LV_SYMBOL_DOWN, 92, 12, 12);
    home->menu_button = button(home->root, 158, 42, 40, 40, 0x302535);
    lv_obj_set_ext_click_area(home->menu_button, 2);
    lv_obj_center(label(home->menu_button, LV_SYMBOL_BARS, 0, 0, 28));
    lv_obj_t *frame = lv_obj_create(home->root);
    lv_obj_remove_style_all(frame);
    lv_obj_set_pos(frame, 85, 86);
    lv_obj_set_size(frame, 70, 70);
    lv_obj_set_style_radius(frame, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(frame, lv_color_hex(0x332336), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(frame, 1, 0);
    lv_obj_set_style_border_color(frame, lv_color_hex(0x5a3946), 0);
    lv_obj_set_clickable(frame, false);
    lv_obj_set_scrollable(frame, false);
    home->headline = label(home->root, "Starting up", 32, 159, 176);
    lv_obj_set_style_text_font(home->headline, &lv_font_montserrat_16, 0);
    home->primary_button = button(home->root, 64, 187, 112, 28, 0xf4efee);
    lv_obj_set_ext_click_area(home->primary_button, 8);
    home->action = label(home->primary_button, "Connection", 0, 0, 108);
    lv_obj_set_style_text_color(home->action, lv_color_hex(0x302131), 0);
    lv_obj_center(home->action);
    for (int i = 0; i < 2; ++i) {
        home->eyes[i] = lv_obj_create(home->root);
        lv_obj_set_style_bg_color(home->eyes[i], lv_color_hex(0x58d5c4), 0);
        lv_obj_set_style_border_width(home->eyes[i], 0, 0);
        lv_obj_set_style_radius(home->eyes[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_remove_flag(home->eyes[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    home->mouth = lv_label_create(home->root);
    lv_obj_set_width(home->mouth, 60);
    lv_obj_set_style_text_align(home->mouth, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(home->mouth, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(home->mouth, lv_color_white(), 0);
    lv_label_set_text_static(home->mouth, "~");
    lv_obj_align(home->mouth, LV_ALIGN_CENTER, 0, 12);
    dial_home_animate(home, "neutral", lv_tick_get());
}

void dial_home_animate(dial_home_t *home, const char *mood, uint32_t now)
{
    uint32_t phase = now % 4400;
    int height = 28;
    if (strcmp(mood, "sleepy") == 0) height = 10;
    else if (strcmp(mood, "happy") == 0) height = 18;
    else if (strcmp(mood, "sad") == 0) height = 24;
    else if (strcmp(mood, "excited") == 0) height = 32;
    int gaze = (int)((now / 1800) % 3) * 4 - 4;
    bool thinking = strcmp(mood, "thinking") == 0;
    for (int i = 0; i < 2; ++i) {
        int eye_height = phase < 140 ? 4 : thinking && i == 0 ? 22 : height;
        lv_obj_set_size(home->eyes[i], 19, eye_height);
        lv_obj_align(home->eyes[i], LV_ALIGN_CENTER, (i == 0 ? -21 : 21) + gaze,
                     thinking ? -17 : strcmp(mood, "sad") == 0 ? -9 : -13);
    }
    const char *mouth = strcmp(mood, "sad") == 0 ? "-" : strcmp(mood, "excited") == 0 ? "o" : "~";
    if (strcmp(lv_label_get_text(home->mouth), mouth) != 0) lv_label_set_text_static(home->mouth, mouth);
}

void dial_home_render(dial_home_t *home, const openclaw_status_facts_t *status, bool assigned,
                      const char *activity, const dial_home_context_t *context)
{
    openclaw_status_bar_set(&home->status, status);
    bool connected = status->gateway_connected;
    home->destination = !connected ? DIAL_HOME_CONNECTION : !context->available ? DIAL_HOME_AGENTS
        : context->preview ? DIAL_HOME_PREVIEW : DIAL_HOME_READER;
    const char *action = home->destination == DIAL_HOME_CONNECTION ? "Connection"
        : home->destination == DIAL_HOME_AGENTS ? "Choose agent"
        : home->destination == DIAL_HOME_PREVIEW ? "Continue" : "Conversations";
    lv_label_set_text_static(home->action, action);
    esp_openclaw_avatar_t avatar = esp_openclaw_agent_avatar(&context->agent);
    bool picture = context->available && avatar.kind != ESP_OPENCLAW_AVATAR_TEXT;
    lv_obj_set_hidden(home->agent_avatar, !picture);
    lv_obj_set_hidden(home->agent_initial, picture);
    if (picture) lv_image_set_src(home->agent_avatar, &avatar_sources[avatar.kind]);
    else if (context->available) {
        const char initial[] = {avatar.text[0], '\0'};
        lv_label_set_text(home->agent_initial, initial);
    } else lv_label_set_text_static(home->agent_initial, LV_SYMBOL_DIRECTORY);
    const char *title = context->available ? context->agent.name : "Agents";
    if (strcmp(lv_label_get_text(home->title), title) != 0)
        lv_label_set_text(home->title, title);
    const char *headline = activity != NULL ? activity : !connected ? "Gateway offline"
        : context->available ? "Ready to continue" : assigned ? "Control ready" : "Ready when you are";
    if (strcmp(lv_label_get_text(home->headline), headline) != 0)
        lv_label_set_text(home->headline, headline);
}
