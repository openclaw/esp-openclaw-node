#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "room_board.h"
#include "room_canvas.h"
#include "room_diagnostics.h"
#include "room_face.h"
#include "room_lobster.h"
#include "room_ui_controller.h"

static lv_display_t *display;
static uint8_t *frame, *draw_buffer;
static int width, height, brightness, display_depth, critical_depth;
static bool deny_display, canvas_active;
static room_canvas_retained_kind_t retained_kind;
static esp_openclaw_node_command_handler_t face_gesture;
static unsigned toggles, holds;
static unsigned talk_starts, talk_stops, display_flushes;
static lv_indev_t *touchscreen;
static lv_indev_state_t touch_state;
static lv_point_t touch_point;
static lv_obj_t *modal;
static lv_obj_t *watch_menu_button;
static room_canvas_action_handler_t canvas_action;
struct ui_timer { esp_timer_create_args_t args; bool active; };
static struct ui_timer timers[4];
static unsigned timer_count;

void ui_enter_critical(portMUX_TYPE *lock) { (void)lock; ++critical_depth; }
void ui_exit_critical(portMUX_TYPE *lock) { (void)lock; assert(critical_depth-- > 0); }
size_t strlcpy(char *dest, const char *source, size_t capacity)
{
    size_t length = strlen(source);
    if (capacity) {
        size_t copy = length < capacity - 1 ? length : capacity - 1;
        memcpy(dest, source, copy);
        dest[copy] = '\0';
    }
    return length;
}
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *timer)
{
    assert(timer_count < sizeof(timers) / sizeof(*timers));
    *timer = &timers[timer_count++];
    (*timer)->args = *args;
    return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t timer) { timer->active = false; return ESP_OK; }
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t us)
{ assert(us > 0); timer->active = true; return ESP_OK; }
int64_t esp_timer_get_time(void) { return 1000000; }

static void fire_timer(const char *name)
{
    for (unsigned i = 0; i < timer_count; ++i) {
        if (timers[i].active && strcmp(timers[i].args.name, name) == 0) {
            timers[i].active = false;
            timers[i].args.callback(timers[i].args.arg);
            return;
        }
    }
    assert(!"expected active timer");
}

esp_err_t esp_openclaw_node_register_command(esp_openclaw_node_handle_t node,
                                            const esp_openclaw_node_command_t *command)
{
    assert(node && command);
    if (strcmp(command->name, "face.gesture") == 0) face_gesture = command->handler;
    return ESP_OK;
}
void room_canvas_set_action_handler(room_canvas_action_handler_t handler) { canvas_action = handler; }
bool room_canvas_is_active(void) { return canvas_active; }
void room_canvas_get_diagnostics(room_canvas_diagnostics_snapshot_t *snapshot)
{
    *snapshot = (room_canvas_diagnostics_snapshot_t){.active = canvas_active, .retained_kind = retained_kind};
}
esp_err_t room_canvas_hide(char **payload, esp_openclaw_node_error_t *error)
{
    (void)error;
    canvas_active = false;
    *payload = NULL;
    canvas_action(ROOM_CANVAS_ACTION_RENDER_CHANGED, 0);
    return ESP_OK;
}
void room_canvas_view_toggle(void)
{
    ++toggles;
    if (retained_kind != ROOM_CANVAS_RETAINED_NONE) {
        canvas_active = !canvas_active;
        canvas_action(ROOM_CANVAS_ACTION_RENDER_CHANGED, 0);
    } else canvas_action(ROOM_CANVAS_ACTION_REQUEST_FACE_HINT, 10000);
}
bool room_diagnostics_is_open(void) { return modal != NULL; }
esp_err_t room_diagnostics_open(void)
{
    ++holds;
    modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(modal, width, height);
    room_ui_set_diagnostics_open(true);
    return ESP_OK;
}
esp_err_t room_diagnostics_close(void)
{
    lv_obj_delete(modal);
    modal = NULL;
    room_ui_set_diagnostics_open(false);
    return ESP_OK;
}

static void flush(lv_display_t *target, const lv_area_t *area, uint8_t *pixels)
{
    ++display_flushes;
    int row_bytes = lv_area_get_width(area) * 4;
    for (int y = area->y1; y <= area->y2; ++y) {
        memcpy(frame + ((size_t)y * width + area->x1) * 4,
            pixels + (size_t)(y - area->y1) * row_bytes, row_bytes);
    }
    lv_display_flush_ready(target);
}
static lv_display_t *start_display(void *ctx) { (void)ctx; return display; }
static bool lock_display(void *ctx, uint32_t ms)
{
    (void)ctx; (void)ms;
    assert(critical_depth == 0);
    if (deny_display) return false;
    ++display_depth;
    return true;
}
static void unlock_display(void *ctx) { (void)ctx; assert(display_depth-- > 0); }
static esp_err_t set_brightness(void *ctx, int value)
{
    (void)ctx;
    assert(critical_depth == 0 && value >= 0 && value <= 100);
    brightness = value;
    return ESP_OK;
}
static esp_err_t open_audio(void *ctx, esp_openclaw_room_audio_handles_t *handles)
{ (void)ctx; (void)handles; return ESP_OK; }

static lv_obj_t *find_text(lv_obj_t *parent, const char *text)
{
    if (lv_obj_check_type(parent, &lv_label_class) &&
        strcmp(lv_label_get_text(parent), text) == 0) return parent;
    for (unsigned i = 0; i < lv_obj_get_child_count(parent); ++i) {
        lv_obj_t *found = find_text(lv_obj_get_child(parent, i), text);
        if (found) return found;
    }
    return NULL;
}
static bool visible_text(const char *text)
{
    lv_obj_update_layout(lv_screen_active());
    lv_obj_t *label = find_text(lv_screen_active(), text);
    return label && lv_obj_is_visible(label);
}
static void talk_action(bool start)
{
    if (start) ++talk_starts;
    else ++talk_stops;
}

static void read_touch(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->state = touch_state;
    data->point = touch_point;
}

static void touch_talk(lv_obj_t *button, bool pressed)
{
    if (touchscreen == NULL) {
        touchscreen = lv_indev_create();
        lv_indev_set_type(touchscreen, LV_INDEV_TYPE_POINTER);
        lv_indev_set_mode(touchscreen, LV_INDEV_MODE_EVENT);
        lv_indev_set_read_cb(touchscreen, read_touch);
    }
    lv_obj_update_layout(lv_layer_top());
    lv_area_t bounds;
    lv_obj_get_coords(button, &bounds);
    touch_point = (lv_point_t){(bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2};
    touch_state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    lv_indev_read(touchscreen);
    lv_tick_inc(40);
}

static void click_talk(lv_obj_t *button)
{
    touch_talk(button, true);
    touch_talk(button, false);
}

static void check_talk_control(void)
{
    room_ui_set(ROOM_UI_LISTENING, NULL);
    room_ui_facts_t facts = {.talk = ROOM_UI_TALK_READY};
    room_ui_store_facts(&facts);
    room_ui_refresh();
    lv_obj_update_layout(lv_layer_top());
    lv_obj_t *label = find_text(lv_layer_top(), "Start Talk");
    assert(label && lv_obj_is_visible(label));
    lv_obj_t *button = lv_obj_get_parent(label);
    lv_obj_update_layout(lv_layer_top());
    lv_area_t bounds;
    lv_obj_get_coords(button, &bounds);
    assert(bounds.x1 >= 0 && bounds.y1 >= 0 && bounds.x2 < width && bounds.y2 < height);
    unsigned old_toggles = toggles, old_holds = holds;
    click_talk(button);
    assert(talk_starts == 1 && talk_stops == 0);
    assert(toggles == old_toggles && holds == old_holds);
    for (int active = ROOM_UI_TALK_CONNECTING; active <= ROOM_UI_TALK_ACTIVE; ++active) {
        facts.talk = active;
        room_ui_store_facts(&facts);
        room_ui_refresh();
        assert(find_text(button, "Stop Talk"));
        click_talk(button);
    }
    assert(talk_stops == 2);
    facts.talk = ROOM_UI_TALK_STOPPING;
    room_ui_store_facts(&facts);
    room_ui_refresh();
    assert(lv_obj_has_state(button, LV_STATE_DISABLED));
    lv_obj_send_event(button, LV_EVENT_SHORT_CLICKED, NULL);
    assert(talk_stops == 2);
    facts.talk = ROOM_UI_TALK_READY;
    room_ui_store_facts(&facts);
    canvas_active = true;
    room_ui_refresh();
    assert(lv_obj_is_visible(button));
    room_diagnostics_open();
    assert(!lv_obj_is_visible(button));
    lv_obj_send_event(button, LV_EVENT_SHORT_CLICKED, NULL);
    assert(talk_starts == 1);
    room_diagnostics_close();
    canvas_active = false;
    facts.talk = ROOM_UI_TALK_UNAVAILABLE;
    room_ui_store_facts(&facts);
    room_ui_refresh();
    assert(lv_obj_has_state(button, LV_STATE_DISABLED));
    lv_obj_send_event(button, LV_EVENT_SHORT_CLICKED, NULL);
    assert(talk_starts == 1 && talk_stops == 2);

    // A held Start press cannot become Stop when another source starts Talk.
    facts.talk = ROOM_UI_TALK_READY;
    room_ui_store_facts(&facts);
    room_ui_refresh();
    touch_talk(button, true);
    facts.talk = ROOM_UI_TALK_CONNECTING;
    room_ui_store_facts(&facts);
    room_ui_refresh();
    touch_talk(button, false);
    assert(talk_starts == 1 && talk_stops == 2);

    // Ending the call cannot turn a held Stop press into a new Start either.
    touch_talk(button, true);
    facts.talk = ROOM_UI_TALK_READY;
    room_ui_store_facts(&facts);
    room_ui_refresh();
    touch_talk(button, false);
    assert(talk_starts == 1 && talk_stops == 2);

    // Connection completion keeps the same Stop intent valid.
    facts.talk = ROOM_UI_TALK_CONNECTING;
    room_ui_store_facts(&facts);
    room_ui_refresh();
    touch_talk(button, true);
    facts.talk = ROOM_UI_TALK_ACTIVE;
    room_ui_store_facts(&facts);
    room_ui_refresh();
    touch_talk(button, false);
    assert(talk_starts == 1 && talk_stops == 3);

    // Reverting to the original label does not revive an invalidated gesture.
    touch_talk(button, true);
    facts.talk = ROOM_UI_TALK_READY;
    room_ui_store_facts(&facts);
    facts.talk = ROOM_UI_TALK_ACTIVE;
    room_ui_store_facts(&facts);
    touch_talk(button, false);
    assert(talk_starts == 1 && talk_stops == 3);
    click_talk(button);
    assert(talk_starts == 1 && talk_stops == 4);

    // A stale painting cannot lend its press to a newly available action.
    facts.talk = ROOM_UI_TALK_READY;
    room_ui_store_facts(&facts);
    touch_talk(button, true);
    room_ui_refresh();
    touch_talk(button, false);
    assert(talk_starts == 1 && talk_stops == 4);
    click_talk(button);
    assert(talk_starts == 2 && talk_stops == 4);

    touch_talk(button, true);
    room_diagnostics_open();
    room_diagnostics_close();
    touch_talk(button, false);
    assert(talk_starts == 2 && talk_stops == 4);

    facts = (room_ui_facts_t){0};
    room_ui_store_facts(&facts);
    room_ui_set(ROOM_UI_IDLE, NULL);
}

static void check_dark_talk_control(void)
{
    if (room_board_config()->display.idle_brightness != 0) return;
    room_ui_facts_t facts = {.talk = ROOM_UI_TALK_READY};
    room_ui_store_facts(&facts);
    room_ui_refresh();
    lv_obj_t *button = lv_obj_get_parent(find_text(lv_layer_top(), "Start Talk"));
    unsigned starts = talk_starts;
    room_canvas_retained_kind_t previous_kind = retained_kind;
    retained_kind = ROOM_CANVAS_RETAINED_IMAGE;
    assert(brightness == 0);
    click_talk(button);
    assert(talk_starts == starts && !canvas_active && brightness > 0);
    click_talk(button);
    assert(talk_starts == starts + 1);

    room_ui_set(ROOM_UI_IDLE, NULL);
    if (brightness != 0) fire_timer("text_hint");
    assert(brightness == 0);
    touch_talk(button, true);
    room_ui_show_face_hint(10000);
    touch_talk(button, false);
    assert(talk_starts == starts + 1 && !canvas_active);

    facts = (room_ui_facts_t){0};
    room_ui_store_facts(&facts);
    room_ui_set(ROOM_UI_IDLE, NULL);
    if (brightness != 0) fire_timer("text_hint");
    retained_kind = previous_kind;
}
static unsigned check_home_tree(lv_obj_t *parent)
{
    unsigned count = 1;
    assert(!lv_obj_has_flag(parent, LV_OBJ_FLAG_SCROLLABLE));
    if (parent != watch_menu_button) assert(!lv_obj_has_flag(parent, LV_OBJ_FLAG_CLICKABLE));
    lv_area_t area;
    lv_obj_get_coords(parent, &area);
    assert(area.x1 >= 0 && area.y1 >= 0 && area.x2 < width && area.y2 < height);
    for (unsigned i = 0; i < lv_obj_get_child_count(parent); ++i)
        count += check_home_tree(lv_obj_get_child(parent, i));
    return count;
}
static void snapshot(const char *path)
{
    lv_refr_now(display);
    unsigned red = 0, light = 0;
    for (int i = 0; i < width * height; ++i) {
        uint8_t *p = frame + i * 4;
        red += p[2] > 140 && p[2] > p[1] * 2;
        light += p[0] > 150 && p[1] > 150 && p[2] > 150;
    }
    assert(red > 2000 && light > 500);
    if (!path) return;
    FILE *out = fopen(path, "wb");
    assert(out);
    fprintf(out, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; ++i) {
        uint8_t rgb[] = {frame[i * 4 + 2], frame[i * 4 + 1], frame[i * 4]};
        assert(fwrite(rgb, sizeof(rgb), 1, out) == 1);
    }
    assert(fclose(out) == 0);
}

static void check_watch_repaint(void)
{
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, width, height);
    assert(room_lobster_create(parent) == ESP_OK);
    room_lobster_pose_t pose = {
        .eye_open_pct = 100, .eye_width_pct = 100, .smile = 24,
        .voice_active = true, .attentive = true,
    };
    room_lobster_update(&pose);
    lv_refr_now(display);
    unsigned previous = display_flushes;
    pose.time_ms = 500;
    room_lobster_update(&pose);
    lv_refr_now(display);
    assert(display_flushes == previous);

    pose.eye_open_pct = 20;
    room_lobster_update(&pose);
    lv_refr_now(display);
    assert(display_flushes > previous);
    previous = display_flushes;
    pose.voice_active = false;
    room_lobster_update(&pose);
    lv_refr_now(display);
    assert(display_flushes > previous);
    lv_obj_delete(parent);
}

static void check_watch_home(const char *path)
{
    room_ui_facts_t facts = {.wifi = ROOM_UI_WIFI_CONNECTED,
        .gateway = ROOM_UI_GATEWAY_CONNECTED, .talk = ROOM_UI_TALK_READY};
    room_ui_store_facts(&facts);
    room_ui_set(ROOM_UI_IDLE, NULL);
    lv_obj_t *title = find_text(lv_layer_top(), "Ready when you are");
    watch_menu_button = lv_obj_get_parent(find_text(lv_layer_top(), LV_SYMBOL_BARS "  Menu"));
    assert(title && lv_obj_is_visible(title));
    assert(room_face_is_visible());
    lv_obj_update_layout(lv_layer_top());
    unsigned objects = check_home_tree(lv_obj_get_parent(title));
    lv_area_t title_area, action_area;
    lv_obj_get_coords(title, &title_area);
    lv_obj_t *button = lv_obj_get_parent(find_text(lv_layer_top(), "Start Talk"));
    lv_obj_get_coords(button, &action_area);
    assert(title_area.y1 >= 312 && title_area.y2 < action_area.y1);
    assert(action_area.x1 >= 24 && action_area.x2 < width - 24);
    assert(action_area.y1 >= 376 && action_area.y2 < height - 16);
    snapshot(path);

    facts.talk = ROOM_UI_TALK_ACTIVE;
    room_ui_store_facts(&facts);
    room_ui_set(ROOM_UI_SPEAKING, NULL);
    assert(find_text(lv_layer_top(), "Conversation live"));
    assert(!find_text(lv_layer_top(), "Speaking"));
    canvas_active = true;
    room_ui_refresh();
    assert(!lv_obj_is_visible(title));
    lv_obj_update_layout(lv_layer_top());
    lv_obj_get_coords(button, &action_area);
    assert(action_area.y1 == 14 && lv_area_get_height(&action_area) == 44);
    canvas_active = false;
    room_ui_refresh();
    assert(lv_obj_is_visible(title));
    lv_obj_update_layout(lv_layer_top());
    lv_obj_get_coords(button, &action_area);
    assert(action_area.y1 == 376);
    room_diagnostics_open();
    assert(!lv_obj_is_visible(title));
    room_diagnostics_close();
    assert(lv_obj_is_visible(title));

    facts.gateway = ROOM_UI_GATEWAY_NO_SESSION;
    facts.talk = ROOM_UI_TALK_NO_SESSION;
    room_ui_store_facts(&facts);
    room_ui_set(ROOM_UI_SETUP, "USB console: gateway setup-code");
    assert(find_text(lv_layer_top(), "Pairing needed"));
    assert(room_face_is_visible());
    room_ui_set(ROOM_UI_SETUP, "USB console:\nwifi set + setup-code");
    lv_obj_t *setup = find_text(lv_layer_top(), "USB console: wifi set + setup-code");
    assert(setup);
    lv_obj_update_layout(lv_layer_top());
    lv_area_t setup_area;
    lv_obj_get_coords(setup, &setup_area);
    assert(setup_area.y2 < action_area.y1);
    char detail[] = "A long error that must fit within the watch";
    room_ui_set(ROOM_UI_ERROR, detail);
    assert(room_face_is_visible());
    memset(detail, 'x', strlen(detail));
    assert(find_text(lv_layer_top(), "A long error that must fit within the watch"));
    for (int i = 0; i < 100; ++i) room_ui_refresh();
    assert(check_home_tree(lv_obj_get_parent(title)) == objects);
    room_ui_set(ROOM_UI_IDLE, NULL);
}

static void check_watch_navigation(void)
{
    room_ui_facts_t facts = {.wifi = ROOM_UI_WIFI_CONNECTED,
        .gateway = ROOM_UI_GATEWAY_CONNECTED, .talk = ROOM_UI_TALK_READY};
    room_ui_store_facts(&facts);
    room_ui_set(ROOM_UI_IDLE, NULL);
    assert(brightness == 0);
    click_talk(watch_menu_button);
    assert(!canvas_active && brightness > 0);
    assert(!lv_obj_is_visible(find_text(lv_layer_top(), "Menu")));
    click_talk(watch_menu_button);
    assert(lv_obj_is_visible(find_text(lv_layer_top(), "Menu")));
    assert(!room_face_is_visible() && brightness == ROOM_CANVAS_ACTIVE_BRIGHTNESS);
    room_ui_show_face_hint(1000);
    assert(!room_face_is_visible() && brightness == ROOM_CANVAS_ACTIVE_BRIGHTNESS);
    assert(!canvas_active);
    lv_obj_t *connection = lv_obj_get_parent(find_text(lv_layer_top(), LV_SYMBOL_WIFI "  Connection"));
    click_talk(connection);
    assert(lv_obj_is_visible(find_text(lv_layer_top(), "Wi-Fi: Connected")));
    facts.talk = ROOM_UI_TALK_ACTIVE;
    room_ui_store_facts(&facts);
    room_ui_set(ROOM_UI_SPEAKING, NULL);
    assert(lv_obj_is_visible(find_text(lv_layer_top(), "Talk: Active")));
    lv_obj_t *stop = lv_obj_get_parent(find_text(lv_layer_top(), "Stop Talk"));
    assert(lv_obj_is_visible(stop) && !room_face_is_visible());
    unsigned stopped = talk_stops;
    click_talk(stop);
    assert(talk_stops == stopped + 1);
    lv_obj_t *home_button = lv_obj_get_parent(find_text(lv_layer_top(), LV_SYMBOL_LEFT " Home"));
    click_talk(home_button);
    assert(lv_obj_is_visible(find_text(lv_layer_top(), "Conversation live")));
    assert(room_face_is_visible());
    click_talk(watch_menu_button);
    click_talk(lv_obj_get_parent(find_text(lv_layer_top(), LV_SYMBOL_SETTINGS "  Device status")));
    assert(room_diagnostics_is_open());
    room_diagnostics_close();
    assert(lv_obj_is_visible(find_text(lv_layer_top(), "Menu")));
    click_talk(lv_obj_get_parent(find_text(lv_layer_top(), LV_SYMBOL_IMAGE "  Canvas")));
    assert(lv_obj_is_visible(find_text(lv_layer_top(), "No card yet\n\nAsk your assistant to show\na card on this watch.")));
    click_talk(home_button);
    facts.talk = ROOM_UI_TALK_READY;
    room_ui_store_facts(&facts);
    room_ui_set(ROOM_UI_IDLE, NULL);
    assert(room_ui_watch_menu("invalid") == ESP_ERR_INVALID_ARG);
    assert(lv_obj_is_visible(find_text(lv_layer_top(), "Ready when you are")));
    assert(room_ui_watch_menu("connection") == ESP_OK);
    assert(lv_obj_is_visible(find_text(lv_layer_top(), "Wi-Fi: Connected")));
    assert(room_ui_watch_menu("home") == ESP_OK);
    canvas_active = true;
    room_ui_refresh();
    assert(room_ui_watch_menu("menu") == ESP_OK);
    assert(!canvas_active && lv_obj_is_visible(find_text(lv_layer_top(), "Menu")));
    retained_kind = ROOM_CANVAS_RETAINED_IMAGE;
    brightness = 0;
    assert(room_ui_watch_menu("canvas") == ESP_OK && canvas_active);
    unsigned turns = toggles;
    assert(room_ui_watch_menu("canvas") == ESP_OK && canvas_active && toggles == turns);
    assert(room_ui_watch_menu("home") == ESP_OK);
    brightness = 0;
    click_talk(watch_menu_button);
    assert(!canvas_active && brightness > 0);
    assert(lv_obj_is_visible(find_text(lv_layer_top(), "Ready when you are")));
    retained_kind = ROOM_CANVAS_RETAINED_NONE;
    assert(!canvas_active);
    room_ui_set(ROOM_UI_IDLE, NULL);
    puts("watch menu, live connection, Talk and empty Canvas: passed");
}

static void check_watch_home_gesture(void)
{
    esp_openclaw_node_handle_t node = (esp_openclaw_node_handle_t)(uintptr_t)1;
    assert(room_face_register_node_commands(node) == ESP_OK);
    assert(face_gesture && brightness == 0);
    const char params[] = "{\"name\":\"nod\"}";
    char *payload = NULL;
    esp_openclaw_node_error_t error = {0};
    assert(face_gesture(node, NULL, params, sizeof(params) - 1, &payload, &error) == ESP_OK);
    assert(brightness == 40);
    assert(payload && strstr(payload, "\"played\":true"));
    free(payload);
    room_ui_set(ROOM_UI_IDLE, NULL);
}

int main(int argc, char **argv)
{
    assert(argc >= 2);
    bool watch = strcmp(argv[1], "watch") == 0;
    bool animated = watch || strcmp(argv[1], "animated") == 0;
    int idle = strcmp(argv[1], "tab5") == 0 ? 18 : 0;
    width = watch ? 368 : animated ? 410 : 1280;
    height = watch ? 448 : animated ? 502 : 720;
    lv_init();
    display = lv_display_create(width, height);
    frame = calloc((size_t)width * height, 4);
    draw_buffer = calloc((size_t)width * height, 4);
    assert(frame && draw_buffer);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(display, draw_buffer, NULL, (size_t)width * height * 4, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    esp_openclaw_room_node_config_t board = {
        .display_name = "OpenClaw M5Stack Tab5 Room Node",
        .model_identifier = watch ? "waveshare-esp32-s3-touch-amoled-1.8" : "m5stack-tab5",
        .display = {.start = start_display, .lock = lock_display, .unlock = unlock_display,
            .set_brightness = set_brightness, .native_width = width, .native_height = height,
            .safe_inset = 24, .animated_face = animated, .idle_brightness = 101},
        .audio = {.open = open_audio, .afe_layout = "MR", .record_channels = 4},
    };
    assert(room_board_bind(&board) == ESP_ERR_INVALID_ARG);
    board.display.idle_brightness = idle;
    assert(room_board_bind(&board) == ESP_OK);
    room_ui_set_talk_action_handler(talk_action);
    room_ui_init();
    assert(brightness == idle);
    check_talk_control();
    check_dark_talk_control();
    if (animated) {
        assert(!find_text(lv_screen_active(), "OpenClaw Room Node"));
        room_ui_set(ROOM_UI_LISTENING, NULL);
        assert(room_face_is_visible() && brightness == 40);
        room_ui_set(ROOM_UI_IDLE, NULL);
        assert(brightness == 0);
    } else {
        assert(visible_text("OpenClaw Room Node"));
        lv_obj_t *home = lv_obj_get_parent(find_text(lv_screen_active(), "OpenClaw Room Node"));
        lv_obj_update_layout(lv_screen_active());
        unsigned objects = check_home_tree(home);
        room_ui_facts_t facts = {
            .wifi = ROOM_UI_WIFI_OFFLINE, .gateway = ROOM_UI_GATEWAY_NO_SESSION,
            .talk = ROOM_UI_TALK_WAITING,
        };
        room_ui_store_facts(&facts);
        room_ui_set(ROOM_UI_CONNECTING, "Wi-Fi");
        assert(visible_text("Gateway  Pairing required") && visible_text("Talk  Waiting for operator"));
        assert(visible_text("OpenClaw Room Node"));
        deny_display = true;
        facts.wifi = ROOM_UI_WIFI_CONNECTED;
        facts.gateway = ROOM_UI_GATEWAY_CONNECTED;
        facts.talk = ROOM_UI_TALK_READY;
        room_ui_store_facts(&facts);
        room_ui_refresh();
        assert(visible_text("Wi-Fi  Offline"));
        facts.wifi = ROOM_UI_WIFI_OFFLINE;
        room_ui_store_facts(&facts);
        room_ui_refresh();
        deny_display = false;
        fire_timer("ui_repaint");
        assert(visible_text("Wi-Fi  Offline") && visible_text("Gateway  Connected") && visible_text("Talk  Ready"));
        room_ui_set(ROOM_UI_IDLE, NULL);
        for (int i = 0; i < 100; ++i) room_ui_refresh();
        assert(check_home_tree(home) == objects && lv_anim_count_running() == 0);
        snapshot(argc == 3 ? argv[2] : NULL);

        const char *setup_details[] = {
            "USB console:\ngateway setup-code",
            "USB console:\nwifi set + setup-code",
        };
        for (size_t i = 0; i < sizeof(setup_details) / sizeof(*setup_details); ++i) {
            room_ui_set(ROOM_UI_SETUP, setup_details[i]);
            assert(visible_text(setup_details[i]) && visible_text("OpenClaw Room Node"));
            assert(visible_text("Gateway  Connected") && visible_text("Talk  Ready"));
            assert(check_home_tree(home) == objects);
            lv_area_t detail_area, talk_area;
            lv_obj_get_coords(find_text(home, setup_details[i]), &detail_area);
            lv_obj_get_coords(find_text(home, "Talk  Ready"), &talk_area);
            assert(detail_area.y1 > talk_area.y2 && detail_area.y2 < height);
            room_diagnostics_open();
            assert(!visible_text(setup_details[i]));
            room_diagnostics_close();
            assert(visible_text(setup_details[i]));
            room_ui_set(ROOM_UI_IDLE, NULL);
            assert(!visible_text(setup_details[i]) && brightness == idle);
        }
        room_ui_set(ROOM_UI_ERROR, "Talk setup failed");
        room_ui_store_facts(&facts);
        room_ui_refresh();
        assert(visible_text("Talk setup failed") && visible_text("Talk  Ready"));
        assert(check_home_tree(home) == objects);
        lv_area_t error_area, talk_area;
        lv_obj_get_coords(find_text(home, "Talk setup failed"), &error_area);
        lv_obj_get_coords(find_text(home, "Talk  Ready"), &talk_area);
        assert(error_area.y1 > talk_area.y2);
        if (argc == 3) {
            char error_path[1024];
            assert(snprintf(error_path, sizeof(error_path), "%s.error.ppm", argv[2]) < (int)sizeof(error_path));
            snapshot(error_path);
        }
        unsigned old_toggles = toggles;
        lv_obj_send_event(lv_screen_active(), LV_EVENT_CLICKED, NULL);
        assert(toggles == old_toggles + 1);
        unsigned holds_before = holds;
        lv_obj_send_event(lv_screen_active(), LV_EVENT_LONG_PRESSED, NULL);
        assert(holds == holds_before + 1 && !visible_text("OpenClaw Room Node") && brightness == ROOM_CANVAS_ACTIVE_BRIGHTNESS);
        assert(!visible_text("Talk setup failed"));
        room_diagnostics_close();
        assert(visible_text("Talk setup failed"));
        room_ui_set(ROOM_UI_IDLE, NULL);
        fire_timer("text_hint");
        assert(!visible_text("Talk setup failed"));
        assert(visible_text("OpenClaw Room Node") && brightness == idle);

        assert(room_ui_camera_indicator_begin() == ESP_OK);
        room_ui_refresh();
        assert(brightness >= 40);
        canvas_active = true;
        room_ui_set(ROOM_UI_SPEAKING, NULL);
        assert(!visible_text("OpenClaw Room Node") && brightness == ROOM_CANVAS_ACTIVE_BRIGHTNESS);
        lv_obj_t *camera = find_text(lv_layer_top(), LV_SYMBOL_EYE_OPEN " Camera active");
        assert(camera && lv_obj_get_child(lv_layer_top(), -1) == camera);
        canvas_action(ROOM_CANVAS_ACTION_RENDER_CHANGED, 0);
        room_diagnostics_open();
        assert(lv_obj_get_child(lv_layer_top(), -1) == camera);
        room_diagnostics_close();
        canvas_active = false;
        room_ui_set(ROOM_UI_IDLE, NULL);
        assert(visible_text("OpenClaw Room Node") && brightness >= 40);
        room_ui_camera_indicator_end();
        assert(brightness == idle);
        room_ui_show_face_hint(1000);
        assert(brightness == 18);
        fire_timer("text_hint");
        assert(brightness == idle);
    }
    if (watch) {
        check_watch_home(argc == 3 ? argv[2] : NULL);
        check_watch_navigation();
        check_watch_home_gesture();
        check_watch_repaint();
    }
    assert(room_board_display_brightness_set(0) == ESP_OK && brightness == 0);
    assert(display_depth == 0 && critical_depth == 0);
    lv_deinit();
    free(frame);
    free(draw_buffer);
    printf("PASS %s: state, power, layering and rendered bounds\n", argv[1]);
    return 0;
}
