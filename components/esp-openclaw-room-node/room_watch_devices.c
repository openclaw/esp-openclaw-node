#include "room_watch_devices.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PAGE_SIZE 3

static lv_obj_t *root, *back_text, *heading, *local, *message, *counter;
static lv_obj_t *refresh, *previous, *next, *rows[PAGE_SIZE], *names[PAGE_SIZE], *states[PAGE_SIZE];
static lv_obj_t *detail_name, *detail_model, *detail_connection, *detail_pairing;
static const room_devices_snapshot_t *devices;
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
    lv_obj_t *back = button(26, 18, 144, 44, ROOM_DEVICE_BACK, handler);
    back_text = label(back, LV_SYMBOL_LEFT " Menu", 10, 14, 124);
    refresh = button(228, 18, 114, 44, ROOM_DEVICE_REFRESH, handler);
    label(refresh, LV_SYMBOL_REFRESH " Refresh", 10, 14, 100);
    heading = label(root, "Devices", 26, 72, 316);
#if LV_FONT_MONTSERRAT_28
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_28, 0);
#endif
    local = button(26, 116, 316, 52, ROOM_DEVICE_LOCAL, handler);
    label(local, LV_SYMBOL_HOME "  This Watch", 18, 18, 280);
    for (unsigned i = 0; i < PAGE_SIZE; ++i) {
        rows[i] = button(26, 176 + (int)i * 48, 316, 44, (int)i, handler);
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
    lv_obj_set_hidden(root, true);
}

void room_watch_devices_reset(void)
{
    offset = 0;
    selected_id[0] = '\0';
    devices = NULL;
}

void room_watch_devices_render(const room_devices_snapshot_t *snapshot, bool hidden)
{
    if (root == NULL) return;
    devices = hidden ? NULL : snapshot;
    lv_obj_set_hidden(root, hidden);
    if (hidden) return;
    bool ready = devices != NULL && devices->state == ROOM_DEVICES_READY && devices->count > 0;
    const room_device_t *selected = NULL;
    for (size_t i = 0; ready && selected_id[0] != '\0' && i < devices->count; ++i)
        if (strcmp(devices->devices[i].id, selected_id) == 0) selected = &devices->devices[i];
    bool detail = selected != NULL;
    if (!detail) selected_id[0] = '\0';
    if (!ready || offset >= devices->count) offset = 0;
    bool loading = devices != NULL && devices->state == ROOM_DEVICES_LOADING;
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
            const room_device_t *device = &devices->devices[offset + i];
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
    } else if (ready) {
        size_t end = offset + PAGE_SIZE < devices->count ? offset + PAGE_SIZE : devices->count;
        lv_label_set_text_fmt(counter, "%u-%u / %u", (unsigned)offset + 1, (unsigned)end, (unsigned)devices->count);
    } else {
        const char *text = "Devices unavailable.\nTap Refresh to try again.";
        if (devices != NULL) switch (devices->state) {
            case ROOM_DEVICES_IDLE: text = "Devices paused.\nTap Refresh to load."; break;
            case ROOM_DEVICES_OFFLINE: text = "Gateway offline.\nThis Watch still works."; break;
            case ROOM_DEVICES_LOADING: text = "Finding your devices..."; break;
            case ROOM_DEVICES_EMPTY: text = "No Gateway devices yet.\nPair a device to see it here."; break;
            case ROOM_DEVICES_TOO_LARGE: text = "Too many devices for this view.\nOpen the mobile app."; break;
            case ROOM_DEVICES_TIMEOUT: text = "Gateway did not reply.\nTap Refresh to try again."; break;
            default: break;
        }
        lv_label_set_text_static(message, text);
    }
}

room_device_action_t room_watch_devices_activate(int action)
{
    if (action == ROOM_DEVICE_BACK) {
        if (selected_id[0] == '\0') return ROOM_DEVICE_LEAVE;
        selected_id[0] = '\0';
    } else if (action == ROOM_DEVICE_LOCAL && selected_id[0] == '\0') return ROOM_DEVICE_OPEN_LOCAL;
    else if (action == ROOM_DEVICE_REFRESH && selected_id[0] == '\0' &&
        (devices == NULL || devices->state != ROOM_DEVICES_LOADING)) {
        room_watch_devices_reset();
        return ROOM_DEVICE_RELOAD;
    } else if (devices != NULL && devices->state == ROOM_DEVICES_READY) {
        if (action == ROOM_DEVICE_PREVIOUS && offset >= PAGE_SIZE) offset -= PAGE_SIZE;
        else if (action == ROOM_DEVICE_NEXT && offset + PAGE_SIZE < devices->count) offset += PAGE_SIZE;
        else if (action >= 0 && action < PAGE_SIZE && offset + (size_t)action < devices->count)
            snprintf(selected_id, sizeof(selected_id), "%s", devices->devices[offset + (size_t)action].id);
    }
    return ROOM_DEVICE_STAY;
}
