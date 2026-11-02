#include "openclaw_density.h"
#include "room_watch_devices.h"
#include "room_tab5_layout.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PAGE_SIZE 3

static lv_obj_t *list, *root, *back, *back_text, *heading, *local, *message, *counter, *detail_body;
static lv_obj_t *refresh, *previous, *next, *rows[PAGE_SIZE], *names[PAGE_SIZE], *states[PAGE_SIZE];
static lv_obj_t *detail_name, *detail_model, *detail_connection, *detail_pairing;
static const esp_openclaw_devices_snapshot_t *devices;
static char selected_id[129];
static size_t offset;

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int width)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, width);
    lv_obj_set_style_text_font(obj, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(0xf4efee), 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
    lv_label_set_text(obj, text);
    lv_obj_set_clickable(obj, false);
    return obj;
}

static lv_obj_t *button(int x, int y, int width, int height, int action, lv_event_cb_t handler)
{
    lv_obj_t *obj = lv_obj_create(root);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x302535), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 14, 0);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, true);
    lv_obj_add_event_cb(obj, handler, LV_EVENT_ALL, (void *)(intptr_t)action);
    return obj;
}

void room_watch_devices_create(lv_obj_t *parent, lv_event_cb_t handler)
{
    root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollable(root, false);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x15101b), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    back = button(26, 18, 144, 44, ROOM_DEVICE_BACK, handler);
    back_text = label(back, LV_SYMBOL_LEFT " Menu", 10, 14, 124);
    refresh = button(228, 64, 114, 44, ROOM_DEVICE_REFRESH, handler);
    label(refresh, LV_SYMBOL_REFRESH " Refresh", 10, 14, 100);
    heading = label(root, "Devices", 26, 72, 190);
#if LV_FONT_MONTSERRAT_28
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_28, 0);
#endif
    local = button(26, 116, 316, 52, ROOM_DEVICE_LOCAL, handler);
    label(local, LV_SYMBOL_HOME "  This Watch", 18, 18, 280);
    list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 26, 176);
    lv_obj_set_size(list, 316, 140);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    for (unsigned i = 0; i < PAGE_SIZE; ++i) {
        rows[i] = button(26, 176 + (int)i * 48, 316, 44, (int)i, handler);
        lv_obj_set_parent(rows[i], list);
        names[i] = label(rows[i], "", 14, 14, 250);
        states[i] = label(rows[i], "", 274, 14, 24);
    }
    message = label(root, "", 32, 202, 304);
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    previous = button(26, 324, 66, 44, ROOM_DEVICE_PREVIOUS, handler);
    label(previous, LV_SYMBOL_LEFT, 26, 14, 24);
    counter = label(root, "", 102, 339, 164);
    lv_obj_set_style_text_align(counter, LV_TEXT_ALIGN_CENTER, 0);
    next = button(276, 324, 66, 44, ROOM_DEVICE_NEXT, handler);
    label(next, LV_SYMBOL_RIGHT, 26, 14, 24);
    detail_name = label(root, "", 32, 122, 304);
#if LV_FONT_MONTSERRAT_20
    lv_obj_set_style_text_font(detail_name, &lv_font_montserrat_20, 0);
#endif
    lv_obj_set_height(detail_name, 54);
    lv_label_set_long_mode(detail_name, LV_LABEL_LONG_WRAP);
    detail_model = label(root, "", 32, 190, 304);
    detail_connection = label(root, "", 32, 250, 304);
    detail_pairing = label(root, "", 32, 294, 304);
    if (room_tab5_layout()) {
        room_tab5_header(root, heading, back);
        lv_label_set_text_static(lv_obj_get_child(local, 0), LV_SYMBOL_HOME "  This Tab5");
        room_tab5_button(local, 160, 208, 624);
        room_tab5_button(refresh, 800, 208, 320);
        lv_obj_set_pos(list, 160, 320);
        lv_obj_set_width(list, 960);
        lv_obj_set_pos(message, 160, 336);
        lv_obj_set_width(message, 960);
        room_tab5_button(previous, 160, 560, 192);
        room_tab5_button(next, 928, 560, 192);
        lv_obj_set_pos(counter, 368, 590);
        lv_obj_set_width(counter, 544);
        detail_body = lv_obj_create(root);
        lv_obj_remove_style_all(detail_body);
        lv_obj_set_pos(detail_body, 160, 208);
        lv_obj_set_size(detail_body, 960, 448);
        lv_obj_set_scroll_dir(detail_body, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(detail_body, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_t *details[] = {detail_name, detail_model, detail_connection, detail_pairing};
        for (unsigned i = 0; i < 4; ++i) {
            lv_obj_set_parent(details[i], detail_body);
            lv_obj_set_width(details[i], 936);
            lv_obj_set_height(details[i], LV_SIZE_CONTENT);
            lv_label_set_long_mode(details[i], LV_LABEL_LONG_WRAP);
        }
    }
    lv_obj_set_hidden(root, true);
}

void room_watch_devices_reset(void)
{
    offset = 0;
    selected_id[0] = '\0';
    devices = NULL;
}

void room_watch_devices_render(const esp_openclaw_devices_snapshot_t *snapshot, bool hidden, int content_bottom)
{
    if (root == NULL) return;
    devices = hidden ? NULL : snapshot;
    lv_obj_set_hidden(root, hidden);
    if (hidden) return;
    bool ready = devices != NULL && devices->state == ESP_OPENCLAW_DEVICES_READY && devices->count > 0;
    const esp_openclaw_device_t *selected = NULL;
    for (size_t i = 0; ready && selected_id[0] != '\0' && i < devices->count; ++i)
        if (strcmp(devices->devices[i].id, selected_id) == 0) selected = &devices->devices[i];
    bool detail = selected != NULL;
    bool tab5 = room_tab5_layout();
    bool entering_detail = tab5 && detail && lv_obj_is_hidden(detail_body);
    int row_height = room_page_row(false);
    int list_top = tab5 ? 320 : 116 + row_height + 8;
    int list_height = content_bottom - (tab5 ? 112 : 56) - list_top;
    if (tab5 && row_height > list_height) row_height = list_height;
    lv_obj_set_height(local, tab5 ? 96 : row_height);
    lv_obj_set_y(lv_obj_get_child(local, 0), (row_height - 16) / 2);
    lv_obj_set_hidden(list, detail || !ready);
    lv_obj_set_y(list, list_top);
    lv_obj_set_height(list, list_height);
    lv_obj_set_y(previous, content_bottom - (tab5 ? 96 : 44));
    lv_obj_set_y(counter, content_bottom - (tab5 ? 66 : 29));
    lv_obj_set_y(next, content_bottom - (tab5 ? 96 : 44));
    for (unsigned i = 0; i < PAGE_SIZE; ++i) {
        lv_obj_set_pos(rows[i], 0, (int)i * (row_height + room_page_gap()));
        lv_obj_set_height(rows[i], row_height);
        lv_obj_set_y(names[i], (row_height - 16) / 2);
        lv_obj_set_y(states[i], (row_height - 16) / 2);
    }
    if (tab5) {
        room_tab5_header(root, heading, back);
        room_tab5_button(local, 160, 208, 624);
        lv_obj_set_height(local, tab5 ? 96 : row_height);
        room_tab5_button(refresh, 800, 208, 320);
        room_tab5_button(previous, 160, content_bottom - 96, 192);
        room_tab5_button(next, 928, content_bottom - 96, 192);
        lv_obj_set_style_text_font(message, room_page_font(), 0);
        lv_obj_set_style_text_font(counter, room_page_font(), 0);
        for (unsigned i = 0; i < PAGE_SIZE; ++i) {
            room_tab5_button(rows[i], 0, (int)i * (row_height + room_page_gap()), 960);
            lv_obj_set_height(rows[i], row_height);
            lv_obj_set_width(names[i], 824);
            lv_obj_set_pos(states[i], 888, 0);
            lv_obj_align(states[i], LV_ALIGN_RIGHT_MID, -24, 0);
            lv_obj_set_width(states[i], 40);
        }
        lv_obj_set_height(detail_body, content_bottom - 208);
        lv_obj_set_hidden(detail_body, !detail);
    }
    if (!detail) selected_id[0] = '\0';
    if (!ready || offset >= devices->count) offset = 0;
    bool loading = devices != NULL && devices->state == ESP_OPENCLAW_DEVICES_LOADING;
    lv_label_set_text_static(back_text, detail ? LV_SYMBOL_LEFT " Devices" : LV_SYMBOL_LEFT " Menu");
    lv_label_set_text_static(heading, detail ? "Device" : "Devices");
    lv_obj_set_hidden(local, detail);
    lv_obj_set_hidden(refresh, detail);
    lv_obj_set_state(refresh, LV_STATE_DISABLED, loading);
    lv_obj_set_hidden(message, detail || ready);
    lv_obj_set_hidden(previous, detail || !ready);
    lv_obj_set_hidden(next, detail || !ready);
    lv_obj_set_hidden(counter, detail || !ready);
    lv_obj_set_state(previous, LV_STATE_DISABLED, offset == 0);
    lv_obj_set_state(next, LV_STATE_DISABLED, !ready || offset + PAGE_SIZE >= devices->count);
    lv_obj_set_hidden(detail_name, !detail);
    lv_obj_set_hidden(detail_model, !detail);
    lv_obj_set_hidden(detail_connection, !detail);
    lv_obj_set_hidden(detail_pairing, !detail);
    for (size_t i = 0; i < PAGE_SIZE; ++i) {
        bool visible = !detail && ready && offset + i < devices->count;
        lv_obj_set_hidden(rows[i], !visible);
        if (visible) {
            const esp_openclaw_device_t *device = &devices->devices[offset + i];
            lv_label_set_text(names[i], device->name);
            lv_label_set_text_static(states[i], !device->connected_known ? "?" :
                device->connected ? LV_SYMBOL_OK : LV_SYMBOL_MINUS);
            lv_obj_set_style_text_color(states[i], lv_color_hex(device->connected_known &&
                device->connected ? 0x8bdfbf : 0xb9aabe), 0);
        }
    }
    if (detail) {
        lv_label_set_text(detail_name, selected->name);
        lv_label_set_text_fmt(detail_model, "Model: %s", selected->model[0] ? selected->model : "Unknown");
        lv_label_set_text_fmt(detail_connection, "Connection: %s", !selected->connected_known ? "Unknown" :
            selected->connected ? "Online" : "Offline");
        lv_label_set_text_fmt(detail_pairing, "Pairing: %s", !selected->paired_known ? "Unknown" :
            selected->paired ? "Paired" : "Not paired");
        if (tab5) {
            lv_obj_t *details[] = {detail_name, detail_model, detail_connection, detail_pairing};
            int y = 0;
            for (unsigned i = 0; i < 4; ++i) {
                lv_obj_set_style_text_font(details[i], room_page_font(), 0);
                lv_obj_set_pos(details[i], 0, y);
                lv_obj_update_layout(details[i]);
                y += lv_obj_get_height(details[i]) + 24;
            }
            if (entering_detail) lv_obj_scroll_to_y(detail_body, 0, LV_ANIM_OFF);
            lv_obj_readjust_scroll(detail_body, LV_ANIM_OFF);
        }
    } else if (ready) {
        size_t end = offset + PAGE_SIZE < devices->count ? offset + PAGE_SIZE : devices->count;
        lv_label_set_text_fmt(counter, "%u-%u / %u", (unsigned)offset + 1, (unsigned)end, (unsigned)devices->count);
    } else {
        const char *text = "Devices unavailable.\nTap Refresh to try again.";
        if (devices != NULL) switch (devices->state) {
            case ESP_OPENCLAW_DEVICES_IDLE: text = "Devices paused.\nTap Refresh to load."; break;
            case ESP_OPENCLAW_DEVICES_OFFLINE: text = tab5 ? "Device list offline.\nThis Tab5 still works." : "Gateway offline.\nThis Watch still works."; break;
            case ESP_OPENCLAW_DEVICES_LOADING: text = "Finding your devices..."; break;
            case ESP_OPENCLAW_DEVICES_EMPTY: text = "No Gateway devices yet.\nPair a device to see it here."; break;
            case ESP_OPENCLAW_DEVICES_TOO_LARGE: text = "Too many devices for this view.\nOpen the mobile app."; break;
            case ESP_OPENCLAW_DEVICES_TIMEOUT: text = "Gateway did not reply.\nTap Refresh to try again."; break;
            default: break;
        }
        lv_label_set_text_static(message, text);
    }
    if (!lv_obj_is_hidden(list)) {
        lv_obj_update_layout(list);
        lv_obj_readjust_scroll(list, LV_ANIM_OFF);
    }
}

room_device_action_t room_watch_devices_activate(int action)
{
    if (action == ROOM_DEVICE_BACK) {
        if (selected_id[0] == '\0') return ROOM_DEVICE_LEAVE;
        selected_id[0] = '\0';
    } else if (action == ROOM_DEVICE_LOCAL && selected_id[0] == '\0') return ROOM_DEVICE_OPEN_LOCAL;
    else if (action == ROOM_DEVICE_REFRESH && selected_id[0] == '\0' &&
        (devices == NULL || devices->state != ESP_OPENCLAW_DEVICES_LOADING)) {
        room_watch_devices_reset();
        return ROOM_DEVICE_RELOAD;
    } else if (devices != NULL && devices->state == ESP_OPENCLAW_DEVICES_READY) {
        if (action == ROOM_DEVICE_PREVIOUS && offset >= PAGE_SIZE) offset -= PAGE_SIZE;
        else if (action == ROOM_DEVICE_NEXT && offset + PAGE_SIZE < devices->count) offset += PAGE_SIZE;
        else if (action >= 0 && action < PAGE_SIZE && offset + (size_t)action < devices->count)
            snprintf(selected_id, sizeof(selected_id), "%s", devices->devices[offset + (size_t)action].id);
    }
    return ROOM_DEVICE_STAY;
}
