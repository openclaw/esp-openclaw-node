#include "openclaw_setup.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_display_t *display;
static lv_indev_t *touch, *encoder;
static lv_indev_data_t pointer_data, encoder_data;
static unsigned commands[6], dma_started, dma_completed;
static openclaw_setup_key_t delivered;
static openclaw_setup_action_t reject = OPENCLAW_SETUP_WAKE;
static bool fail_command, dma_pending, credential_visible_on_cancel;
static lv_obj_t *credential_label;
static uint8_t *frame;
static int width, height;

static esp_err_t action(void *context, openclaw_setup_action_t type, openclaw_setup_key_t *key)
{
    assert(context == &delivered);
    if (type == OPENCLAW_SETUP_CANCEL && credential_visible_on_cancel)
        assert(strstr(lv_label_get_text(credential_label), "PoP:") != NULL);
    ++commands[type];
    if (type == OPENCLAW_SETUP_OPEN) *key = (openclaw_setup_key_t){.request = 7};
    delivered = *key;
    return fail_command && type == reject ? ESP_ERR_TIMEOUT : ESP_OK;
}

static void read_pointer(lv_indev_t *device, lv_indev_data_t *data)
{ assert(lv_indev_get_user_data(device) == &pointer_data); *data = pointer_data; }
static void read_encoder(lv_indev_t *device, lv_indev_data_t *data)
{ assert(lv_indev_get_user_data(device) == &encoder_data); *data = encoder_data; }
static void replacement(lv_indev_t *device, lv_indev_data_t *data) { read_pointer(device, data); }

static void flush(lv_display_t *d, const lv_area_t *area, uint8_t *pixels)
{
    (void)d;
    assert(!dma_pending);
    int stride = lv_draw_buf_width_to_stride(lv_area_get_width(area), LV_COLOR_FORMAT_RGB888);
    for (int y = area->y1; y <= area->y2; ++y)
        memcpy(frame + (y * width + area->x1) * 3, pixels + (y - area->y1) * stride,
               lv_area_get_width(area) * 3);
    dma_pending = true;
    ++dma_started; /* DMA deliberately remains incomplete after the final flush. */
}

static void wait_dma(lv_display_t *d)
{
    assert(dma_pending);
    dma_pending = false;
    ++dma_completed;
    lv_display_flush_ready(d);
}

static lv_obj_t *find(lv_obj_t *root, const char *text)
{
    if (lv_obj_check_type(root, &lv_label_class) && strcmp(lv_label_get_text(root), text) == 0) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *result = find(lv_obj_get_child(root, i), text);
        if (result) return result;
    }
    return NULL;
}

#if LV_USE_QRCODE
static lv_obj_t *find_qr(lv_obj_t *root)
{
    if (lv_obj_check_type(root, &lv_qrcode_class)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *result = find_qr(lv_obj_get_child(root, i));
        if (result) return result;
    }
    return NULL;
}
#endif

static lv_point_t center(const char *caption)
{
    lv_obj_t *label = find(lv_screen_active(), caption);
    assert(label && lv_obj_is_visible(label));
    lv_area_t area;
    lv_obj_get_coords(lv_obj_get_parent(label), &area);
    return (lv_point_t){(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
}

static void read_touch_at(lv_point_t point, bool pressed)
{
    pointer_data = (lv_indev_data_t){.point = point,
        .state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED};
    lv_indev_read(touch);
}

static void tap(const char *caption)
{
    lv_point_t point = center(caption);
    read_touch_at(point, true);
    read_touch_at(point, false);
}

static void snapshot(const char *directory, const char *name)
{
    if (!directory) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.ppm", directory, name);
    FILE *file = fopen(path, "wbx");
    assert(file);
    fprintf(file, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; ++i) {
        uint8_t rgb[] = {frame[i * 3 + 2], frame[i * 3 + 1], frame[i * 3]};
        assert(fwrite(rgb, 1, 3, file) == 3);
    }
    fclose(file);
}

int main(int argc, char **argv)
{
    bool watch = strcmp(argv[1], "watch") == 0;
    width = watch ? 368 : 240;
    height = watch ? 448 : 240;
    lv_init();
    display = lv_display_create(width, height);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB888);
    frame = calloc(width * height, 3);
    void *buffer = malloc(width * 16 * 3);
    lv_display_set_buffers(display, buffer, NULL, width * 16 * 3, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_flush_wait_cb(display, wait_dma);
    touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_mode(touch, LV_INDEV_MODE_EVENT);
    lv_indev_set_user_data(touch, &pointer_data);
    lv_indev_set_read_cb(touch, read_pointer);
    encoder = lv_indev_create();
    lv_indev_set_type(encoder, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_mode(encoder, LV_INDEV_MODE_EVENT);
    lv_indev_set_user_data(encoder, &encoder_data);
    lv_indev_set_read_cb(encoder, read_encoder);
    lv_obj_update_layout(lv_screen_active());
    openclaw_setup_t *view = openclaw_setup_create(lv_screen_active(), watch, action, &delivered);
    assert(view && openclaw_setup_show(view));
    esp_openclaw_setup_runtime_snapshot_t state = {.can_open = true};
    openclaw_setup_update(view, &state, true);
    lv_refr_now(display);
#if LV_USE_QRCODE
    assert(!lv_obj_is_visible(find_qr(lv_screen_active())));
#endif
    snapshot(argc > 2 ? argv[2] : NULL, "setup-start");
    tap("Start phone setup");
    unsigned cancellations_before_wake = commands[OPENCLAW_SETUP_CANCEL];
    openclaw_setup_update(view, &state, false); /* Power state is newer than this pre-OPEN snapshot. */
    read_touch_at(center("Cancel"), true);
    assert(commands[OPENCLAW_SETUP_WAKE] == 1 && commands[OPENCLAW_SETUP_CANCEL] == cancellations_before_wake);
    read_touch_at(center("Cancel"), false);
    openclaw_setup_update(view, &state, true); /* stale pre-OPEN snapshot */
    openclaw_setup_close(view);
    assert(commands[OPENCLAW_SETUP_OPEN] == 1 && delivered.request == 7);
    assert(openclaw_setup_show(view));
    openclaw_setup_update(view, &state, true);
    lv_refr_now(display);
    tap("Other ways");
    cancellations_before_wake = commands[OPENCLAW_SETUP_CANCEL];
    openclaw_setup_update(view, &state, false);
    if (watch) {
        assert(openclaw_setup_boot_sample(view, true));
        assert(openclaw_setup_boot_sample(view, false));
    } else {
        read_touch_at(center("Cancel"), true);
        read_touch_at(center("Cancel"), false);
    }
    assert(commands[OPENCLAW_SETUP_WAKE] == 2 && commands[OPENCLAW_SETUP_CANCEL] == cancellations_before_wake);
    openclaw_setup_update(view, &state, true);
    tap("Cancel");
    assert(commands[OPENCLAW_SETUP_CANCEL] == cancellations_before_wake + 1);
    assert(openclaw_setup_show(view));
    state = (esp_openclaw_setup_runtime_snapshot_t){.request_id = 8,
        .phase = ESP_OPENCLAW_SETUP_RUNTIME_ACTIVE,
        .session = {.generation = 9, .phase = ESP_OPENCLAW_SETUP_DISCOVERABLE},
        .qr = "synthetic-noncredential-qr", .service = "PROV_OC_12345678",
        .pop = "00000000000000000000000000000000"};
    openclaw_setup_update(view, &state, true);
    lv_refr_now(display);
#if LV_USE_QRCODE
    lv_obj_t *qr = find_qr(lv_screen_active());
    assert(qr && lv_obj_is_visible(qr) && lv_qrcode_is_render_valid(qr));
    lv_area_t qr_area;
    lv_obj_get_coords(qr, &qr_area);
    unsigned black = 0, white = 0;
    for (int y = qr_area.y1; y <= qr_area.y2; ++y)
        for (int x = qr_area.x1; x <= qr_area.x2; ++x) {
            uint8_t *pixel = frame + (y * width + x) * 3;
            if (pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0) ++black;
            if (pixel[0] == 255 && pixel[1] == 255 && pixel[2] == 255) ++white;
        }
    assert(black && white); /* Actual rendered QR, including its resized I1 palette. */
    tap("QR / manual");
    openclaw_setup_update(view, &state, true);
    assert(!lv_obj_is_visible(qr));
#endif
    assert(find(lv_screen_active(), "PROV_OC_12345678\nopenclaw\nPoP:\n0000000000000000\n0000000000000000"));
    credential_label = find(lv_screen_active(), "PROV_OC_12345678\nopenclaw\nPoP:\n0000000000000000\n0000000000000000");
    credential_visible_on_cancel = true;
    openclaw_setup_close(view);
    credential_visible_on_cancel = false;
    assert(strcmp(lv_label_get_text(credential_label), "") == 0);
#if LV_USE_QRCODE
    lv_draw_buf_t *qr_buffer = lv_canvas_get_draw_buf(qr);
    assert(qr_buffer->header.cf == LV_COLOR_FORMAT_I1);
    for (unsigned y = 0; y < qr_buffer->header.h; ++y)
        for (unsigned x = 0; x < qr_buffer->header.w; ++x)
            assert((*(uint8_t *)lv_draw_buf_goto_xy(qr_buffer, x, y) & (0x80U >> (x % 8))) == 0);
#endif
    assert(openclaw_setup_show(view));
    state = (esp_openclaw_setup_runtime_snapshot_t){.request_id = 11, .peer = 22,
        .phase = ESP_OPENCLAW_SETUP_RUNTIME_ACTIVE,
        .session = {.generation = 33, .phase = ESP_OPENCLAW_SETUP_COMPARISON}, .comparison = "ABCD 1234"};
    encoder_data.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(encoder);
    openclaw_setup_update(view, &state, true);
    assert(dma_pending && dma_completed < dma_started);
    read_touch_at((lv_point_t){0, 0}, false);
    openclaw_setup_boot_sample(view, false);
    assert(commands[OPENCLAW_SETUP_NEUTRAL] == 0); /* final DMA still pending */
    lv_refr_now(display);
    openclaw_setup_boot_sample(view, false);
    encoder_data.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(encoder);
    assert(commands[OPENCLAW_SETUP_NEUTRAL] == 0); /* cached touch release is insufficient */
    read_touch_at((lv_point_t){0, 0}, false);
    assert(commands[OPENCLAW_SETUP_NEUTRAL] == 1);
    lv_obj_t *confirm = lv_obj_get_parent(find(lv_screen_active(), "Codes match"));
    lv_obj_send_event(confirm, LV_EVENT_PRESSED, NULL);
    lv_obj_send_event(confirm, LV_EVENT_RELEASED, NULL);
    lv_obj_send_event(confirm, LV_EVENT_CLICKED, NULL);
    assert(commands[OPENCLAW_SETUP_CONFIRM] == 0);
    lv_refr_now(display);
    snapshot(argc > 2 ? argv[2] : NULL, "setup-comparison");
    tap("Codes match");
    assert(commands[OPENCLAW_SETUP_CONFIRM] == 1);
    assert(delivered.request == 11 && delivered.generation == 33 && delivered.peer == 22);
    read_touch_at(center("Codes match"), false);
    assert(commands[OPENCLAW_SETUP_CONFIRM] == 1);

    /* A held old press cannot confirm a replacement peer/comparison. */
    read_touch_at(center("Codes match"), true);
    ++state.peer;
    openclaw_setup_update(view, &state, true);
    lv_refr_now(display);
    lv_indev_read(encoder);
    openclaw_setup_boot_sample(view, false);
    read_touch_at(center("Codes match"), false);
    assert(commands[OPENCLAW_SETUP_CONFIRM] == 1);
    tap("Codes match");
    assert(commands[OPENCLAW_SETUP_CONFIRM] == 2 && delivered.peer == 23);

    /* Queue rejection stays visible; repeated reads never auto replay it. */
    ++state.peer;
    openclaw_setup_update(view, &state, true);
    lv_refr_now(display);
    fail_command = true;
    reject = OPENCLAW_SETUP_NEUTRAL;
    lv_indev_read(encoder);
    openclaw_setup_boot_sample(view, false);
    read_touch_at(center("Codes match"), false);
    unsigned rejected = commands[OPENCLAW_SETUP_NEUTRAL];
    lv_indev_read(touch);
    lv_indev_read(encoder);
    assert(commands[OPENCLAW_SETUP_NEUTRAL] == rejected);
    fail_command = false;
    tap("Retry");
    lv_indev_read(encoder);
    openclaw_setup_boot_sample(view, false);
    lv_indev_read(touch);
    assert(commands[OPENCLAW_SETUP_NEUTRAL] == rejected + 1);

    /* Wake consumes the complete first hold/release, even across repaint. */
    unsigned wakes_before_comparison = commands[OPENCLAW_SETUP_WAKE];
    openclaw_setup_update(view, &state, false);
    read_touch_at(center("Codes match"), true);
    assert(commands[OPENCLAW_SETUP_WAKE] == wakes_before_comparison + 1);
    openclaw_setup_update(view, &state, true);
    lv_refr_now(display);
    lv_indev_read(encoder);
    openclaw_setup_boot_sample(view, false);
    read_touch_at(center("Codes match"), false);
    assert(commands[OPENCLAW_SETUP_CONFIRM] == 2);
    tap("Codes match");
    assert(commands[OPENCLAW_SETUP_CONFIRM] == 3);
    /* Encoder confirmation requires actual focus and a fresh physical cycle. */
    ++state.peer;
    openclaw_setup_update(view, &state, true);
    lv_refr_now(display);
    lv_group_t *group = lv_group_create();
    lv_group_add_obj(group, confirm);
    lv_indev_set_group(encoder, group);
    lv_indev_read(encoder);
    lv_indev_read(touch);
    openclaw_setup_boot_sample(view, false);
    lv_group_focus_obj(confirm);
    assert(lv_group_get_focused(group) == confirm);
    encoder_data.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(encoder);
    encoder_data.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(encoder);
    assert(commands[OPENCLAW_SETUP_CONFIRM] == 4);
    unsigned cancellations = commands[OPENCLAW_SETUP_CANCEL];
    if (watch) assert(openclaw_setup_boot_sample(view, true));
    else openclaw_setup_close(view);
    assert(commands[OPENCLAW_SETUP_CANCEL] == cancellations + 1);
    assert(lv_indev_get_read_cb(touch) == read_pointer && lv_indev_get_read_cb(encoder) == read_encoder);
    openclaw_setup_update(view, &state, true); /* late snapshot cannot reopen */
    assert(!lv_obj_is_visible(confirm));
    assert(openclaw_setup_show(view));
    lv_indev_set_read_cb(touch, replacement);
    openclaw_setup_update(view, &state, true);
    assert(commands[OPENCLAW_SETUP_CANCEL] == cancellations + 2);
    assert(lv_indev_get_read_cb(touch) == replacement);
    openclaw_setup_destroy(view);
    lv_indev_delete(touch);
    lv_indev_delete(encoder);
    lv_group_delete(group);
    if (dma_pending) wait_dma(display);
    lv_display_delete(display);
    free(buffer);
    free(frame);
    puts(watch ? "Watch setup presentation/input proof: PASS" : "Dial geometry/shared input proof: PASS");
    return 0;
}
