#include "room_ui.h"

#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "room_canvas.h"

/* 16 rows x 410 px x RGB565 = ~13 KiB, matching CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT. */
#define ROOM_UI_DRAW_ROWS 16

static const char *TAG = "room_ui";
static lv_obj_t *status_label;
static lv_obj_t *talk_pill;
static lv_obj_t *talk_pill_label;
/* Last state/detail survive canvas mode so leaving it restores the live Talk
 * state instead of forcing idle while a call is still active. */
static room_ui_state_t current_state = ROOM_UI_IDLE;
static char current_detail[48];

void room_ui_init(void)
{
    lv_display_t *display = bsp_display_start();
    if (display == NULL) {
        ESP_LOGE(TAG, "failed to start display");
        return;
    }
    /*
     * The BSP's draw buffer is allocated from the default heap and lands in
     * PSRAM, which this QSPI panel cannot DMA from. esp_lcd then bounce-buffers
     * every flush through a fresh internal allocation, and once Wi-Fi/TLS and
     * the always-on audio pipeline claim internal RAM those allocations start
     * failing, silently dropping flushes (stale/overlapping pixels on screen).
     * Own the draw buffer instead: internal DMA-capable memory taken once here,
     * before the network and audio stacks start, so no per-flush allocation is
     * ever needed. Must run under the display lock; the LVGL port task is
     * already rendering into the buffers this replaces.
     */
    if (!bsp_display_lock(0)) {
        ESP_LOGE(TAG, "failed to lock display for draw buffer setup");
        return;
    }
    size_t draw_bytes = (size_t)BSP_LCD_H_RES * ROOM_UI_DRAW_ROWS * 2;
    void *draw_buffer = heap_caps_aligned_alloc(
        CONFIG_LV_DRAW_BUF_ALIGN,
        draw_bytes,
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (draw_buffer != NULL) {
        lv_display_set_buffers(
            display,
            draw_buffer,
            NULL,
            (uint32_t)draw_bytes,
            LV_DISPLAY_RENDER_MODE_PARTIAL);
    } else {
        ESP_LOGW(TAG, "no internal DMA memory for the draw buffer; flushes will bounce");
    }
    bsp_display_unlock();
    if (!bsp_display_lock(0)) {
        ESP_LOGE(TAG, "failed to lock display during initialization");
        return;
    }
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    status_label = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_color(status_label, lv_color_white(), 0);
#if LV_FONT_MONTSERRAT_28
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_28, 0);
#else
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, 0);
#endif
    lv_obj_center(status_label);
    lv_label_set_text(status_label, "OpenClaw");
    bsp_display_unlock();
    bsp_display_brightness_set(0);
}

/* Paints `current_state`/`current_detail` with the display lock held. Returns
 * true when the caller must apply the idle/active brightness after unlocking. */
static bool room_ui_render_locked(void)
{
    static const char *labels[] = {"OpenClaw", "Listening", "Connecting", "Speaking", "Error", "Setup"};
    if (room_canvas_is_active()) {
        if (current_state == ROOM_UI_IDLE) {
            if (talk_pill != NULL) {
                lv_obj_delete(talk_pill);
                talk_pill = NULL;
                talk_pill_label = NULL;
            }
        } else {
            if (talk_pill == NULL) {
                talk_pill = lv_obj_create(lv_layer_top());
                lv_obj_set_height(talk_pill, LV_SIZE_CONTENT);
                lv_obj_set_width(talk_pill, LV_SIZE_CONTENT);
                lv_obj_set_style_bg_color(talk_pill, lv_color_hex(0x181818), 0);
                lv_obj_set_style_bg_opa(talk_pill, LV_OPA_90, 0);
                lv_obj_set_style_border_width(talk_pill, 0, 0);
                lv_obj_set_style_radius(talk_pill, 12, 0);
                lv_obj_set_style_pad_hor(talk_pill, 10, 0);
                lv_obj_set_style_pad_ver(talk_pill, 6, 0);
                talk_pill_label = lv_label_create(talk_pill);
                lv_obj_set_style_text_color(talk_pill_label, lv_color_white(), 0);
                lv_obj_set_style_text_font(talk_pill_label, &lv_font_montserrat_14, 0);
                lv_obj_center(talk_pill_label);
            }
            lv_label_set_text(talk_pill_label, labels[current_state]);
            lv_obj_align(talk_pill, LV_ALIGN_TOP_RIGHT, -8, 8);
        }
        return false;
    }
    if (talk_pill != NULL) {
        lv_obj_delete(talk_pill);
        talk_pill = NULL;
        talk_pill_label = NULL;
    }
    lv_label_set_text_fmt(
        status_label,
        current_detail[0] != '\0' ? "%s\n%s" : "%s",
        labels[current_state],
        current_detail);
    return true;
}

void room_ui_set(room_ui_state_t state, const char *detail)
{
    if (status_label == NULL) {
        ESP_LOGE(TAG, "status display is not initialized");
        return;
    }
    if (!bsp_display_lock(100)) {
        ESP_LOGE(TAG, "failed to lock display for status update");
        return;
    }
    current_state = state;
    current_detail[0] = '\0';
    if (detail != NULL) {
        strlcpy(current_detail, detail, sizeof(current_detail));
    }
    bool apply_brightness = room_ui_render_locked();
    room_ui_state_t painted_state = current_state;
    bsp_display_unlock();
    if (apply_brightness) {
        /* AMOLED is fully dark while idle; active states use a deliberately low brightness. */
        bsp_display_brightness_set(painted_state == ROOM_UI_IDLE ? 0 : 18);
    }
}

void room_ui_refresh(void)
{
    if (status_label == NULL) {
        return;
    }
    if (!bsp_display_lock(100)) {
        ESP_LOGE(TAG, "failed to lock display for status refresh");
        return;
    }
    /* Repaint the stored state under one lock hold; a concurrent Talk
     * transition either lands before this render or repaints right after it,
     * so no snapshot of the state can overwrite a newer one. */
    bool apply_brightness = room_ui_render_locked();
    room_ui_state_t painted_state = current_state;
    bsp_display_unlock();
    if (apply_brightness) {
        bsp_display_brightness_set(painted_state == ROOM_UI_IDLE ? 0 : 18);
    }
}
