#include "dial_input.h"
#include "dial_home.h"
#include "dial_navigation.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static dial_input_t input;
static dial_home_t home;
static lv_indev_state_t encoder_state, pointer_state;
static int encoder_diff;
static lv_point_t pointer_point;
static lv_indev_t *pointer;
static unsigned confirmations, homes, changes;
static dial_navigation_t navigation;
static dial_agents_snapshot_t roster;
static bool navigating;
static unsigned refreshes, previous, next;
static size_t requested_offset;
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
    if (kind == DIAL_INPUT_PREVIOUS) ++previous;
    if (kind == DIAL_INPUT_NEXT) ++next;
    if (navigating && kind != DIAL_INPUT_HOME) {
        if (kind == DIAL_INPUT_PREVIOUS) dial_navigation_move(&navigation, -1);
        else if (kind == DIAL_INPUT_NEXT) dial_navigation_move(&navigation, 1);
        else if (kind == DIAL_INPUT_OPEN) dial_navigation_activate(&navigation);
        return;
    }
    if (kind == DIAL_INPUT_CONFIRM) ++confirmations;
    else if (kind == DIAL_INPUT_CHANGE) ++changes;
    else {
        if (kind == DIAL_INPUT_HOME) {
            ++homes;
            navigating = false;
            if (navigation.root != NULL) lv_obj_set_hidden(navigation.root, true);
        }
        bool control = kind == DIAL_INPUT_OPEN || kind == DIAL_INPUT_PREVIOUS || kind == DIAL_INPUT_NEXT;
        lv_obj_set_flag(input.arc, LV_OBJ_FLAG_HIDDEN, !control);
        lv_obj_set_hidden(home.root, control);
        dial_input_set_control(&input, control);
    }
}

static void navigation_event(dial_navigation_event_t kind, size_t offset, void *context)
{
    (void)context;
    dial_input_set_control(&input, false);
    if (kind == DIAL_NAV_REFRESH) { ++refreshes; requested_offset = offset; }
    else if (kind == DIAL_NAV_HOME || kind == DIAL_NAV_CONTROL) {
        navigating = false;
        lv_obj_set_hidden(navigation.root, true);
        lv_obj_set_hidden(home.root, kind != DIAL_NAV_HOME);
        lv_obj_set_hidden(input.arc, kind != DIAL_NAV_CONTROL);
        dial_input_set_control(&input, kind == DIAL_NAV_CONTROL);
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
    lv_obj_set_hidden(home.root, true);
    lv_obj_set_hidden(input.arc, false);
    dial_input_set_control(&input, true);
    lv_arc_set_value(input.arc, 50);
    advance(40);
}

static void check_home_bounds(lv_obj_t *object)
{
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    const int x[] = {area.x1 - 120, area.x2 - 120};
    const int y[] = {area.y1 - 120, area.y2 - 120};
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            assert(x[i] * x[i] + y[j] * y[j] <= 112 * 112);
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        check_home_bounds(lv_obj_get_child(object, i));
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
    dial_home_create(&home, screen);
    dial_home_render(&home, true, true, true, NULL);
    lv_obj_update_layout(screen);
    for (unsigned i = 0; i < lv_obj_get_child_count(home.root); ++i)
        check_home_bounds(lv_obj_get_child(home.root, i));
    assert(strcmp(lv_label_get_text(home.action), "Open menu") == 0);
    dial_home_render(&home, true, false, false, NULL);
    assert(strcmp(lv_label_get_text(home.link), "Offline") == 0);
    assert(strcmp(lv_label_get_text(home.headline), "Gateway offline") == 0);
    dial_home_render(&home, true, true, false, NULL);
    assert(strcmp(lv_label_get_text(home.headline), "No action assigned") == 0);
    assert(strcmp(lv_label_get_text(home.action), "Open menu") == 0);
    char activity[] = "Action failed; press to retry";
    dial_home_render(&home, true, true, true, activity);
    memset(activity, 'x', strlen(activity));
    assert(strcmp(lv_label_get_text(home.headline), "Action failed; press to retry") == 0);
    lv_obj_update_layout(screen);
    check_home_bounds(home.headline);
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

    // A remotely shown/reconfigured control needs a fresh press to confirm.
    for (unsigned i = 0; i < 2; ++i) {
        encoder_state = LV_INDEV_STATE_PRESSED;
        lv_indev_read(encoder);
        advance(40);
        control();
        encoder_state = LV_INDEV_STATE_RELEASED;
        lv_indev_read(encoder);
        advance(40);
        assert(confirmations == 2);
    }
    encoder_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(encoder);
    advance(40);
    encoder_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(encoder);
    advance(40);
    assert(confirmations == 3);
    puts("control changes cancel held encoder presses: passed");

    // A remote view change also consumes an in-progress touch gesture.
    touch(120, 120, true);
    lv_obj_set_hidden(input.arc, true);
    lv_obj_set_hidden(home.root, false);
    dial_input_set_control(&input, false);
    touch(120, 120, false);
    assert(!input.control && confirmations == 3);
    touch(120, 120, true);
    touch(120, 120, false);
    assert(input.control && confirmations == 3);
    touch(120, 120, true);
    control();
    touch(120, 120, false);
    assert(confirmations == 3);
    touch(120, 120, true);
    touch(120, 120, false);
    assert(confirmations == 4);
    puts("view changes cancel held touches: passed");

    // The same real encoder now moves focus in both directions, and a press
    // opens the focused row without sending a numeric action.
    dial_navigation_create(&navigation, screen, navigation_event, NULL);
    navigating = true;
    lv_obj_set_hidden(arc, true);
    lv_obj_set_hidden(home.root, true);
    dial_navigation_open(&navigation, false);
    advance(40);
    encoder_diff = 1;
    lv_indev_read(encoder);
    advance(40);
    assert(next > 0 && navigation.focus == 1);
    encoder_diff = -1;
    lv_indev_read(encoder);
    advance(40);
    assert(previous > 0 && navigation.focus == 0);
    touch(120, 82, true);
    touch(150, 95, true);
    touch(150, 95, false);
    assert(navigation.view == DIAL_NAV_MENU && refreshes == 0);
    touch(120, 82, true);
    touch(120, 82, false);
    assert(navigation.view == DIAL_NAV_AGENTS && refreshes == 1);
    roster = (dial_agents_snapshot_t){.state = DIAL_AGENTS_READY, .revision = 1,
        .page = {.count = 8, .total = 10}};
    for (unsigned i = 0; i < 8; ++i) {
        snprintf(roster.page.agents[i].id, 65, "agent-%u", i);
        snprintf(roster.page.agents[i].name, 65, "Assistant %u", i);
    }
    strcpy(roster.page.agents[0].name, "A very long assistant name that needs truncation");
    strcpy(roster.page.agents[0].emoji, "🦊");
    dial_navigation_update(&navigation, &roster);
    advance(40);
    for (unsigned i = 0; i < lv_obj_get_child_count(navigation.root); ++i)
        check_home_bounds(lv_obj_get_child(navigation.root, i));
    touch(120, 82, true);
    touch(120, 82, false);
    assert(navigation.view == DIAL_NAV_DETAIL);
    assert(strcmp(lv_label_get_text(navigation.identity), "agent-0") == 0);
    touch(68, 44, true);
    touch(68, 44, false);
    assert(navigation.view == DIAL_NAV_AGENTS && navigation.focus == 0);

    // Both row and Back presses belong to the rendered revision. The shared
    // input cancellation must consume their release after the roster changes.
    touch(120, 82, true);
    ++roster.revision;
    strcpy(roster.page.agents[0].id, "replacement");
    dial_navigation_update(&navigation, &roster);
    touch(120, 82, false);
    assert(navigation.view == DIAL_NAV_AGENTS);
    touch(68, 44, true);
    ++roster.revision;
    dial_navigation_update(&navigation, &roster);
    touch(68, 44, false);
    assert(navigation.view == DIAL_NAV_AGENTS);

    encoder_diff = 8;
    lv_indev_read(encoder);
    advance(40);
    assert(navigation.focus == 8);
    encoder_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(encoder);
    advance(40);
    encoder_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(encoder);
    advance(40);
    assert(refreshes == 2 && requested_offset == 8);

    // A replacement/offline roster invalidates an in-progress press. Releasing
    // it cannot open the new first row or send a stale control action.
    encoder_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(encoder);
    advance(40);
    roster.state = DIAL_AGENTS_OFFLINE;
    ++roster.revision;
    dial_navigation_update(&navigation, &roster);
    encoder_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(encoder);
    advance(40);
    assert(refreshes == 2 && navigation.view == DIAL_NAV_AGENTS);
    assert(strstr(lv_label_get_text(navigation.description), "Gateway offline") != NULL);
    for (unsigned i = 0; i < lv_obj_get_child_count(navigation.root); ++i)
        check_home_bounds(lv_obj_get_child(navigation.root, i));
    encoder_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(encoder);
    advance(1000);
    assert(!navigating && !input.control);
    encoder_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(encoder);
    advance(40);
    assert(confirmations == 4);
    puts("agent navigation, paging, bounds and stale presses: passed");

    // An empty end page still exposes Previous, Refresh and Back on the two
    // available rows; it must not strand the encoder on a hidden third row.
    navigating = true;
    lv_obj_set_hidden(home.root, true);
    dial_navigation_open(&navigation, true);
    roster = (dial_agents_snapshot_t){.state = DIAL_AGENTS_EMPTY, .revision = 3,
        .page = {.offset = 8, .total = 8}};
    dial_navigation_update(&navigation, &roster);
    dial_navigation_move(&navigation, 1);
    dial_navigation_move(&navigation, 1);
    assert(strcmp(lv_label_get_text(navigation.names[0]), "Back") == 0);
    assert(!lv_obj_has_flag(navigation.rows[0], LV_OBJ_FLAG_HIDDEN));
    dial_navigation_activate(&navigation);
    assert(navigation.view == DIAL_NAV_MENU);
    puts("empty-page navigation: passed");

    lv_deinit();
}
