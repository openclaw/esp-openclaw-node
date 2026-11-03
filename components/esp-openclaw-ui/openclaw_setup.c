#include "openclaw_setup.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define INPUT_LIMIT 4
typedef struct {
    lv_indev_t *device;
    lv_indev_read_cb_t read;
    bool released, pressed, candidate;
    lv_point_t point;
} setup_input_t;

struct openclaw_setup {
    lv_obj_t *root, *heading, *body, *code, *qr, *confirm, *start, *retry, *cancel, *other, *manual_button;
    lv_display_t *display;
    openclaw_setup_action_cb_t action;
    void *context;
    openclaw_setup_key_t key;
    setup_input_t inputs[INPUT_LIMIT];
    unsigned count;
    bool visible, awake, has_boot, boot_released, boot_pressed;
    bool waiting, painted, neutral, failed, failed_open, comparison, other_ways, manual_visible;
    char discovery[192], manual[112], comparison_text[10];
};

/* A device has one local presentation of its one canonical setup session. */
static openclaw_setup_t *current;
static void observe_read(lv_indev_t *device, lv_indev_data_t *data);

static void wipe(void *data, size_t size)
{
    volatile unsigned char *p = data;
    while (size--) *p++ = 0;
}

static void disarm(openclaw_setup_t *view)
{
    view->waiting = view->painted = view->neutral = false;
    view->boot_released = !view->has_boot;
    for (unsigned i = 0; i < view->count; ++i)
        view->inputs[i].released = view->inputs[i].candidate = false;
    lv_obj_set_disabled(view->confirm, true);
}

static void clear_discovery(openclaw_setup_t *view)
{
    size_t length = strlen(view->discovery);
    wipe(view->discovery, sizeof(view->discovery));
    wipe(view->manual, sizeof(view->manual));
    wipe(view->comparison_text, sizeof(view->comparison_text));
    lv_label_set_text_static(view->code, "");
#if LV_USE_QRCODE
    /* Replace LVGL's retained payload too; its allocator has no secure erase API. */
    if (length) lv_qrcode_update(view->qr, view->discovery, length);
    /* Indexed canvas fill uses the blue byte as palette index; zero is white. */
    lv_canvas_fill_bg(view->qr, lv_color_black(), LV_OPA_COVER);
#else
    (void)length;
#endif
    lv_obj_set_hidden(view->qr, true);
}

static esp_err_t command(openclaw_setup_t *view, openclaw_setup_action_t action)
{
    openclaw_setup_key_t key = view->key;
    esp_err_t result = view->action(view->context, action, &key);
    if (result == ESP_OK && action == OPENCLAW_SETUP_OPEN) {
        view->key = key;
        lv_label_set_text_static(view->body, "Preparing secure setup...");
        lv_obj_set_hidden(view->start, true);
    }
    if (result != ESP_OK && action != OPENCLAW_SETUP_WAKE) {
        view->failed = true;
        view->failed_open = action == OPENCLAW_SETUP_OPEN;
        disarm(view);
        lv_label_set_text_static(view->body, "Action unavailable. Tap Retry or Cancel.");
        lv_obj_set_hidden(view->retry, false);
    }
    return result;
}

static bool inputs_current(openclaw_setup_t *view)
{
    unsigned count = 0;
    for (lv_indev_t *d = lv_indev_get_next(NULL); d; d = lv_indev_get_next(d)) {
        if (lv_indev_get_display(d) != view->display) continue;
        bool found = false;
        for (unsigned i = 0; i < view->count; ++i)
            if (view->inputs[i].device == d && lv_indev_get_read_cb(d) == observe_read) found = true;
        if (!found) return false;
        ++count;
    }
    return count == view->count;
}

static bool inside(lv_obj_t *object, lv_point_t point)
{
    return lv_obj_is_visible(object) && lv_obj_hit_test(object, &point);
}

static void neutral_if_ready(openclaw_setup_t *view)
{
    if (!view->painted || view->neutral || view->failed || !view->awake || !view->boot_released) return;
    for (unsigned i = 0; i < view->count; ++i)
        if (!view->inputs[i].released || view->inputs[i].pressed) return;
    if (command(view, OPENCLAW_SETUP_NEUTRAL) == ESP_OK) {
        view->neutral = true;
        lv_obj_set_disabled(view->confirm, false);
        lv_label_set_text_static(view->body, "Same code? Press Codes match.");
    }
}

static void observe_read(lv_indev_t *device, lv_indev_data_t *data)
{
    openclaw_setup_t *view = current;
    if (!view) return;
    setup_input_t *input = NULL;
    for (unsigned i = 0; i < view->count; ++i) if (view->inputs[i].device == device) input = &view->inputs[i];
    if (!input) return;
    input->read(device, data); /* Preserve board driver/user data and full output. */
    bool pressed = data->state == LV_INDEV_STATE_PRESSED;
    bool rising = pressed && !input->pressed;
    input->pressed = pressed;
    if (!view->visible || !lv_obj_is_visible(view->root)) return;
    if (!view->awake) {
        disarm(view);
        if (rising) {
            lv_indev_wait_release(device);
            command(view, OPENCLAW_SETUP_WAKE);
        }
        return;
    }
    if (!view->comparison || !view->painted || view->failed) return;
    if (!inputs_current(view)) { openclaw_setup_close(view); return; }
    input->released = !pressed && data->enc_diff == 0;
    neutral_if_ready(view);
    if (!view->neutral) return;
    bool pointer = lv_indev_get_type(device) == LV_INDEV_TYPE_POINTER;
    if (rising) {
        input->point = data->point;
        bool enter = lv_indev_get_type(device) == LV_INDEV_TYPE_ENCODER ||
            (lv_indev_get_type(device) == LV_INDEV_TYPE_KEYPAD && data->key == LV_KEY_ENTER);
        input->candidate = pointer ? inside(view->confirm, data->point) :
            enter && lv_indev_get_group(device) && lv_group_get_focused(lv_indev_get_group(device)) == view->confirm;
    }
    if (pointer && input->candidate && (abs(data->point.x - input->point.x) > LV_INDEV_DEF_SCROLL_LIMIT ||
        abs(data->point.y - input->point.y) > LV_INDEV_DEF_SCROLL_LIMIT)) input->candidate = false;
    if (!pointer && data->enc_diff) input->candidate = false;
    if (!pressed && input->candidate) {
        input->candidate = false;
        if ((!pointer || inside(view->confirm, data->point)) && inputs_current(view)) {
            disarm(view); /* One physical attempt, including a rejected queue write. */
            command(view, OPENCLAW_SETUP_CONFIRM);
        }
    }
}

static void display_event(lv_event_t *event)
{
    openclaw_setup_t *view = lv_event_get_user_data(event);
    if (view->visible && view->comparison && view->waiting) {
        /* The preceding final DMA has drained, unlike FLUSH_FINISH/REFR_READY. */
        view->waiting = false;
        view->painted = true;
    }
}

static void button_event(lv_event_t *event)
{
    openclaw_setup_t *view = lv_event_get_user_data(event);
    lv_obj_t *button = lv_event_get_target_obj(event);
    if (!view->visible || !view->awake) return;
    if (button == view->cancel) openclaw_setup_close(view);
    else if (button == view->other) {
        disarm(view);
        command(view, OPENCLAW_SETUP_CANCEL);
        clear_discovery(view);
        view->other_ways = true;
        lv_label_set_text_static(view->heading, "Other ways");
        lv_label_set_text_static(view->body, "Use USB serial setup:\n\nwifi set <ssid> <password>\ngateway setup-code <code>\n\nKeep passwords and setup codes private.");
        lv_obj_set_hidden(view->confirm, true);
        lv_obj_set_hidden(view->start, true);
        lv_obj_set_hidden(view->retry, true);
        lv_obj_set_hidden(view->manual_button, true);
    } else if (button == view->start) command(view, OPENCLAW_SETUP_OPEN);
    else if (button == view->manual_button) {
        view->manual_visible = !view->manual_visible;
        lv_obj_set_hidden(view->qr, true);
    }
    else if (button == view->retry) {
        if (view->failed && view->failed_open) { view->failed = false; command(view, OPENCLAW_SETUP_OPEN); }
        else if (view->failed && view->comparison) {
            view->failed = false;
            disarm(view);
            view->painted = true; /* Same comparison remains painted. New reads required. */
        } else command(view, OPENCLAW_SETUP_RETRY);
    }
}

static lv_obj_t *label(lv_obj_t *parent, int y, int width)
{
    lv_obj_t *object = lv_label_create(parent);
    int inset = width < 300 ? 28 : 16;
    lv_obj_set_pos(object, inset, y);
    lv_obj_set_width(object, width - 2 * inset);
    lv_obj_set_style_text_font(object, &lv_font_montserrat_14, 0);
    return object;
}

static lv_obj_t *button(openclaw_setup_t *view, const char *text, int x, int y, int width)
{
    lv_obj_t *object = lv_button_create(view->root);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, lv_obj_get_height(view->root) < 300 ? 30 : 34);
    lv_obj_t *caption = lv_label_create(object);
    lv_label_set_text_static(caption, text);
    lv_obj_center(caption);
    lv_obj_add_event_cb(object, button_event, LV_EVENT_CLICKED, view);
    return object;
}

openclaw_setup_t *openclaw_setup_create(lv_obj_t *parent, bool has_boot,
    openclaw_setup_action_cb_t action, void *context)
{
    if (current || !parent || !action) return NULL;
    lv_obj_update_layout(parent);
    openclaw_setup_t *view = calloc(1, sizeof(*view));
    if (!view) return NULL;
    view->display = lv_obj_get_display(parent);
    view->action = action;
    view->context = context;
    view->has_boot = has_boot;
    view->root = lv_obj_create(parent);
    lv_obj_remove_style_all(view->root);
    lv_obj_set_size(view->root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(view->root, lv_color_hex(0x15101b), 0);
    lv_obj_set_style_bg_opa(view->root, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(view->root, lv_color_hex(0xf4efee), 0);
    lv_obj_set_scrollable(view->root, false);
    lv_obj_update_layout(view->root);
    int width = lv_obj_get_width(parent), height = lv_obj_get_height(parent);
    view->heading = label(view->root, 14, width);
    lv_obj_set_style_text_align(view->heading, LV_TEXT_ALIGN_CENTER, 0);
    view->body = label(view->root, 48, width);
    view->code = label(view->root, 72, width);
#if LV_USE_QRCODE
    view->qr = lv_qrcode_create(view->root);
    lv_qrcode_set_quiet_zone(view->qr, true);
    lv_qrcode_set_size(view->qr, height < 300 ? 108 : 156);
    /* Resizing replaces the I1 buffer, including its palette (LVGL 9.6). */
    lv_canvas_set_palette(view->qr, 0, lv_color_to_32(lv_color_white(), LV_OPA_COVER));
    lv_canvas_set_palette(view->qr, 1, lv_color_to_32(lv_color_black(), LV_OPA_COVER));
    lv_obj_align(view->qr, LV_ALIGN_TOP_MID, 0, height < 300 ? 40 : 100);
#else
    view->qr = lv_obj_create(view->root);
#endif
    int primary_y = height - (height < 300 ? 90 : 80);
    int footer_y = height - (height < 300 ? 56 : 38), side = width < 300 ? 32 : 8;
    view->confirm = button(view, "Codes match", 16, primary_y, width - 32);
    view->start = button(view, "Start phone setup", 16, primary_y, width - 32);
    view->retry = button(view, "Retry", 16, primary_y, width - 32);
    view->manual_button = button(view, "QR / manual", 16, primary_y, width - 32);
    view->other = button(view, "Other ways", side, footer_y, width / 2 - side - 4);
    view->cancel = button(view, "Cancel", width / 2 + 4, footer_y, width / 2 - side - 4);
    lv_display_add_event_cb(view->display, display_event, LV_EVENT_FLUSH_WAIT_FINISH, view);
    lv_obj_set_hidden(view->root, true);
    current = view;
    return view;
}

bool openclaw_setup_show(openclaw_setup_t *view)
{
    if (!view || view->visible) return false;
    view->count = 0;
    for (lv_indev_t *d = lv_indev_get_next(NULL); d; d = lv_indev_get_next(d)) {
        if (lv_indev_get_display(d) != view->display) continue;
        if (view->count == INPUT_LIMIT || !lv_indev_get_read_cb(d)) return false;
        view->inputs[view->count++] = (setup_input_t){.device = d, .read = lv_indev_get_read_cb(d)};
    }
    if (!view->count) return false;
    for (unsigned i = 0; i < view->count; ++i) lv_indev_set_read_cb(view->inputs[i].device, observe_read);
    view->visible = true;
    view->other_ways = view->failed = view->manual_visible = view->comparison = false;
    view->key = (openclaw_setup_key_t){0};
    clear_discovery(view);
    lv_obj_set_hidden(view->root, false);
    disarm(view);
    return true;
}

void openclaw_setup_update(openclaw_setup_t *view,
    const esp_openclaw_setup_runtime_snapshot_t *snapshot, bool awake)
{
    if (!view || !view->visible) return;
    bool was_awake = view->awake;
    view->awake = awake;
    if (view->other_ways) return;
    if (snapshot->request_id < view->key.request) return; /* Pre-OPEN snapshot cannot lose its cancel ID. */
    if (!inputs_current(view)) { openclaw_setup_close(view); return; }
    openclaw_setup_key_t key = {snapshot->request_id, snapshot->session.generation, snapshot->peer};
    bool comparison = snapshot->session.phase == ESP_OPENCLAW_SETUP_COMPARISON && key.peer && snapshot->comparison[0];
    bool changed = memcmp(&key, &view->key, sizeof(key)) != 0 || comparison != view->comparison ||
        (comparison && (strcmp(view->comparison_text, snapshot->comparison) != 0 || (awake && !was_awake)));
    view->key = key;
    view->comparison = comparison;
    if (changed) { view->failed = false; disarm(view); clear_discovery(view); }
    if ((!snapshot->qr[0] || comparison) &&
        (view->discovery[0] || (!comparison && view->comparison_text[0]))) clear_discovery(view);
    lv_obj_set_hidden(view->confirm, true);
    lv_obj_set_hidden(view->retry, true);
    lv_obj_set_hidden(view->start, true);
    lv_obj_set_hidden(view->manual_button, true);
    lv_obj_set_hidden(view->body, false);
    lv_obj_set_y(view->body, comparison ? (lv_obj_get_height(view->root) < 300 ? 108 : 118) : 48);
    if (view->failed) { lv_obj_set_hidden(view->retry, false); return; }
    const char *title = "Phone setup", *body = "Preparing secure setup...";
    if (comparison) {
        title = "Compare codes";
        body = view->neutral ? "Same code? Press Codes match." : "Match phone code.\nRelease all controls.";
        memcpy(view->comparison_text, snapshot->comparison, sizeof(view->comparison_text));
        lv_label_set_text_static(view->code, view->comparison_text);
#if LV_FONT_MONTSERRAT_28
        lv_obj_set_style_text_font(view->code, &lv_font_montserrat_28, 0);
#endif
        lv_obj_set_hidden(view->confirm, false);
    } else if (snapshot->error != ESP_OK) {
        body = snapshot->can_open ? "Setup could not start. Tap Start again." : "Setup needs attention. Retry cleanup or Cancel.";
        lv_obj_set_hidden(snapshot->can_open ? view->start : view->retry, false);
    } else if (snapshot->phase == ESP_OPENCLAW_SETUP_RUNTIME_CLOSED) {
        body = snapshot->can_open ? "Use OpenClaw on your phone to set up Wi-Fi and Gateway." : "Waiting for the previous setup to finish cleanup.";
        if (snapshot->can_open) lv_obj_set_hidden(view->start, false);
    } else if (snapshot->phase == ESP_OPENCLAW_SETUP_RUNTIME_CLOSING) body = "Closing setup. Earlier saved changes may remain.";
    else if (snapshot->session.phase == ESP_OPENCLAW_SETUP_READY || snapshot->session.phase == ESP_OPENCLAW_SETUP_BUSY) {
        title = "Phone connected";
        body = snapshot->session.phase == ESP_OPENCLAW_SETUP_BUSY ? "Applying your phone's request..." : "Continue setup on your phone. Review the destination before Apply.";
        if (snapshot->session.has_operation && snapshot->session.phase == ESP_OPENCLAW_SETUP_READY) {
            if (snapshot->session.operation.outcome == ESP_OPENCLAW_SETUP_FAILED)
                body = "Request failed. Review the result on your phone.";
            else if (snapshot->session.operation.outcome == ESP_OPENCLAW_SETUP_UNKNOWN)
                body = "Outcome unknown. Check your phone before retrying.";
            else if (snapshot->session.operation.outcome == ESP_OPENCLAW_SETUP_SUCCEEDED)
                body = "Request completed. Continue setup on your phone.";
        }
    } else if (snapshot->qr[0]) {
        title = "Scan in OpenClaw";
#if LV_USE_QRCODE
        lv_obj_set_hidden(view->manual_button, false);
#endif
        lv_obj_set_style_text_font(view->code, &lv_font_montserrat_14, 0);
        lv_obj_set_hidden(view->body, true);
        if (strcmp(view->discovery, snapshot->qr) != 0) {
            memcpy(view->discovery, snapshot->qr, sizeof(view->discovery));
            snprintf(view->manual, sizeof(view->manual), "%s\nopenclaw\nPoP:\n%.16s\n%.16s",
                     snapshot->service, snapshot->pop, snapshot->pop + 16);
#if LV_USE_QRCODE
            if (lv_qrcode_update(view->qr, view->discovery, strlen(view->discovery)) != LV_RESULT_OK)
                view->manual_visible = true;
#else
            view->manual_visible = true;
#endif
        }
        if (view->manual_visible) {
            title = "Manual phone setup";
            lv_label_set_text_static(view->code, view->manual);
        }
        else {
            lv_label_set_text_static(view->code, "");
            lv_obj_set_hidden(view->qr, false);
        }
    }
    lv_label_set_text_static(view->heading, title);
    lv_label_set_text_static(view->body, body);
    if (comparison && changed) {
        lv_obj_update_layout(view->root);
        lv_refr_now(view->display);
        view->waiting = true;
        lv_obj_invalidate(view->code); /* Next wait must drain the preceding final DMA. */
    }
}

void openclaw_setup_close(openclaw_setup_t *view)
{
    if (!view || !view->visible) return;
    view->visible = false;
    disarm(view);
    command(view, OPENCLAW_SETUP_CANCEL);
    clear_discovery(view);
    for (unsigned i = 0; i < view->count; ++i)
        if (lv_indev_get_read_cb(view->inputs[i].device) == observe_read)
            lv_indev_set_read_cb(view->inputs[i].device, view->inputs[i].read);
    lv_obj_set_hidden(view->root, true);
}

bool openclaw_setup_boot_sample(openclaw_setup_t *view, bool pressed)
{
    if (!view || !view->visible) return false;
    bool rising = pressed && !view->boot_pressed;
    view->boot_pressed = pressed;
    if (!view->awake) { if (rising) command(view, OPENCLAW_SETUP_WAKE); return true; }
    if (rising) openclaw_setup_close(view);
    else if (!pressed && view->painted) { view->boot_released = true; neutral_if_ready(view); }
    return true;
}

void openclaw_setup_destroy(openclaw_setup_t *view)
{
    if (!view) return;
    openclaw_setup_close(view);
    lv_display_remove_event_cb_with_user_data(view->display, display_event, view);
    lv_obj_delete(view->root);
    wipe(view, sizeof(*view));
    free(view);
    current = NULL;
}
