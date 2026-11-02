#include "room_handwriting_pad.h"

#include <ctype.h>
#include "room_handwriting.h"
#include "room_tab5_layout.h"

extern const uint8_t model[] __asm__("_binary_handwriting_english36_bin_start");
static lv_obj_t *root, *ink, *status, *candidates[3], *clear, *letter_case, *recognize, *keyboard, *accept;
static bool (*is_current)(void), (*insert)(char);
static void (*leave)(void);
static room_handwriting_point_t points[ROOM_HANDWRITING_POINTS_MAX];
static size_t count;
static char choices[3];
static int selected = -1, pressed = -1;
static uint32_t epoch, pressed_epoch;
static bool shown, drawing, overflow, uppercase;
static int last_bottom;
enum { CLEAR = 3, CASE, RECOGNIZE, KEYBOARD, ACCEPT };

static void show_choices(void)
{
    for (unsigned i = 0; i < 3; ++i) {
        char text[2] = {uppercase ? (char)toupper((unsigned char)choices[i]) : choices[i], '\0'};
        lv_label_set_text(lv_obj_get_child(candidates[i], 0), text[0] ? text : "-");
        lv_obj_set_state(candidates[i], LV_STATE_DISABLED, !text[0]);
        lv_obj_set_state(candidates[i], LV_STATE_CHECKED, selected == (int)i);
    }
    lv_obj_set_state(accept, LV_STATE_DISABLED, selected < 0 || overflow);
    lv_label_set_text_static(lv_obj_get_child(letter_case, 0), uppercase ? "ABC" : "abc");
}

static void reset_ink(void)
{
    count = 0;
    choices[0] = choices[1] = choices[2] = '\0';
    selected = -1;
    drawing = overflow = false;
    lv_label_set_text_static(status, "One English letter or digit at a time");
    show_choices();
    lv_obj_invalidate(ink);
}

static void action(lv_event_t *event)
{
    int value = (int)(intptr_t)lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        pressed = shown && is_current() ? value : -1;
        pressed_epoch = epoch;
    } else if (code == LV_EVENT_PRESS_LOST) pressed = -1;
    else if (code == LV_EVENT_SHORT_CLICKED) {
        bool valid = pressed == value && pressed_epoch == epoch && shown && is_current();
        pressed = -1;
        if (!valid) return;
        ++epoch;
        if (value < 3 && choices[value]) selected = value;
        else if (value == CLEAR) reset_ink();
        else if (value == CASE) uppercase = !uppercase;
        else if (value == KEYBOARD) { leave(); return; }
        else if (value == ACCEPT && selected >= 0 && !overflow) {
            char character = uppercase ? (char)toupper((unsigned char)choices[selected]) : choices[selected];
            if (insert(character)) {
                reset_ink();
                lv_label_set_text_fmt(status, "Added %c. Write the next character.", character);
            } else lv_label_set_text_static(status, "Draft full. Use Keyboard to delete text.");
        } else if (value == RECOGNIZE && !overflow) {
            selected = -1;
            room_handwriting_result_t result = room_handwriting_recognize(model,
                ROOM_HANDWRITING_MODEL_BYTES, points, count, choices);
            if (result != ROOM_HANDWRITING_READY) choices[0] = choices[1] = choices[2] = '\0';
            lv_label_set_text_static(status, result == ROOM_HANDWRITING_READY ? "Choose a candidate, then Accept" :
                result == ROOM_HANDWRITING_EMPTY ? "Draw a larger character, then Recognize" :
                result == ROOM_HANDWRITING_DENSE ? "Too much ink. Clear and try one character." :
                "Recognition unavailable. Use Keyboard.");
        }
        show_choices();
    }
}

static lv_obj_t *button(const char *text, int value)
{
    lv_obj_t *object = lv_obj_create(root);
    lv_obj_remove_style_all(object);
    lv_obj_set_height(object, 44);
    lv_obj_set_style_radius(object, 12, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(0x302535), 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(0x704769), LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(object, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(object, action, LV_EVENT_ALL, (void *)(intptr_t)value);
    lv_obj_t *label = lv_label_create(object);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xf4efee), 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(label);
    return object;
}

static void record_point(bool start)
{
    lv_indev_t *device = lv_indev_active();
    if (device == NULL) return;
    lv_point_t point;
    lv_area_t bounds;
    lv_indev_get_point(device, &point);
    lv_obj_get_coords(ink, &bounds);
    if (point.x < bounds.x1 || point.x > bounds.x2 || point.y < bounds.y1 || point.y > bounds.y2) {
        drawing = false;
        return;
    }
    int x = point.x - bounds.x1, y = point.y - bounds.y1;
    if (!start && count && points[count - 1].x == x && points[count - 1].y == y) return;
    if (count == ROOM_HANDWRITING_POINTS_MAX) {
        overflow = true;
        drawing = false;
        lv_label_set_text_static(status, "Too many points. Clear and try one character.");
        show_choices();
        return;
    }
    points[count++] = (room_handwriting_point_t){x, y, start};
    lv_obj_invalidate(ink);
}

static void ink_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DRAW_MAIN) {
        lv_area_t bounds;
        lv_obj_get_coords(ink, &bounds);
        lv_draw_line_dsc_t line;
        lv_draw_line_dsc_init(&line);
        line.color = lv_color_white();
        line.width = room_tab5_layout() ? 6 : 3;
        line.round_start = line.round_end = true;
        for (size_t i = 0; i < count; ++i) {
            size_t before = i == 0 || points[i].start ? i : i - 1;
            line.p1 = (lv_point_precise_t){bounds.x1 + points[before].x, bounds.y1 + points[before].y};
            line.p2 = (lv_point_precise_t){bounds.x1 + points[i].x, bounds.y1 + points[i].y};
            if (before == i) ++line.p2.x;
            lv_draw_line(lv_event_get_layer(event), &line);
        }
    } else if (code == LV_EVENT_PRESSED) {
        drawing = shown && !overflow && is_current();
        if (drawing) {
            choices[0] = choices[1] = choices[2] = '\0';
            selected = -1;
            ++epoch;
            show_choices();
            lv_label_set_text_static(status, "Lift between strokes. Tap Recognize when done.");
            record_point(true);
        }
    } else if (code == LV_EVENT_PRESSING || code == LV_EVENT_RELEASED) {
        if (drawing && shown && is_current()) record_point(false);
        if (code == LV_EVENT_RELEASED) drawing = false;
    } else if (code == LV_EVENT_PRESS_LOST) drawing = false;
}

void room_handwriting_pad_create(lv_obj_t *parent, bool (*current)(void),
    bool (*accepted)(char), void (*typing)(void))
{
    is_current = current;
    insert = accepted;
    leave = typing;
    root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    ink = lv_obj_create(root);
    lv_obj_remove_style_all(ink);
    lv_obj_set_style_bg_color(ink, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ink, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ink, 1, 0);
    lv_obj_set_style_border_color(ink, lv_color_hex(0x705f70), 0);
    lv_obj_remove_flag(ink, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ink, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ink, ink_event, LV_EVENT_ALL, NULL);
    status = lv_label_create(root);
    lv_obj_set_style_text_color(status, lv_color_hex(0xf4efee), 0);
    lv_obj_set_style_text_font(status, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(status, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_remove_flag(status, LV_OBJ_FLAG_CLICKABLE);
    for (unsigned i = 0; i < 3; ++i) candidates[i] = button("-", i);
    clear = button("Clear", CLEAR);
    letter_case = button("abc", CASE);
    recognize = button("Recognize", RECOGNIZE);
    keyboard = button("Keyboard", KEYBOARD);
    accept = button("Accept", ACCEPT);
    reset_ink();
    lv_obj_set_hidden(root, true);
}

void room_handwriting_pad_show(bool visible, int bottom)
{
    if (shown != visible || (visible && last_bottom != bottom)) {
        ++epoch;
        pressed = -1;
        reset_ink();
    }
    shown = visible;
    last_bottom = bottom;
    lv_obj_set_hidden(root, !visible);
    if (!visible) return;
    bool wide = room_tab5_layout();
    int width = wide ? 960 : 316, top = wide ? 152 : 112, height = bottom - top;
    lv_obj_set_pos(root, wide ? 160 : 26, top);
    lv_obj_set_size(root, width, height);
    lv_obj_set_size(status, width, 18);
    lv_obj_set_pos(ink, 0, 24);
    lv_obj_set_size(ink, width, height - 172);
    for (unsigned i = 0; i < 3; ++i) {
        lv_obj_set_pos(candidates[i], i * (width + 8) / 3, height - 140);
        lv_obj_set_width(candidates[i], (width - 16) / 3);
    }
    lv_obj_t *controls[] = {clear, letter_case, recognize};
    for (unsigned i = 0; i < 3; ++i) {
        lv_obj_set_pos(controls[i], i * (width + 8) / 3, height - 92);
        lv_obj_set_width(controls[i], (width - 16) / 3);
    }
    lv_obj_set_pos(keyboard, 0, height - 44);
    lv_obj_set_width(keyboard, (width - 12) / 2);
    lv_obj_set_pos(accept, (width + 12) / 2, height - 44);
    lv_obj_set_width(accept, (width - 12) / 2);
}
