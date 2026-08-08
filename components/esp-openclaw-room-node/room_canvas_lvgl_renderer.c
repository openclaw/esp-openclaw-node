#include "room_canvas_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "room_board.h"

#define TAG "room_canvas_renderer"
#define ROOM_CANVAS_MAX_DEPTH 8
#define ROOM_CANVAS_MAX_RENDER_NODES 256
#define ROOM_CANVAS_DISPLAY_LOCK_MS 1000

static lv_obj_t *action_status_label;

static const lv_font_t *font_for_usage(const char *usage)
{
    if (usage != NULL && strcmp(usage, "h1") == 0) {
#if LV_FONT_MONTSERRAT_28
        return &lv_font_montserrat_28;
#elif LV_FONT_MONTSERRAT_24
        return &lv_font_montserrat_24;
#elif LV_FONT_MONTSERRAT_20
        return &lv_font_montserrat_20;
#elif LV_FONT_MONTSERRAT_16
        return &lv_font_montserrat_16;
#else
        return &lv_font_montserrat_14;
#endif
    }
    if (usage != NULL && strcmp(usage, "h2") == 0) {
#if LV_FONT_MONTSERRAT_24
        return &lv_font_montserrat_24;
#elif LV_FONT_MONTSERRAT_20
        return &lv_font_montserrat_20;
#elif LV_FONT_MONTSERRAT_16
        return &lv_font_montserrat_16;
#else
        return &lv_font_montserrat_14;
#endif
    }
    if (usage != NULL && strcmp(usage, "h3") == 0) {
#if LV_FONT_MONTSERRAT_20
        return &lv_font_montserrat_20;
#elif LV_FONT_MONTSERRAT_16
        return &lv_font_montserrat_16;
#else
        return &lv_font_montserrat_14;
#endif
    }
    if (usage != NULL && strcmp(usage, "caption") == 0) {
        return &lv_font_montserrat_14;
    }
#if LV_FONT_MONTSERRAT_16
    return &lv_font_montserrat_16;
#else
    return &lv_font_montserrat_14;
#endif
}

static lv_flex_align_t cross_alignment(const char *alignment)
{
    if (alignment != NULL && strcmp(alignment, "center") == 0) {
        return LV_FLEX_ALIGN_CENTER;
    }
    if (alignment != NULL && strcmp(alignment, "end") == 0) {
        return LV_FLEX_ALIGN_END;
    }
    return LV_FLEX_ALIGN_START;
}

static room_canvas_image_t *find_image(
    room_canvas_image_t *retained_room_canvas_images,
    size_t retained_room_canvas_image_count,
    const char *component_id)
{
    for (size_t i = 0; i < retained_room_canvas_image_count; ++i) {
        if (retained_room_canvas_images[i].component_id != NULL &&
            strcmp(retained_room_canvas_images[i].component_id, component_id) == 0) {
            return &retained_room_canvas_images[i];
        }
    }
    return NULL;
}

typedef struct {
    char id[65];
    char name[65];
    char surface[65];
    char component[65];
    char context[2049];
} room_canvas_touch_action_t;

static void show_action_status(const char *text)
{
    if (!room_board_display_lock(100)) return;
    if (action_status_label == NULL) {
        action_status_label = lv_label_create(lv_layer_top());
        if (action_status_label != NULL) {
            lv_obj_set_style_bg_color(action_status_label, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(action_status_label, LV_OPA_80, 0);
            lv_obj_set_style_text_color(action_status_label, lv_color_white(), 0);
            lv_obj_set_style_pad_all(action_status_label, 8, 0);
            lv_obj_align(action_status_label, LV_ALIGN_BOTTOM_MID, 0, -16);
        }
    }
    if (action_status_label != NULL) lv_label_set_text(action_status_label, text);
    room_board_display_unlock();
}

static void action_response(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_gateway_result_t *result,
    void *ctx)
{
    (void)node;
    free(ctx);
    show_action_status(result->ok ? "Action sent" : "Action failed");
}

static void sanitize_tag(const char *input, char *output, size_t size)
{
    size_t at = 0;
    for (const char *p = input; *p != '\0' && at + 1 < size; ++p) {
        char ch = *p == ' ' ? '_' : *p;
        bool allowed = isalnum((unsigned char)ch) || ch == '_' || ch == '-' || ch == '.' || ch == ':';
        output[at++] = allowed ? ch : '_';
    }
    if (at == 0 && size > 1) output[at++] = '-';
    output[at] = '\0';
}

static void button_event(lv_event_t *event)
{
    room_canvas_touch_action_t *action = lv_event_get_user_data(event);
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        if (action == NULL || action->name[0] == '\0' || room_canvas_node == NULL) {
            show_action_status("Action unavailable");
            return;
        }
        char session_key[ESP_OPENCLAW_NODE_MAX_SESSION_KEY_LEN + 1];
        if (!room_canvas_copy_owner_session(session_key, sizeof(session_key))) {
            show_action_status("Action has no session owner");
            return;
        }
        const esp_openclaw_room_node_config_t *board = room_board_config();
        char name[65], surface[65], component[65], host[65], instance[65], id[65];
        sanitize_tag(action->name, name, sizeof(name));
        sanitize_tag(action->surface, surface, sizeof(surface));
        sanitize_tag(action->component, component, sizeof(component));
        sanitize_tag(board != NULL ? board->display_name : "room-node", host, sizeof(host));
        sanitize_tag(esp_openclaw_node_get_device_id(room_canvas_node), instance, sizeof(instance));
        sanitize_tag(action->id, id, sizeof(id));
        char message[3072];
        snprintf(
            message, sizeof(message),
            "CANVAS_A2UI action=%s session=%s surface=%s component=%s host=%s instance=%s%s%s default=update_canvas",
            name, session_key, surface, component, host, instance,
            action->context[0] != '\0' ? " ctx=" : "",
            action->context);
        cJSON *payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "message", message);
        cJSON_AddStringToObject(payload, "sessionKey", session_key);
        cJSON_AddStringToObject(payload, "thinking", "low");
        cJSON_AddBoolToObject(payload, "deliver", false);
        cJSON_AddStringToObject(payload, "key", id);
        char *payload_json = cJSON_PrintUnformatted(payload);
        cJSON_Delete(payload);
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "event", "agent.request");
        cJSON_AddStringToObject(params, "payloadJSON", payload_json != NULL ? payload_json : "{}");
        char *request_json = cJSON_PrintUnformatted(params);
        cJSON_Delete(params);
        free(payload_json);
        char *callback_id = strdup(action->id);
        esp_err_t err = request_json != NULL
            ? esp_openclaw_node_gateway_request(
                room_canvas_node, "node.event", request_json, action_response, callback_id)
            : ESP_ERR_NO_MEM;
        free(request_json);
        if (err != ESP_OK) {
            free(callback_id);
            show_action_status("Action failed");
        } else {
            show_action_status("Sending action…");
        }
    } else if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        free(action);
    }
}

static lv_obj_t *render_component(
    const char *id,
    lv_obj_t *parent,
    room_canvas_image_t *render_room_canvas_images,
    size_t render_room_canvas_image_count,
    size_t depth,
    bool root,
    bool parent_row,
    size_t *node_budget,
    esp_openclaw_node_error_t *out_error);

static void apply_weight(lv_obj_t *object, const room_canvas_component_t *entry)
{
    if (entry->weight > 0) {
        uint32_t weight = entry->weight >= UINT32_MAX
            ? UINT32_MAX
            : (uint32_t)entry->weight;
        lv_obj_set_flex_grow(object, weight > 0 ? weight : 1);
    }
}

static void render_children(
    cJSON *props,
    lv_obj_t *parent,
    room_canvas_image_t *render_room_canvas_images,
    size_t render_room_canvas_image_count,
    size_t depth,
    bool parent_row,
    size_t *node_budget,
    esp_openclaw_node_error_t *out_error)
{
    cJSON *children = cJSON_GetObjectItemCaseSensitive(props, "children");
    cJSON *list = cJSON_IsObject(children)
        ? cJSON_GetObjectItemCaseSensitive(children, "explicitList")
        : NULL;
    cJSON *child = NULL;
    cJSON_ArrayForEach(child, list) {
        if (cJSON_IsString(child) && child->valuestring != NULL) {
            render_component(
                child->valuestring,
                parent,
                render_room_canvas_images,
                render_room_canvas_image_count,
                depth + 1,
                false,
                parent_row,
                node_budget,
                out_error);
            if (out_error->code != NULL) {
                return;
            }
        }
    }
}

static lv_obj_t *render_container(
    const room_canvas_component_t *entry,
    cJSON *props,
    lv_obj_t *parent,
    room_canvas_image_t *render_room_canvas_images,
    size_t render_room_canvas_image_count,
    size_t depth,
    bool root,
    bool row,
    size_t *node_budget,
    esp_openclaw_node_error_t *out_error)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(object, 4, 0);
    lv_obj_set_style_pad_gap(object, 8, 0);
    lv_obj_set_layout(object, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(object, row ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    cJSON *alignment = cJSON_GetObjectItemCaseSensitive(props, "alignment");
    const char *alignment_text = cJSON_IsString(alignment) ? alignment->valuestring : NULL;
    lv_obj_set_flex_align(
        object,
        LV_FLEX_ALIGN_START,
        cross_alignment(alignment_text),
        LV_FLEX_ALIGN_START);
    lv_obj_set_size(
        object,
        LV_PCT(100),
        root ? LV_PCT(100) : LV_SIZE_CONTENT);
    apply_weight(object, entry);
    render_children(
        props,
        object,
        render_room_canvas_images,
        render_room_canvas_image_count,
        depth,
        row,
        node_budget,
        out_error);
    if (alignment_text != NULL && strcmp(alignment_text, "stretch") == 0) {
        uint32_t child_count = lv_obj_get_child_count(object);
        for (uint32_t i = 0; i < child_count; ++i) {
            lv_obj_t *child = lv_obj_get_child(object, (int32_t)i);
            if (row) {
                lv_obj_set_height(child, LV_PCT(100));
            } else {
                lv_obj_set_width(child, LV_PCT(100));
            }
        }
    }
    return object;
}

static lv_obj_t *render_component(
    const char *id,
    lv_obj_t *parent,
    room_canvas_image_t *render_room_canvas_images,
    size_t render_room_canvas_image_count,
    size_t depth,
    bool root,
    bool parent_row,
    size_t *node_budget,
    esp_openclaw_node_error_t *out_error)
{
    if (*node_budget == 0) {
        room_canvas_set_error(
            out_error,
            "INVALID_PARAMS",
            "A2UI graph expands past the 256-node bound",
            ESP_ERR_INVALID_SIZE);
        return NULL;
    }
    --*node_budget;
    if (depth > ROOM_CANVAS_MAX_DEPTH) {
        room_canvas_set_error(
            out_error,
            "INVALID_PARAMS",
            "A2UI component nesting exceeds the 8-level bound",
            ESP_ERR_INVALID_SIZE);
        return NULL;
    }
    room_canvas_component_t *entry = room_canvas_find_component(id);
    if (entry == NULL) {
        lv_obj_t *missing = lv_label_create(parent);
        lv_label_set_text_fmt(missing, "[missing:%s]", id);
        return missing;
    }
    cJSON *type = room_canvas_component_type_item(entry);
    if (type == NULL || type->string == NULL || !cJSON_IsObject(type)) {
        lv_obj_t *invalid = lv_label_create(parent);
        lv_label_set_text(invalid, "[Invalid]");
        return invalid;
    }
    const char *name = type->string;
    cJSON *props = type;

    if (strcmp(name, "Column") == 0 || strcmp(name, "List") == 0) {
        return render_container(
            entry,
            props,
            parent,
            render_room_canvas_images,
            render_room_canvas_image_count,
            depth,
            root,
            false,
            node_budget,
            out_error);
    }
    if (strcmp(name, "Row") == 0) {
        return render_container(
            entry,
            props,
            parent,
            render_room_canvas_images,
            render_room_canvas_image_count,
            depth,
            root,
            true,
            node_budget,
            out_error);
    }
    if (strcmp(name, "Text") == 0) {
        lv_obj_t *label = lv_label_create(parent);
        cJSON *text = cJSON_GetObjectItemCaseSensitive(props, "text");
        cJSON *usage = cJSON_GetObjectItemCaseSensitive(props, "usageHint");
        char text_buffer[32] = {0};
        lv_label_set_text(label, room_canvas_resolve_text(text, text_buffer, sizeof(text_buffer)));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(label, parent_row ? LV_SIZE_CONTENT : LV_PCT(100));
        lv_obj_set_style_text_font(
            label,
            font_for_usage(cJSON_IsString(usage) ? usage->valuestring : "body"),
            0);
        apply_weight(label, entry);
        return label;
    }
    if (strcmp(name, "Image") == 0) {
        room_canvas_image_t *image = find_image(
            render_room_canvas_images,
            render_room_canvas_image_count,
            id);
        if (image == NULL) {
            lv_obj_t *missing = lv_label_create(parent);
            lv_label_set_text(missing, "[Image]");
            return missing;
        }
        lv_obj_t *object = lv_image_create(parent);
        lv_image_set_src(object, image->decoded);
        room_canvas_configure_image_object(object, parent, image, root);
        apply_weight(object, entry);
        return object;
    }
    if (strcmp(name, "Button") == 0) {
        lv_obj_t *button = lv_button_create(parent);
        lv_obj_t *label = lv_label_create(button);
        cJSON *label_value = cJSON_GetObjectItemCaseSensitive(props, "label");
        char label_buffer[32] = {0};
        lv_label_set_text(
            label,
            room_canvas_resolve_text(label_value, label_buffer, sizeof(label_buffer)));
        lv_obj_center(label);
        cJSON *action = cJSON_GetObjectItemCaseSensitive(props, "action");
        cJSON *action_name = cJSON_IsObject(action)
            ? cJSON_GetObjectItemCaseSensitive(action, "name")
            : NULL;
        cJSON *action_id = cJSON_IsObject(action)
            ? cJSON_GetObjectItemCaseSensitive(action, "id") : NULL;
        cJSON *context = cJSON_IsObject(action)
            ? cJSON_GetObjectItemCaseSensitive(action, "context") : NULL;
        char *context_json = cJSON_IsObject(context) ? cJSON_PrintUnformatted(context) : NULL;
        room_canvas_touch_action_t *action_data = calloc(1, sizeof(*action_data));
        if (action_data != NULL && cJSON_IsString(action_name) && action_name->valuestring != NULL &&
            strlen(action_name->valuestring) <= 64 && strlen(id) <= 64 &&
            (room_canvas_surface_id == NULL || strlen(room_canvas_surface_id) <= 64) &&
            (context_json == NULL || strlen(context_json) <= 2048)) {
            strlcpy(action_data->name, action_name->valuestring, sizeof(action_data->name));
            strlcpy(action_data->component, id, sizeof(action_data->component));
            strlcpy(action_data->surface, room_canvas_surface_id != NULL ? room_canvas_surface_id : "main", sizeof(action_data->surface));
            strlcpy(action_data->id,
                cJSON_IsString(action_id) && action_id->valuestring != NULL && strlen(action_id->valuestring) <= 64
                    ? action_id->valuestring : id,
                sizeof(action_data->id));
            if (context_json != NULL) strlcpy(action_data->context, context_json, sizeof(action_data->context));
        }
        free(context_json);
        lv_obj_add_event_cb(button, button_event, LV_EVENT_ALL, action_data);
        apply_weight(button, entry);
        return button;
    }
    if (strcmp(name, "Card") == 0) {
        lv_obj_t *card = lv_obj_create(parent);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x151515), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(card, lv_color_white(), 0);
        lv_obj_set_style_pad_all(card, 12, 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x3a3a3a), 0);
        cJSON *child = cJSON_GetObjectItemCaseSensitive(props, "child");
        if (cJSON_IsString(child) && child->valuestring != NULL) {
            render_component(
                child->valuestring,
                card,
                render_room_canvas_images,
                render_room_canvas_image_count,
                depth + 1,
                false,
                false,
                node_budget,
                out_error);
        }
        apply_weight(card, entry);
        return card;
    }
    if (strcmp(name, "Divider") == 0) {
        lv_obj_t *divider = lv_obj_create(parent);
        lv_obj_remove_style_all(divider);
        lv_obj_set_size(divider, LV_PCT(100), 1);
        lv_obj_set_style_bg_color(divider, lv_color_hex(0x666666), 0);
        lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
        return divider;
    }
    if (strcmp(name, "CheckBox") == 0) {
        lv_obj_t *checkbox = lv_checkbox_create(parent);
        cJSON *label = cJSON_GetObjectItemCaseSensitive(props, "label");
        cJSON *value = cJSON_GetObjectItemCaseSensitive(props, "value");
        char label_buffer[32] = {0};
        lv_checkbox_set_text(
            checkbox,
            room_canvas_resolve_text(label, label_buffer, sizeof(label_buffer)));
        if (room_canvas_resolve_boolean(value)) {
            lv_obj_add_state(checkbox, LV_STATE_CHECKED);
        }
        lv_obj_remove_flag(checkbox, LV_OBJ_FLAG_CLICKABLE);
        apply_weight(checkbox, entry);
        return checkbox;
    }

    lv_obj_t *unknown = lv_label_create(parent);
    lv_label_set_text_fmt(unknown, "[%s]", name);
    lv_obj_set_style_text_font(unknown, font_for_usage("caption"), 0);
    return unknown;
}

static esp_err_t prefetch_a2ui_room_canvas_images(esp_openclaw_node_error_t *out_error)
{
    if (room_canvas_root_component_id == NULL) {
        if (!room_board_display_lock(ROOM_CANVAS_DISPLAY_LOCK_MS)) {
            return room_canvas_set_error(
                out_error,
                "UNAVAILABLE",
                "the display is busy; retry the A2UI command",
                ESP_ERR_TIMEOUT);
        }
        room_canvas_show_placeholder();
        room_canvas_record_no_retained_content();
        room_canvas_activate_locked();
        room_board_display_unlock();
        room_board_display_brightness_set(ROOM_CANVAS_ACTIVE_BRIGHTNESS);
        room_canvas_emit_action(ROOM_CANVAS_ACTION_RENDER_CHANGED, 0);
        return ESP_OK;
    }

    room_canvas_image_t fetched[ROOM_CANVAS_MAX_IMAGES] = {0};
    size_t fetched_count = 0;
    for (size_t i = 0; i < room_canvas_component_count; ++i) {
        if (!room_canvas_component_is_image(&room_canvas_components[i])) {
            continue;
        }
        cJSON *type = room_canvas_component_type_item(&room_canvas_components[i]);
        cJSON *url = cJSON_GetObjectItemCaseSensitive(type, "url");
        const char *url_text = room_canvas_resolve_string(url);
        if (url_text[0] == '\0') {
            room_canvas_release_image_array(fetched, ROOM_CANVAS_MAX_IMAGES);
            return room_canvas_set_error(
                out_error,
                "INVALID_PARAMS",
                "A2UI Image url must resolve to a non-empty string",
                ESP_ERR_INVALID_ARG);
        }
        esp_err_t err = room_canvas_fetch_image(url_text, &fetched[fetched_count], out_error);
        if (err != ESP_OK) {
            room_canvas_release_image_array(fetched, ROOM_CANVAS_MAX_IMAGES);
            return err;
        }
        fetched[fetched_count].component_id = strdup(room_canvas_components[i].id);
        if (fetched[fetched_count].component_id == NULL) {
            room_canvas_release_image_array(fetched, ROOM_CANVAS_MAX_IMAGES);
            return room_canvas_set_error(
                out_error,
                "INTERNAL",
                "not enough memory for the A2UI image store",
                ESP_ERR_NO_MEM);
        }
        ++fetched_count;
    }

    for (size_t i = 0; i < fetched_count; ++i) {
        if (!room_canvas_validate_image(&fetched[i])) {
            room_canvas_release_image_array(fetched, ROOM_CANVAS_MAX_IMAGES);
            return room_canvas_set_error(
                out_error,
                "DECODE_FAILED",
                "LVGL could not decode an A2UI Image; room_canvas_images are bounded to 2048 px per side and 1 megapixel",
                ESP_ERR_INVALID_RESPONSE);
        }
    }

    if (!room_board_display_lock(ROOM_CANVAS_DISPLAY_LOCK_MS)) {
        room_canvas_release_image_array(fetched, ROOM_CANVAS_MAX_IMAGES);
        return room_canvas_set_error(
            out_error,
            "UNAVAILABLE",
            "the display is busy; retry the A2UI command",
            ESP_ERR_TIMEOUT);
    }

    lv_obj_t *container = lv_obj_create(room_canvas_screen);
    if (container == NULL) {
        room_canvas_release_image_array(fetched, ROOM_CANVAS_MAX_IMAGES);
        room_board_display_unlock();
        return room_canvas_set_error(
            out_error,
            "INTERNAL",
            "not enough memory to stage the A2UI surface",
            ESP_ERR_NO_MEM);
    }
    /* This hidden transaction root has exactly the canvas content geometry,
     * so an LV_PCT(100) root renders as it did directly on room_canvas_screen. */
    lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_style_all(container);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_layout(container, LV_LAYOUT_NONE);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_size(container, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(container, LV_SCROLLBAR_MODE_OFF);
    /* Background taps must reach the screen's tap-to-hide handler; A2UI
     * buttons stay individually clickable. */
    lv_obj_remove_flag(container, LV_OBJ_FLAG_CLICKABLE);

    esp_openclaw_node_error_t render_error = {0};
    size_t node_budget = ROOM_CANVAS_MAX_RENDER_NODES;
    lv_obj_t *root = render_component(
        room_canvas_root_component_id,
        container,
        fetched,
        fetched_count,
        1,
        true,
        false,
        &node_budget,
        &render_error);
    if (root == NULL || render_error.code != NULL) {
        lv_obj_delete(container);
        room_canvas_release_image_array(fetched, ROOM_CANVAS_MAX_IMAGES);
        if (render_error.code != NULL) {
            *out_error = render_error;
        } else {
            room_canvas_set_error(
                out_error,
                "INTERNAL",
                "not enough memory to render the A2UI surface",
                ESP_ERR_NO_MEM);
        }
        room_board_display_unlock();
        return render_error.code != NULL ? ESP_ERR_INVALID_SIZE : ESP_ERR_NO_MEM;
    }

    uint32_t child_count = lv_obj_get_child_count(room_canvas_screen);
    for (uint32_t i = child_count; i > 0; --i) {
        lv_obj_t *child = lv_obj_get_child(room_canvas_screen, (int32_t)i - 1);
        if (child != container) {
            lv_obj_delete(child);
        }
    }
    room_canvas_clear_images_locked();
    for (size_t i = 0; i < fetched_count; ++i) {
        /* LVGL retains the heap draw-buffer pointer, so moving its owning
         * record to stable storage does not invalidate the staged widget. */
        room_canvas_images[i] = fetched[i];
        memset(&fetched[i], 0, sizeof(fetched[i]));
    }
    room_canvas_image_count = fetched_count;
    /* The image path runs full-bleed (pad 0); restore the safe-area inset for
     * laid-out content. Only on success, so a failed render leaves the screen
     * exactly as it was. */
    room_canvas_style_screen(room_canvas_safe_pad());
    lv_obj_remove_flag(container, LV_OBJ_FLAG_HIDDEN);
    /* The successfully staged tree is now the locally retained canvas. */
    room_canvas_record_a2ui_retained();
    room_canvas_activate_locked();
    room_board_display_unlock();
    room_board_display_brightness_set(ROOM_CANVAS_ACTIVE_BRIGHTNESS);
    room_canvas_emit_action(ROOM_CANVAS_ACTION_RENDER_CHANGED, 0);
    return ESP_OK;
}

esp_err_t room_canvas_render_a2ui_result(
    bool force_active,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    bool shown = force_active || room_canvas_active;
    if (shown) {
        esp_err_t err = prefetch_a2ui_room_canvas_images(out_error);
        if (err != ESP_OK) {
            return err;
        }
    }
    char payload[64];
    snprintf(
        payload,
        sizeof(payload),
        "{\"shown\":%s,\"kind\":\"a2ui\",\"components\":%u}",
        shown ? "true" : "false",
        (unsigned)room_canvas_component_count);
    *out_payload_json = strdup(payload);
    if (*out_payload_json == NULL) {
        return room_canvas_set_error(out_error, "INTERNAL", "not enough memory for the command result", ESP_ERR_NO_MEM);
    }
    return ESP_OK;
}
