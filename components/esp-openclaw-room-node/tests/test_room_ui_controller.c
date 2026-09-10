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
#include "room_ui_controller.h"

static lv_display_t *display;
static uint8_t *frame, *draw_buffer;
static int width, height, brightness, display_depth, critical_depth;
static bool deny_display, canvas_active, face_visible;
static unsigned toggles, holds;
static lv_obj_t *modal;
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

void room_face_set_controller(const room_face_controller_t *controller) { assert(controller); }
esp_err_t room_face_create(lv_obj_t *parent) { assert(parent); return ESP_OK; }
void room_face_show(room_face_state_t state) { (void)state; face_visible = true; }
void room_face_show_hint(int64_t until)
{ (void)until; face_visible = room_board_config()->display.animated_face; }
void room_face_hide(void) { face_visible = false; }
bool room_face_is_visible(void) { return face_visible; }
void room_face_reset_mood(void) {}
void room_canvas_set_action_handler(room_canvas_action_handler_t handler) { canvas_action = handler; }
bool room_canvas_is_active(void) { return canvas_active; }
void room_canvas_view_toggle(void) { ++toggles; }
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
static unsigned check_home_tree(lv_obj_t *parent)
{
    unsigned count = 1;
    assert(!lv_obj_has_flag(parent, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
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

int main(int argc, char **argv)
{
    assert(argc >= 2);
    bool animated = strcmp(argv[1], "animated") == 0;
    int idle = strcmp(argv[1], "tab5") == 0 ? 18 : 0;
    width = animated ? 410 : 1280;
    height = animated ? 502 : 720;
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
        .model_identifier = "m5stack-tab5",
        .display = {.start = start_display, .lock = lock_display, .unlock = unlock_display,
            .set_brightness = set_brightness, .native_width = width, .native_height = height,
            .safe_inset = 24, .animated_face = animated, .idle_brightness = 101},
        .audio = {.open = open_audio, .afe_layout = "MR", .record_channels = 4},
    };
    assert(room_board_bind(&board) == ESP_ERR_INVALID_ARG);
    board.display.idle_brightness = idle;
    assert(room_board_bind(&board) == ESP_OK);
    room_ui_init();
    assert(brightness == idle);
    if (animated) {
        assert(!find_text(lv_screen_active(), "OpenClaw Room Node"));
        room_ui_set(ROOM_UI_LISTENING, NULL);
        assert(face_visible && brightness == 40);
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
        lv_obj_send_event(lv_screen_active(), LV_EVENT_CLICKED, NULL);
        assert(toggles == 1);
        unsigned holds_before = holds;
        lv_obj_send_event(lv_screen_active(), LV_EVENT_LONG_PRESSED, NULL);
        assert(holds == holds_before + 1 && !visible_text("OpenClaw Room Node") && brightness == ROOM_CANVAS_ACTIVE_BRIGHTNESS);
        assert(!visible_text("Talk setup failed"));
        room_diagnostics_close();
        assert(visible_text("Talk setup failed"));
        room_ui_set(ROOM_UI_IDLE, NULL);
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
    assert(room_board_display_brightness_set(0) == ESP_OK && brightness == 0);
    assert(display_depth == 0 && critical_depth == 0);
    lv_deinit();
    free(frame);
    free(draw_buffer);
    printf("PASS %s: state, power, layering and rendered bounds\n", argv[1]);
    return 0;
}
