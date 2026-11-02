#include "room_text_composer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openclaw_reading.h"
#include "room_handwriting_pad.h"
#include "room_tab5_layout.h"

static lv_obj_t *root, *heading, *recipient, *editor, *detail, *keyboard, *review_body, *review_text, *notice;
static lv_obj_t *keys[6], *captions[6], *space, *remove_key, *page_key;
static lv_obj_t *back, *left, *right, *previous, *review, *review_caption, *handwrite;
static bool (*is_current)(void);
static void (*submit)(bool, const char *);
static uint32_t generation, epoch, pressed_epoch;
static int pressed = -1;
static unsigned page;
static int content_bottom = 432;
static bool full_warning;
static size_t full_bytes;
static bool reviewing, submitted, ready, available, keyboard_held, writing;
static uint32_t keyboard_epoch;
static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,?!'\"-:;/@_()[]{}<>+=*#%&\\|~`^$";
enum { BACK = 6, LEFT, RIGHT, SPACE, DELETE, PAGE, PREVIOUS, REVIEW, HANDWRITE };

static lv_obj_t *label(lv_obj_t *parent, const char *text, int width)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_obj_set_width(object, width);
    lv_label_set_long_mode(object, LV_LABEL_LONG_WRAP);
    lv_label_set_text(object, text);
    lv_obj_set_style_text_color(object, lv_color_hex(0xf4efee), 0);
    lv_obj_set_style_text_font(object, &lv_font_montserrat_14, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    return object;
}

static void layout(void);

static bool handwriting_current(void)
{
    return writing && !reviewing && !submitted && available && is_current();
}

static bool handwriting_accept(char character)
{
    if (!handwriting_current()) return false;
    size_t before = strlen(lv_textarea_get_text(editor));
    lv_textarea_add_char(editor, character);
    return strlen(lv_textarea_get_text(editor)) == before + 1;
}

static void handwriting_keyboard(void)
{
    writing = false;
    ++epoch;
    layout();
}

static void action(lv_event_t *event)
{
    int value = (int)(intptr_t)lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        pressed = is_current() ? value : -1;
        pressed_epoch = epoch;
    } else if (code == LV_EVENT_PRESS_LOST) pressed = -1;
    else if (code == LV_EVENT_SHORT_CLICKED) {
        bool valid = pressed == value && pressed_epoch == epoch && is_current();
        pressed = -1;
        if (!valid) return;
        if (value == BACK) { submit(false, NULL); return; }
        if (submitted) return;
        if (value == HANDWRITE) writing = true;
        else if (value == REVIEW) {
            if (reviewing) {
                if (lv_textarea_get_text(editor)[0] != '\0') submit(true, lv_textarea_get_text(editor));
            } else reviewing = true;
        } else if (reviewing && value == PREVIOUS) reviewing = false;
        else if (!reviewing) {
            if (value < 6 && page * 6 + (unsigned)value < sizeof(alphabet) - 1)
                lv_textarea_add_char(editor, alphabet[page * 6 + value]);
            else if (value == LEFT) lv_textarea_cursor_left(editor);
            else if (value == RIGHT) lv_textarea_cursor_right(editor);
            else if (value == SPACE) lv_textarea_add_char(editor, ' ');
            else if (value == DELETE) lv_textarea_delete_char(editor);
            else if (value == PAGE) page = (page + 1) % ((sizeof(alphabet) + 4) / 6);
            else if (value == PREVIOUS) page = (page + (sizeof(alphabet) + 4) / 6 - 1) % ((sizeof(alphabet) + 4) / 6);
        }
        ++epoch;
        layout();
    }
}

static lv_obj_t *button(const char *text, int x, int y, int width, int value)
{
    lv_obj_t *object = lv_obj_create(root);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, 44);
    lv_obj_set_style_radius(object, 12, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(0x302535), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(object, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(object, action, LV_EVENT_ALL, (void *)(intptr_t)value);
    lv_obj_t *caption = label(object, text, width - 8);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(caption);
    return object;
}

static void insert(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED) {
        if (full_warning && strlen(lv_textarea_get_text(editor)) != full_bytes) {
            full_warning = false;
            lv_label_set_text_static(detail, "");
        }
        return;
    }
    const char *text = lv_event_get_param(event);
    /* LVGL sends deletion through INSERT too; it never grows the draft. */
    if (text != NULL && text[0] == LV_KEY_DEL && text[1] == '\0') return;
    /* LVGL's maximum length counts characters. The chat contract counts bytes. */
    if (text != NULL && strlen(lv_textarea_get_text(editor)) + strlen(text) >= ESP_OPENCLAW_CHAT_TEXT_BYTES) {
        full_warning = true;
        full_bytes = strlen(lv_textarea_get_text(editor));
        lv_textarea_set_insert_replace(editor, "");
        lv_label_set_text_static(detail, "Draft full: 2047 bytes. Delete text to continue.");
    }
}

static void keyboard_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        keyboard_held = is_current() && !reviewing && !submitted && !writing;
        keyboard_epoch = epoch;
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        if (!keyboard_held || keyboard_epoch != epoch || !is_current() || reviewing || submitted || writing) return;
        lv_keyboard_def_event_cb(event);
        /* All maintained maps use release activation, including after case changes. */
        lv_buttonmatrix_set_button_ctrl_all(keyboard, LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT);
    } else if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        if (keyboard_held && keyboard_epoch == epoch && is_current() && !reviewing && !submitted && !writing) {
            reviewing = true; /* Both visible keyboard actions keep the draft for review. */
            keyboard_held = false;
            ++epoch;
            layout();
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) keyboard_held = false;
}

void room_text_composer_create(lv_obj_t *parent, bool (*current)(void), void (*handler)(bool, const char *))
{
    is_current = current;
    submit = handler;
    root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x15101b), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    back = button("Cancel", 26, 18, 116, BACK);
    handwrite = button("Handwrite", 152, 18, 80, HANDWRITE);
    left = button(LV_SYMBOL_LEFT, 242, 18, 44, LEFT);
    right = button(LV_SYMBOL_RIGHT, 298, 18, 44, RIGHT);
    heading = label(root, "Write message", 316);
    lv_obj_set_pos(heading, 26, 70);
    recipient = label(root, "", 316);
    lv_obj_set_pos(recipient, 26, 92);
    lv_label_set_long_mode(recipient, LV_LABEL_LONG_SCROLL_CIRCULAR);
    editor = lv_textarea_create(root);
    lv_obj_set_pos(editor, 26, 120);
    lv_obj_set_size(editor, 316, 98);
    lv_obj_set_style_text_font(editor, room_page_font(), 0);
    lv_textarea_set_max_length(editor, ESP_OPENCLAW_CHAT_TEXT_BYTES - 1);
    lv_textarea_set_placeholder_text(editor, "Type your message");
    lv_obj_add_event_cb(editor, insert, LV_EVENT_INSERT, NULL);
    lv_obj_add_event_cb(editor, insert, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_style_bg_color(editor, lv_color_hex(0x302535), 0);
    lv_obj_set_style_text_color(editor, lv_color_hex(0xf4efee), 0);
    review_body = lv_obj_create(root);
    lv_obj_set_pos(review_body, 26, 120);
    lv_obj_set_size(review_body, 316, 204);
    lv_obj_set_style_bg_color(review_body, lv_color_hex(0x302535), 0);
    lv_obj_set_scroll_dir(review_body, LV_DIR_VER);
    review_text = label(review_body, "", 288);
    lv_obj_set_style_text_font(review_text, room_page_font(), 0);
    notice = label(review_body, "Unicode escapes show unavailable glyphs; backslashes are doubled. Send preserves your original text.", 288);
    lv_obj_set_hidden(notice, true);
    detail = label(root, "", 316);
    lv_obj_set_pos(detail, 26, 224);
    for (int i = 0; i < 6; ++i) {
        keys[i] = button("", 26 + i % 3 * 108, 228 + i / 3 * 50, 100, i);
        captions[i] = lv_obj_get_child(keys[i], 0);
    }
    space = button("Space", 26, 328, 100, SPACE);
    remove_key = button(LV_SYMBOL_BACKSPACE, 134, 328, 100, DELETE);
    page_key = button("Next keys", 242, 328, 100, PAGE);
    previous = button("Prev keys", 26, 384, 148, PREVIOUS);
    review = button("Review", 186, 384, 156, REVIEW);
    review_caption = lv_obj_get_child(review, 0);
    if (room_tab5_layout()) {
        lv_obj_set_pos(back, 64, 32);
        lv_obj_set_pos(heading, 224, 40);
        lv_obj_set_style_text_font(heading, room_page_font(), 0);
        lv_obj_set_pos(recipient, 160, 110);
        lv_obj_set_width(recipient, 960);
        lv_obj_set_pos(editor, 160, 152);
        lv_obj_set_width(editor, 960);
        lv_obj_set_pos(review_body, 160, 152);
        lv_obj_set_size(review_body, 960, 330);
        lv_obj_set_width(review_text, 920);
        lv_obj_set_width(notice, 920);
        lv_obj_set_pos(detail, 160, 350);
        lv_obj_set_width(detail, 960);
        lv_obj_set_pos(left, 1008, 32);
        lv_obj_set_pos(right, 1076, 32);
        lv_obj_set_pos(handwrite, 800, 32);
        lv_obj_set_width(handwrite, 160);
        lv_obj_set_pos(previous, 160, 600);
        lv_obj_set_width(previous, 300);
        lv_obj_set_pos(review, 820, 600);
        lv_obj_set_width(review, 300);
        keyboard = lv_keyboard_create(root);
        lv_obj_set_align(keyboard, LV_ALIGN_TOP_LEFT);
        lv_obj_set_pos(keyboard, 160, 380);
        lv_obj_set_size(keyboard, 960, 220);
        lv_obj_set_style_pad_all(keyboard, 2, 0);
        lv_obj_set_style_pad_row(keyboard, 2, 0);
        /* Review and Send belong to this screen, never the keyboard's OK key. */
        lv_keyboard_set_textarea(keyboard, editor);
        lv_obj_remove_event_cb(keyboard, lv_keyboard_def_event_cb);
        lv_obj_add_event_cb(keyboard, keyboard_event, LV_EVENT_ALL, NULL);
        lv_buttonmatrix_set_button_ctrl_all(keyboard, LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT);
    }
    room_handwriting_pad_create(root, handwriting_current, handwriting_accept, handwriting_keyboard);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
}

void room_text_composer_open(uint32_t value, const char *text)
{
    generation = value;
    page = 0;
    reviewing = submitted = full_warning = writing = false;
    pressed = -1;
    keyboard_held = false;
    ++epoch;
    lv_textarea_set_text(editor, text == NULL ? "" : text);
    lv_textarea_set_cursor_pos(editor, LV_TEXTAREA_CURSOR_LAST);
    lv_label_set_text_static(detail, "");
    layout();
}

/* Presentation only: the textarea and queued Send retain the original UTF8.
 * Input is already produced by LVGL or the transcription boundary. */
static bool render_review(void)
{
    const unsigned char *text = (const unsigned char *)lv_textarea_get_text(editor);
    char *display = malloc(strlen((const char *)text) * 6 + 1);
    if (display == NULL) {
        lv_label_set_text_static(review_text, "Not enough memory to review. Cancel and try again.");
        return false;
    }
    char *out = display;
    bool escaped = false;
    while (*text != '\0') {
        const unsigned char *start = text;
        uint32_t point = *text++;
        if (point >= 0x80) {
            unsigned tails = point < 0xe0 ? 1 : point < 0xf0 ? 2 : 3;
            point &= (1u << (6 - tails)) - 1;
            while (tails--) point = (point << 6) | (*text++ & 0x3f);
        }
        lv_font_glyph_dsc_t glyph;
        bool supported = point == '\n' || point == '\t' ||
            (lv_font_get_glyph_dsc(room_page_font(), &glyph, point, 0) && !glyph.is_placeholder);
        if (!supported) {
            out += sprintf(out, point <= 0xffff ? "\\u%04lX" : "\\U%08lX", (unsigned long)point);
            escaped = true;
        } else if (point == '\\') { *out++ = '\\'; *out++ = '\\'; escaped = true; }
        else { size_t bytes = (size_t)(text - start); memcpy(out, start, bytes); out += bytes; }
    }
    *out = '\0';
    lv_label_set_text(review_text, display);
    free(display);
    lv_obj_set_hidden(notice, !escaped);
    if (escaped) {
        lv_obj_update_layout(review_text);
        lv_obj_align_to(notice, review_text, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 12);
    }
    return true;
}

static void layout(void)
{
    bool wide = room_tab5_layout();
    bool editing = !reviewing && !submitted && available && !writing;
    for (unsigned i = 0; i < 6; ++i) {
        unsigned index = page * 6 + i;
        char text[2] = {index < sizeof(alphabet) - 1 ? alphabet[index] : '\0', '\0'};
        lv_label_set_text(captions[i], text);
        lv_obj_set_y(keys[i], 228 + i / 3 * 50);
        lv_obj_set_hidden(keys[i], wide || !editing || text[0] == '\0');
    }
    lv_obj_set_y(space, 328);
    lv_obj_set_y(remove_key, 328);
    lv_obj_set_y(page_key, 328);
    lv_obj_set_hidden(space, wide || !editing);
    lv_obj_set_hidden(remove_key, wide || !editing);
    lv_obj_set_hidden(page_key, wide || !editing);
    lv_obj_set_hidden(left, !editing);
    lv_obj_set_hidden(right, !editing);
    lv_obj_set_hidden(handwrite, !editing);
    lv_obj_set_hidden(detail, (writing && available) || (!wide && editing));
    if (keyboard != NULL) lv_obj_set_hidden(keyboard, !editing);
    lv_obj_set_hidden(previous, writing || submitted || !available || (wide && editing));
    lv_obj_set_hidden(review, writing || submitted);
    bool compact = content_bottom < (wide ? 600 : 400);
    lv_obj_set_height(editor, wide ? compact ? 96 : 184 : compact ? 50 : 98);
    lv_obj_set_height(review_body, wide ? compact ? 272 : 330 : compact ? 142 : 204);
    lv_obj_set_y(detail, wide ? editing ? compact ? 250 : 350 : compact ? 432 : 492 : compact ? 270 : 332);
    lv_obj_set_height(detail, wide ? 44 : 48);
    if (!wide && compact) {
        for (unsigned i = 0; i < 6; ++i) lv_obj_set_y(keys[i], 178 + i / 3 * 50);
        lv_obj_set_y(space, 278);
        lv_obj_set_y(remove_key, 278);
        lv_obj_set_y(page_key, 278);
    }
    if (keyboard != NULL) {
        lv_obj_set_y(keyboard, compact ? 280 : 380);
        lv_obj_set_height(keyboard, compact ? 192 : 220);
    }
    lv_obj_set_y(previous, content_bottom - (wide ? 56 : 44));
    lv_obj_set_y(review, content_bottom - (wide ? 56 : 44));
    lv_label_set_text_static(lv_obj_get_child(previous, 0), reviewing ? "Edit" : "Prev keys");
    lv_label_set_text_static(review_caption, reviewing ? "Send" : "Review");
    bool review_ok = editing || writing || render_review();
    lv_obj_set_state(review, LV_STATE_DISABLED, !available || (reviewing && (!ready || !review_ok || lv_textarea_get_text(editor)[0] == '\0')));
    lv_label_set_text_static(lv_obj_get_child(back, 0), submitted ? "Done" : "Cancel");
    lv_label_set_text_static(heading, submitted ? "Message status" : !available ? "Draft unavailable" : reviewing ? "Review message" : writing ? "Handwriting" : "Write message");
    lv_obj_set_hidden(editor, !editing);
    lv_obj_set_hidden(review_body, editing || writing);
    room_handwriting_pad_show(writing && !reviewing && !submitted && available, content_bottom);
}

void room_text_composer_render(const room_voice_snapshot_t *draft, bool hidden, int bottom)
{
    if (root == NULL) return;
    lv_obj_set_hidden(root, hidden);
    if (hidden) { pressed = -1; keyboard_held = false; room_handwriting_pad_show(false, bottom); return; }
    bool changed = submitted != (draft->state >= ROOM_VOICE_SENDING);
    submitted = draft->state >= ROOM_VOICE_SENDING;
    if (changed) { ++epoch; pressed = -1; }
    bool same = draft->generation == generation;
    available = same && draft->state == ROOM_VOICE_DRAFT;
    ready = available && draft->can_send;
    content_bottom = bottom;
    layout();
    char destination[192];
    snprintf(destination, sizeof(destination), "%s%s%s%s", draft->recipient,
        draft->conversation[0] ? " / " : "", draft->conversation, submitted ? "" : " / Not sent");
    lv_label_set_text(recipient, same ? destination : "Preparing destination...");
    if (same && !full_warning) lv_label_set_text(detail, draft->detail);

    if (!same || (draft->state != ROOM_VOICE_DRAFT && !submitted)) {
        lv_label_set_text_static(heading, "Draft unavailable");
        lv_obj_set_state(review, LV_STATE_DISABLED, true);
    }
}
