#include "dial_input.h"

#include <assert.h>
#include <stdio.h>

static dial_input_t input;
static lv_indev_state_t encoder_state, pointer_state;
static int encoder_diff;
static lv_point_t pointer_point;
static lv_indev_t *pointer;
static unsigned confirmations, homes, changes;
static uint16_t pixels[240 * 20];

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{
    (void)area;
    (void)data;
    lv_display_flush_ready(display);
}

static void encoder_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->key = LV_KEY_ENTER;
    data->state = encoder_state;
    data->enc_diff = encoder_diff;
    encoder_diff = 0;
}

static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->state = pointer_state;
    data->point = pointer_point;
}

static void event(dial_input_event_t kind, int value, bool pointer, void *context)
{
    (void)value;
    (void)pointer;
    (void)context;
    if (kind == DIAL_INPUT_CONFIRM) ++confirmations;
    else if (kind == DIAL_INPUT_CHANGE) ++changes;
    else {
        if (kind == DIAL_INPUT_HOME) ++homes;
        bool control = kind == DIAL_INPUT_OPEN;
        lv_obj_set_flag(input.arc, LV_OBJ_FLAG_HIDDEN, !control);
        dial_input_set_control(&input, control);
    }
}

static void advance(unsigned milliseconds)
{
    for (unsigned i = 0; i < milliseconds; i += 40) {
        lv_tick_inc(40);
        lv_timer_handler();
    }
}

static void control(void)
{
    lv_obj_set_hidden(input.arc, false);
    dial_input_set_control(&input, true);
    lv_arc_set_value(input.arc, 50);
    advance(40);
}

static void touch(int x, int y, bool pressed)
{
    pointer_point = (lv_point_t){x, y};
    pointer_state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer);
    advance(40);
}

int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(240, 240);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_clickable(screen, true);
    lv_obj_t *arc = lv_arc_create(screen);
    lv_obj_set_size(arc, 218, 218);
    lv_obj_center(arc);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 50);
    lv_indev_t *encoder = lv_indev_create();
    lv_indev_set_type(encoder, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_mode(encoder, LV_INDEV_MODE_EVENT); // BSP's original mode.
    lv_indev_set_read_cb(encoder, encoder_read);
    pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, pointer_read);
    dial_input_init(&input, encoder, screen, arc, event, NULL);
    advance(40);

    // Turning must still adjust the value after confirming once.
    control();
    encoder_diff = 1;
    lv_indev_read(encoder);
    advance(40);
    assert(lv_arc_get_value(arc) == 51);
    encoder_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(encoder);
    advance(40);
    encoder_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(encoder);
    advance(40);
    assert(confirmations == 1);
    encoder_diff = 1;
    lv_indev_read(encoder);
    advance(40);
    assert(lv_arc_get_value(arc) == 52);
    puts("turn-confirm-turn: passed");

    // A stationary hold returns home and must never submit on release.
    encoder_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(encoder);
    advance(40);
    advance(1000);
    assert(!input.control && homes == 1);
    encoder_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(encoder);
    advance(40);
    assert(confirmations == 1);
    puts("stationary encoder hold: passed");

    // Returning from the face must restore a usable control.
    encoder_diff = 1;
    lv_indev_read(encoder);
    advance(40);
    assert(input.control);

    // Both short and sustained arc drags adjust only, never confirm or go home.
    control();
    unsigned before_changes = changes;
    touch(215, 120, true);
    touch(120, 25, true);
    touch(120, 25, false);
    assert(changes > before_changes && input.control);
    assert(confirmations == 1 && homes == 1);
    control();
    touch(215, 120, true);
    touch(120, 25, true);
    advance(1000);
    assert(input.control && homes == 1);
    touch(120, 25, false);
    assert(confirmations == 1);
    puts("touch drags: passed");

    // Stationary touches retain the documented tap and hold actions.
    touch(120, 120, true);
    touch(120, 120, false);
    assert(confirmations == 2);
    touch(120, 120, true);
    advance(1000);
    assert(!input.control && homes == 2);
    touch(120, 120, false);
    assert(confirmations == 2);
    puts("stationary touch tap/hold: passed");

    lv_deinit();
}
