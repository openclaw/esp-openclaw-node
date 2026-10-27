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
static unsigned refreshes, previous, next, opens, reads;
static dial_conversations_snapshot_t conversations;
static const char *snapshot_prefix;
static uint16_t frame[240 * 240];
static size_t requested_offset;
static uint16_t pixels[240 * 240];

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x)
            frame[y * 240 + x] = ((const uint16_t *)data)[(y - area->y1) * (lv_draw_buf_width_to_stride(area->x2 - area->x1 + 1, LV_COLOR_FORMAT_RGB565) / 2) + x - area->x1];
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
    if (kind == DIAL_NAV_CONVERSATIONS) { ++opens; requested_offset = offset; }
    else if (kind == DIAL_NAV_READ) { ++reads; requested_offset = offset; }
    else if (kind == DIAL_NAV_REFRESH || kind == DIAL_NAV_SESSIONS_REFRESH) { ++refreshes; requested_offset = offset; }
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

static void snapshot(const char *name)
{
    if (snapshot_prefix == NULL) return;
    char path[1024];
    assert(snprintf(path, sizeof(path), "%s-%s.ppm", snapshot_prefix, name) < (int)sizeof(path));
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    fprintf(file, "P6\n240 240\n255\n");
    for (unsigned i = 0; i < 240 * 240; ++i) {
        unsigned pixel = frame[i];
        unsigned char rgb[] = {(pixel >> 11) * 255 / 31, ((pixel >> 5) & 63) * 255 / 63, (pixel & 31) * 255 / 31};
        assert(fwrite(rgb, 1, 3, file) == 3);
    }
    assert(fclose(file) == 0);
}

int main(int argc, char **argv)
{
    snapshot_prefix = argc > 1 ? argv[1] : NULL;
    lv_init();
    lv_display_t *display = lv_display_create(240, 240);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x15101b), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0x362031), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
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

    roster.state = DIAL_AGENTS_READY;
    roster.revision = 4;
    roster.page.count = roster.page.total = 1;
    roster.page.offset = 0;
    strcpy(roster.page.agents[0].id, "beta");
    strcpy(roster.page.agents[0].name, "Beta");
    dial_navigation_update(&navigation, &roster);
    dial_navigation_open(&navigation, true);
    touch(120, 82, true); touch(120, 82, false);
    assert(navigation.view == DIAL_NAV_DETAIL);
    snapshot("agent");
    touch(120, 172, true); touch(120, 172, false);
    assert(navigation.view == DIAL_NAV_SESSIONS && opens == 1 && requested_offset == 0);
    conversations = (dial_conversations_snapshot_t){.state = DIAL_AGENTS_LOADING, .revision = 1};
    conversations.agent = roster.page.agents[0];
    dial_navigation_conversations_update(&navigation, &conversations);
    assert(strstr(lv_label_get_text(navigation.description), "Loading conversations") != NULL);
    conversations.state = DIAL_AGENTS_READY;
    conversations.page.count = 2;
    conversations.page.has_more = true;
    conversations.page.next_offset = 2;
    strcpy(conversations.page.sessions[0].title, "Planning the next adventure");
    strcpy(conversations.page.sessions[1].title, "A question for Beta");
    dial_navigation_conversations_update(&navigation, &conversations);
    snapshot("list");
    assert(strncmp(lv_label_get_text(navigation.names[0]), "Planning", 8) == 0);
    touch(120, 82, true);
    ++conversations.revision;
    dial_navigation_conversations_update(&navigation, &conversations);
    touch(120, 82, false);
    assert(navigation.view == DIAL_NAV_SESSIONS && reads == 0);
    dial_navigation_move(&navigation, 1);
    dial_navigation_activate(&navigation);
    assert(navigation.view == DIAL_NAV_PREVIEW && reads == 1 && requested_offset == 1);
    conversations.selected = 1;
    conversations.preview_state = DIAL_AGENTS_LOADING;
    ++conversations.preview_revision;
    dial_navigation_conversations_update(&navigation, &conversations);
    assert(strstr(lv_label_get_text(navigation.preview_text), "Loading") != NULL);
    conversations.preview_state = DIAL_AGENTS_READY;
    conversations.preview.status = ESP_OPENCLAW_PREVIEW_OK;
    conversations.preview.count = 2;
    conversations.preview.items[0].role = ESP_OPENCLAW_PREVIEW_USER;
    strcpy(conversations.preview.items[0].text, "What should we work on next?");
    conversations.preview.items[1].role = ESP_OPENCLAW_PREVIEW_ASSISTANT;
    strcpy(conversations.preview.items[1].text, "Start with a small working journey. Pick an agent, read a conversation, and return without losing your place. Then check how the controls behave when the network drops and reconnects.");
    dial_navigation_conversations_update(&navigation, &conversations);
    snapshot("preview");
    assert(strstr(lv_label_get_text(navigation.preview_text), "What should we work on next?") != NULL);
    dial_navigation_move(&navigation, 1);
    advance(40);
    assert(lv_obj_get_scroll_y(navigation.preview_body) > 0);
    snapshot("scrolled");
    touch(68, 44, true); touch(68, 44, false);
    assert(navigation.view == DIAL_NAV_SESSIONS && navigation.focus == 1);
    dial_navigation_activate(&navigation);
    conversations.preview.count = 0;
    conversations.preview.status = ESP_OPENCLAW_PREVIEW_MISSING;
    ++conversations.preview_revision;
    dial_navigation_conversations_update(&navigation, &conversations);
    assert(strcmp(lv_label_get_text(navigation.preview_text), "Conversation unavailable") == 0);
    snapshot("missing");
    conversations.preview_state = conversations.state = DIAL_AGENTS_OFFLINE;
    dial_navigation_conversations_update(&navigation, &conversations);
    assert(strstr(lv_label_get_text(navigation.preview_text), "Gateway offline") != NULL);
    assert(strstr(lv_label_get_text(navigation.preview_text), "What should") == NULL);
    touch(68, 44, true); touch(68, 44, false);
    touch(68, 44, true); touch(68, 44, false);
    assert(navigation.view == DIAL_NAV_DETAIL && navigation.focus == 0 && confirmations == 4);
    puts("conversation list, scrolling, missing/offline preview and return focus: passed");

    // A rejected reader command after Home must leave the active control usable.
    navigation.view = DIAL_NAV_PREVIEW;
    navigating = false;
    lv_obj_set_hidden(navigation.root, true);
    control();
    unsigned before_rejection = changes;
    conversations = (dial_conversations_snapshot_t){.revision = 99};
    assert(!dial_navigation_show_reader(&navigation, &roster, &conversations, true));
    encoder_diff = 1;
    lv_indev_read(input.encoder);
    advance(40);
    assert(changes == before_rejection + 1);
    puts("rejected reader page preserves active control input: passed");

    lv_deinit();
}
