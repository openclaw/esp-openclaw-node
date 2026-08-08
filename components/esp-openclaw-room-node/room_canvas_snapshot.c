#include "room_canvas.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_jpeg_enc.h"
#include "mbedtls/base64.h"
#include "room_board.h"
#include "room_canvas_internal.h"
#include "src/core/lv_obj_draw_private.h"

#define ROOM_CANVAS_DISPLAY_LOCK_MS 1000

esp_err_t room_canvas_snapshot(
    int max_width,
    double quality,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    if (room_canvas_screen == NULL) {
        return room_canvas_set_error(out_error, "UNAVAILABLE", "the display is not initialized", ESP_ERR_INVALID_STATE);
    }
    if (!room_board_display_lock(ROOM_CANVAS_DISPLAY_LOCK_MS)) {
        return room_canvas_set_error(out_error, "UNAVAILABLE", "the display is busy; retry canvas.snapshot", ESP_ERR_TIMEOUT);
    }
    lv_obj_t *active_screen = lv_screen_active();
    lv_obj_update_layout(active_screen);
    int32_t ext_draw_size = lv_obj_get_ext_draw_size(active_screen);
    int64_t snapshot_width = (int64_t)lv_obj_get_width(active_screen) + (int64_t)ext_draw_size * 2;
    int64_t snapshot_height = (int64_t)lv_obj_get_height(active_screen) + (int64_t)ext_draw_size * 2;
    if (ext_draw_size < 0 || snapshot_width <= 0 || snapshot_height <= 0 ||
        snapshot_width > UINT16_MAX || snapshot_height > UINT16_MAX) {
        room_board_display_unlock();
        return room_canvas_set_error(out_error, "INTERNAL", "LVGL could not capture the active screen", ESP_FAIL);
    }
    uint32_t snapshot_stride = lv_draw_buf_width_to_stride(
        (uint32_t)snapshot_width,
        LV_COLOR_FORMAT_RGB888);
    if (snapshot_stride == 0 || snapshot_stride > UINT16_MAX ||
        (uint64_t)snapshot_stride * (uint64_t)snapshot_height > UINT32_MAX) {
        room_board_display_unlock();
        return room_canvas_set_error(out_error, "INTERNAL", "LVGL could not capture the active screen", ESP_FAIL);
    }
    uint32_t snapshot_size = snapshot_stride * (uint32_t)snapshot_height;
    uint8_t *snapshot_pixels = room_canvas_large_aligned_alloc(LV_DRAW_BUF_ALIGN, snapshot_size);
    if (snapshot_pixels == NULL) {
        room_board_display_unlock();
        return room_canvas_set_error(out_error, "INTERNAL", "LVGL could not capture the active screen", ESP_FAIL);
    }
    /* The stack descriptor borrows caller-owned storage; LVGL must not destroy either. */
    lv_draw_buf_t snapshot;
    if (lv_draw_buf_init(
            &snapshot,
            (uint32_t)snapshot_width,
            (uint32_t)snapshot_height,
            LV_COLOR_FORMAT_RGB888,
            snapshot_stride,
            snapshot_pixels,
            snapshot_size) != LV_RESULT_OK ||
        lv_snapshot_take_to_draw_buf(
            active_screen,
            LV_COLOR_FORMAT_RGB888,
            &snapshot) != LV_RESULT_OK) {
        heap_caps_free(snapshot_pixels);
        room_board_display_unlock();
        return room_canvas_set_error(out_error, "INTERNAL", "LVGL could not capture the active screen", ESP_FAIL);
    }

    int source_width = (int)snapshot.header.w;
    int source_height = (int)snapshot.header.h;
    int width = max_width > 0 && max_width < source_width ? max_width : source_width;
    int height = width < source_width
        ? (int)(((int64_t)source_height * width) / source_width)
        : source_height;
    if (height < 1) {
        height = 1;
    }
    if ((size_t)width > SIZE_MAX / 3U / (size_t)height) {
        heap_caps_free(snapshot_pixels);
        room_board_display_unlock();
        return room_canvas_set_error(out_error, "INTERNAL", "LVGL could not capture the active screen", ESP_FAIL);
    }
    size_t rgb_size = (size_t)width * (size_t)height * 3;
    if (rgb_size > INT_MAX) {
        heap_caps_free(snapshot_pixels);
        room_board_display_unlock();
        return room_canvas_set_error(out_error, "INTERNAL", "LVGL could not capture the active screen", ESP_FAIL);
    }
    uint8_t *rgb = room_canvas_large_aligned_alloc(16, rgb_size);
    if (rgb == NULL) {
        heap_caps_free(snapshot_pixels);
        room_board_display_unlock();
        return room_canvas_set_error(out_error, "INTERNAL", "not enough memory for the snapshot pixels", ESP_ERR_NO_MEM);
    }
    for (int y = 0; y < height; ++y) {
        int source_y = (int)(((int64_t)y * source_height) / height);
        const uint8_t *source_row = snapshot.data +
            ((size_t)source_y * snapshot.header.stride);
        uint8_t *target_row = rgb + ((size_t)y * (size_t)width * 3);
        for (int x = 0; x < width; ++x) {
            int source_x = (int)(((int64_t)x * source_width) / width);
            const uint8_t *source_pixel = source_row + ((size_t)source_x * 3);
            uint8_t *target_pixel = target_row + ((size_t)x * 3);
            target_pixel[0] = source_pixel[2];
            target_pixel[1] = source_pixel[1];
            target_pixel[2] = source_pixel[0];
        }
    }
    heap_caps_free(snapshot_pixels);
    room_board_display_unlock();

    jpeg_enc_config_t config = DEFAULT_JPEG_ENC_CONFIG();
    config.width = width;
    config.height = height;
    config.src_type = JPEG_PIXEL_FORMAT_RGB888;
    config.subsampling = JPEG_SUBSAMPLE_420;
    config.quality = (uint8_t)(1 + (int)(quality * 99.0));
    config.rotate = JPEG_ROTATE_0D;
    config.task_enable = false;
    jpeg_enc_handle_t encoder = NULL;
    if (jpeg_enc_open(&config, &encoder) != JPEG_ERR_OK || encoder == NULL) {
        heap_caps_free(rgb);
        return room_canvas_set_error(out_error, "INTERNAL", "could not initialize the JPEG encoder", ESP_FAIL);
    }

    size_t jpeg_capacity = rgb_size + 1024;
    uint8_t *jpeg = room_canvas_large_alloc(jpeg_capacity);
    int jpeg_size = 0;
    jpeg_error_t jpeg_err = jpeg != NULL
        ? jpeg_enc_process(
            encoder,
            rgb,
            (int)rgb_size,
            jpeg,
            (int)jpeg_capacity,
            &jpeg_size)
        : JPEG_ERR_NO_MEM;
    jpeg_enc_close(encoder);
    heap_caps_free(rgb);
    if (jpeg_err != JPEG_ERR_OK || jpeg_size <= 0) {
        heap_caps_free(jpeg);
        return room_canvas_set_error(
            out_error,
            "INTERNAL",
            jpeg_err == JPEG_ERR_NO_MEM
                ? "not enough memory for the JPEG snapshot"
                : "the JPEG encoder could not encode the snapshot",
            jpeg_err == JPEG_ERR_NO_MEM ? ESP_ERR_NO_MEM : ESP_FAIL);
    }

    size_t base64_len = 4 * (((size_t)jpeg_size + 2) / 3);
    char prefix[64] = {0};
    int prefix_len = snprintf(prefix, sizeof(prefix), "{\"format\":\"jpeg\",\"base64\":\"");
    char suffix[80] = {0};
    int suffix_len = snprintf(
        suffix,
        sizeof(suffix),
        "\",\"width\":%d,\"height\":%d}",
        width,
        height);
    size_t payload_size = (size_t)prefix_len + base64_len + (size_t)suffix_len + 1;
    char *payload = room_canvas_large_alloc(payload_size);
    if (payload == NULL) {
        heap_caps_free(jpeg);
        return room_canvas_set_error(out_error, "INTERNAL", "not enough memory for the base64 snapshot", ESP_ERR_NO_MEM);
    }
    memcpy(payload, prefix, (size_t)prefix_len);
    size_t written = 0;
    int base64_err = mbedtls_base64_encode(
        (unsigned char *)payload + prefix_len,
        base64_len + 1,
        &written,
        jpeg,
        (size_t)jpeg_size);
    heap_caps_free(jpeg);
    if (base64_err != 0 || written != base64_len) {
        heap_caps_free(payload);
        return room_canvas_set_error(out_error, "INTERNAL", "could not base64-encode the snapshot", ESP_FAIL);
    }
    memcpy(payload + prefix_len + written, suffix, (size_t)suffix_len);
    payload[prefix_len + written + suffix_len] = '\0';
    *out_payload_json = payload;
    return ESP_OK;
}
