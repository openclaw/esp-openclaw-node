#include "room_live_talk.h"

#include <stdio.h>
#include <string.h>
#include "openclaw_reading.h"

static lv_obj_t *root, *heading, *body, *text;
static bool tablet;

void room_live_talk_create(lv_obj_t *parent, bool wide)
{
    tablet = wide;
    root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_pos(root, wide ? 160 : 26, wide ? 184 : 104);
    lv_obj_set_size(root, wide ? 960 : 316, wide ? 352 : 254);
    lv_obj_set_style_bg_color(root, lv_color_hex(wide ? 0x1c1b19 : 0x15101b), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(root, false);
    heading = lv_label_create(root);
    lv_obj_set_width(heading, LV_PCT(100));
    lv_obj_set_style_text_color(heading, lv_color_hex(0xf1ede5), 0);
    body = lv_obj_create(root);
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, 0, wide ? 56 : 36);
    lv_obj_set_size(body, LV_PCT(100), wide ? 296 : 218);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    text = lv_label_create(body);
    lv_obj_set_width(text, wide ? 948 : 304);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(text, lv_color_hex(0xf1ede5), 0);
    lv_obj_set_clickable(text, false);
    lv_obj_set_hidden(root, true);
}

void room_live_talk_render(const esp_openclaw_talk_snapshot_t *view, bool hidden)
{
    if (root == NULL) return;
    bool entering = lv_obj_is_hidden(root) && !hidden;
    lv_obj_set_hidden(root, hidden);
    if (hidden) return;
    const lv_font_t *font = openclaw_reading_font();
#if LV_FONT_MONTSERRAT_28
    if (tablet) font = &lv_font_montserrat_28;
#endif
#if LV_FONT_MONTSERRAT_36
    if (tablet && openclaw_reading_size() == OPENCLAW_READING_LARGE) font = &lv_font_montserrat_36;
#endif
    lv_obj_set_style_text_font(heading, font, 0);
    lv_obj_set_style_text_font(text, font, 0);
    const char *state = view->sequence == 0 ? "Call open" :
        view->phase == ESP_OPENCLAW_TALK_SPEAKING ? "Agent speaking" :
        view->phase == ESP_OPENCLAW_TALK_THINKING ? "Thinking" :
        view->phase == ESP_OPENCLAW_TALK_ERROR ? "Conversation interrupted" : "Listening";
    lv_label_set_text(heading, state);
    static char transcript[2 * ESP_OPENCLAW_TALK_TEXT_BYTES + 160];
    snprintf(transcript, sizeof(transcript), "%s%s%s%s%s%s%s%s",
        view->user_text[0] ? view->user_final ? "You" : "You (partial)" : "",
        view->user_text[0] && view->user_incomplete ? " [incomplete]" : "",
        view->user_text[0] ? "\n" : "", view->user_text,
        view->agent_text[0] ? view->agent_final ? "\n\nAgent\n" : "\n\nAgent (partial)\n" : "",
        view->agent_text, view->agent_text[0] && view->agent_incomplete ? "\n[Incomplete text]" : "",
        !view->user_text[0] && !view->agent_text[0] ? "Your words will appear here as they arrive." : "");
    if (strcmp(lv_label_get_text(text), transcript) != 0) {
        bool follow = entering || lv_obj_get_scroll_bottom(body) <= 8;
        lv_label_set_text(text, transcript);
        lv_obj_update_layout(body);
        if (follow) lv_obj_scroll_to_y(body, lv_obj_get_scroll_y(body) + lv_obj_get_scroll_bottom(body), LV_ANIM_OFF);
        else lv_obj_readjust_scroll(body, LV_ANIM_OFF);
    }
}
