#include "dial_input.h"

#include <stdlib.h>

static void emit(dial_input_t *input, dial_input_event_t event, bool pointer)
{
    input->handler(event, lv_arc_get_value(input->arc), pointer, input->context);
}

static void pointer_motion(dial_input_t *input, lv_indev_t *indev)
{
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    int limit = LV_INDEV_DEF_SCROLL_LIMIT;
    if (abs(point.x - input->press_point.x) > limit ||
        abs(point.y - input->press_point.y) > limit) input->pointer_moved = true;
}

static void object_event(lv_event_t *event)
{
    dial_input_t *input = lv_event_get_user_data(event);
    lv_indev_t *indev = lv_indev_active();
    bool pointer = indev != NULL && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER;
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED && pointer) {
        input->pointer_moved = false;
        lv_indev_get_point(indev, &input->press_point);
    } else if (code == LV_EVENT_PRESSING && pointer) {
        pointer_motion(input, indev);
    } else if (code == LV_EVENT_PRESS_LOST && pointer) {
        input->pointer_moved = true;
    } else if (code == LV_EVENT_KEY && !input->control) {
        uint32_t key = lv_event_get_key(event);
        if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) emit(input, DIAL_INPUT_OPEN, false);
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        // Changing the arc is adjustment, even if the finger barely moves.
        if (pointer) input->pointer_moved = true;
        emit(input, DIAL_INPUT_CHANGE, pointer);
    } else if (code == LV_EVENT_LONG_PRESSED && pointer) {
        pointer_motion(input, indev);
        if (!input->pointer_moved) {
            emit(input, DIAL_INPUT_HOME, true);
            lv_indev_wait_release(indev);
        }
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        if (!pointer || !input->pointer_moved) {
            emit(input, input->control ? DIAL_INPUT_CONFIRM : DIAL_INPUT_OPEN, pointer);
        }
    } else if (code == LV_EVENT_RELEASED && input->control) {
        // LVGL's arc exits encoder editing on release; this is always a dial.
        lv_group_set_editing(input->group, true);
    }
}

static void encoder_long_press(lv_event_t *event)
{
    dial_input_t *input = lv_event_get_user_data(event);
    emit(input, DIAL_INPUT_HOME, false);
    // Stop LVGL from treating this same hold/release as an edit toggle or click.
    lv_indev_reset(input->encoder, NULL);
    lv_indev_wait_release(input->encoder);
}

void dial_input_init(dial_input_t *input, lv_indev_t *encoder, lv_obj_t *screen,
                     lv_obj_t *arc, dial_input_handler_t handler, void *context)
{
    *input = (dial_input_t){.screen = screen, .arc = arc, .encoder = encoder,
        .group = lv_group_create(), .handler = handler, .context = context};
    lv_group_add_obj(input->group, screen);
    lv_group_add_obj(input->group, arc);
    lv_indev_set_group(encoder, input->group);
    // The BSP reports edges only. Poll the held state so LVGL can detect holds.
    lv_indev_set_mode(encoder, LV_INDEV_MODE_TIMER);
    lv_indev_add_event_cb(encoder, encoder_long_press, LV_EVENT_LONG_PRESSED, input);
    lv_obj_add_event_cb(screen, object_event, LV_EVENT_ALL, input);
    lv_obj_add_event_cb(arc, object_event, LV_EVENT_ALL, input);
    dial_input_set_control(input, false);
}

void dial_input_set_control(dial_input_t *input, bool control)
{
    // A held press belongs to the old view/configuration, not its replacement.
    lv_display_t *display = lv_obj_get_display(input->screen);
    for (lv_indev_t *indev = lv_indev_get_next(NULL); indev != NULL; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_display(indev) == display &&
            lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
            lv_indev_reset(indev, NULL);
            lv_indev_wait_release(indev);
        }
    }
    input->control = control;
    lv_group_focus_obj(control ? input->arc : input->screen);
    lv_group_set_editing(input->group, true);
}
