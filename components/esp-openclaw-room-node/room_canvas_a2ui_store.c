#include "room_canvas.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "room_board.h"
#include "room_canvas_internal.h"
#include "room_diagnostics.h"
#include "freertos/FreeRTOS.h"

#define set_error room_canvas_set_error
#define large_alloc room_canvas_large_alloc
#define large_aligned_alloc room_canvas_large_aligned_alloc

#define TAG "room_canvas"
#define ROOM_CANVAS_MAX_JSONL_BYTES (256 * 1024)
#define ROOM_CANVAS_MAX_STORE_BYTES (256 * 1024)
#define ROOM_CANVAS_MAX_DEPTH 8
#define ROOM_CANVAS_MAX_RENDER_NODES 256
#define ROOM_CANVAS_DISPLAY_LOCK_MS 1000

typedef struct {
    room_canvas_component_t components[ROOM_CANVAS_MAX_COMPONENTS];
    size_t component_count;
    cJSON *data_model;
    char *surface_id;
    char *root_component_id;
} room_canvas_store_snapshot_t;

static lv_obj_t *status_screen;
lv_obj_t *room_canvas_screen;
#define canvas_screen room_canvas_screen
volatile bool room_canvas_active;
#define canvas_active room_canvas_active
esp_openclaw_node_handle_t room_canvas_node;
#define canvas_node room_canvas_node
char *room_canvas_gateway_http_base;
#define gateway_http_base room_canvas_gateway_http_base
room_canvas_component_t room_canvas_components[ROOM_CANVAS_MAX_COMPONENTS];
size_t room_canvas_component_count;
cJSON *room_canvas_data_model;
char *room_canvas_surface_id;
char *room_canvas_root_component_id;
room_canvas_image_t room_canvas_images[ROOM_CANVAS_MAX_IMAGES];
size_t room_canvas_image_count;
#define components room_canvas_components
#define component_count room_canvas_component_count
#define data_model room_canvas_data_model
#define surface_id room_canvas_surface_id
#define root_component_id room_canvas_root_component_id
#define images room_canvas_images
#define image_count room_canvas_image_count
static room_canvas_action_handler_t action_handler;
static portMUX_TYPE owner_session_mux = portMUX_INITIALIZER_UNLOCKED;
static char owner_session_key[ESP_OPENCLAW_NODE_MAX_SESSION_KEY_LEN + 1];
static portMUX_TYPE diagnostics_mux = portMUX_INITIALIZER_UNLOCKED;
static room_canvas_diagnostics_snapshot_t diagnostics_snapshot;

static void record_retained_kind(room_canvas_retained_kind_t kind)
{
    taskENTER_CRITICAL(&diagnostics_mux);
    diagnostics_snapshot.retained_kind = kind;
    diagnostics_snapshot.retained_components = kind == ROOM_CANVAS_RETAINED_A2UI
        ? (uint16_t)component_count
        : 0;
    diagnostics_snapshot.retained_images = (uint8_t)image_count;
    taskEXIT_CRITICAL(&diagnostics_mux);
}

void room_canvas_record_a2ui_retained(void)
{
    record_retained_kind(ROOM_CANVAS_RETAINED_A2UI);
}

void room_canvas_record_no_retained_content(void)
{
    record_retained_kind(ROOM_CANVAS_RETAINED_NONE);
}

void room_canvas_bind_owner_session(const char *session_key)
{
    portENTER_CRITICAL(&owner_session_mux);
    if (session_key == NULL) {
        owner_session_key[0] = '\0';
    } else {
        size_t len = strnlen(session_key, sizeof(owner_session_key) - 1U);
        memcpy(owner_session_key, session_key, len);
        owner_session_key[len] = '\0';
    }
    portEXIT_CRITICAL(&owner_session_mux);
}

bool room_canvas_copy_owner_session(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) return false;
    portENTER_CRITICAL(&owner_session_mux);
    size_t len = strnlen(owner_session_key, sizeof(owner_session_key));
    if (len >= buffer_size) len = buffer_size - 1U;
    memcpy(buffer, owner_session_key, len);
    buffer[len] = '\0';
    portEXIT_CRITICAL(&owner_session_mux);
    return buffer[0] != '\0';
}

void room_canvas_set_action_handler(room_canvas_action_handler_t handler)
{
    action_handler = handler;
}

void room_canvas_emit_action(room_canvas_action_t action, uint32_t value)
{
    if (action_handler != NULL) action_handler(action, value);
}

#define fetch_image room_canvas_fetch_image
#define release_image room_canvas_release_image
#define release_image_array room_canvas_release_image_array
#define validate_image room_canvas_validate_image
#define find_component room_canvas_find_component
#define component_type_item room_canvas_component_type_item
#define component_is_image room_canvas_component_is_image
#define resolve_string room_canvas_resolve_string
#define resolve_text room_canvas_resolve_text
#define resolve_boolean room_canvas_resolve_boolean
#define canvas_safe_pad room_canvas_safe_pad
#define canvas_width room_canvas_width
#define canvas_height room_canvas_height
#define style_canvas_screen room_canvas_style_screen
#define show_placeholder_locked room_canvas_show_placeholder
#define activate_canvas_locked room_canvas_activate_locked
#define clear_images_locked room_canvas_clear_images_locked
#define render_a2ui_result room_canvas_render_a2ui_result
#define configure_image_object room_canvas_configure_image_object

esp_err_t room_canvas_set_error(
    esp_openclaw_node_error_t *out_error,
    const char *code,
    const char *message,
    esp_err_t err)
{
    out_error->code = code;
    out_error->message = message;
    return err;
}

void *room_canvas_large_alloc(size_t size)
{
    void *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return buffer;
}

void *room_canvas_large_aligned_alloc(size_t alignment, size_t size)
{
    void *buffer = heap_caps_aligned_alloc(
        alignment,
        size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = heap_caps_aligned_alloc(alignment, size, MALLOC_CAP_8BIT);
    }
    return buffer;
}

void room_canvas_clear_images_locked(void)
{
    release_image_array(images, ROOM_CANVAS_MAX_IMAGES);
    image_count = 0;
}

int room_canvas_width(void)
{
    return lv_display_get_horizontal_resolution(lv_display_get_default());
}

int room_canvas_height(void)
{
    return lv_display_get_vertical_resolution(lv_display_get_default());
}

int room_canvas_safe_pad(void)
{
    const esp_openclaw_room_node_config_t *board = room_board_config();
    return board != NULL ? board->display.safe_inset : 0;
}

void room_canvas_style_screen(int32_t pad)
{
    lv_obj_set_style_bg_color(canvas_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(canvas_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(canvas_screen, lv_color_white(), 0);
    lv_obj_set_style_pad_all(canvas_screen, pad, 0);
    lv_obj_remove_flag(canvas_screen, LV_OBJ_FLAG_SCROLLABLE);
}

void room_canvas_show_placeholder(void)
{
    lv_obj_clean(canvas_screen);
    clear_images_locked();
    style_canvas_screen(canvas_safe_pad());
    lv_obj_t *label = lv_label_create(canvas_screen);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
#if LV_FONT_MONTSERRAT_20
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
#elif LV_FONT_MONTSERRAT_16
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
#else
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
#endif
    lv_label_set_text(label, "Canvas ready");
    lv_obj_center(label);
}

void room_canvas_activate_locked(void)
{
    lv_screen_load(canvas_screen);
    canvas_active = true;
    taskENTER_CRITICAL(&diagnostics_mux);
    diagnostics_snapshot.active = true;
    taskEXIT_CRITICAL(&diagnostics_mux);
}

void room_canvas_configure_image_object(
    lv_obj_t *object,
    lv_obj_t *parent,
    const room_canvas_image_t *image,
    bool fullscreen)
{
    lv_image_set_inner_align(object, LV_IMAGE_ALIGN_CONTAIN);
    if (fullscreen) {
        lv_obj_set_size(object, canvas_width(), canvas_height());
    } else {
        lv_obj_update_layout(parent);
        int width = lv_obj_get_content_width(parent);
        if (width <= 0 || width > canvas_width() - 24) {
            width = canvas_width() - 24;
        }
        int height = image->width > 0
            ? (int)(((uint64_t)width * image->height) / image->width)
            : canvas_height() / 3;
        if (height <= 0 || height > canvas_height() / 3) {
            height = canvas_height() / 3;
        }
        lv_obj_set_size(object, LV_PCT(100), height);
    }
    lv_obj_set_align(object, LV_ALIGN_CENTER);
}

static esp_err_t show_present_image(
    room_canvas_image_t *image,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    if (!validate_image(image)) {
        release_image(image);
        return set_error(
            out_error,
            "DECODE_FAILED",
            "LVGL could not decode the PNG or JPEG; images are bounded to 2048 px per side and 1 megapixel",
            ESP_ERR_INVALID_RESPONSE);
    }

    if (!room_board_display_lock(ROOM_CANVAS_DISPLAY_LOCK_MS)) {
        release_image(image);
        return set_error(
            out_error,
            "UNAVAILABLE",
            "the display is busy; retry canvas.present",
            ESP_ERR_TIMEOUT);
    }

    lv_obj_clean(canvas_screen);
    clear_images_locked();
    style_canvas_screen(0);
    images[0] = *image;
    memset(image, 0, sizeof(*image));
    image_count = 1;
    lv_obj_t *object = lv_image_create(canvas_screen);
    lv_image_set_src(object, images[0].decoded);
    configure_image_object(object, canvas_screen, &images[0], true);
    record_retained_kind(ROOM_CANVAS_RETAINED_IMAGE);
    activate_canvas_locked();
    room_board_display_unlock();
    room_board_display_brightness_set(ROOM_CANVAS_ACTIVE_BRIGHTNESS);
    room_canvas_emit_action(ROOM_CANVAS_ACTION_RENDER_CHANGED, 0);

    if (asprintf(
            out_payload_json,
            "{\"shown\":true,\"kind\":\"image\",\"width\":%d,\"height\":%d}",
            canvas_width(),
            canvas_height()) < 0) {
        *out_payload_json = NULL;
    }
    if (*out_payload_json == NULL) {
        return set_error(out_error, "INTERNAL", "not enough memory for the command result", ESP_ERR_NO_MEM);
    }
    return ESP_OK;
}

static void clear_store(void)
{
    for (size_t i = 0; i < component_count; ++i) {
        free(components[i].id);
        cJSON_Delete(components[i].component);
        memset(&components[i], 0, sizeof(components[i]));
    }
    component_count = 0;
    cJSON_Delete(data_model);
    data_model = NULL;
    free(surface_id);
    surface_id = NULL;
    free(root_component_id);
    root_component_id = NULL;
}

#undef components
#undef component_count
#undef data_model
#undef surface_id
#undef root_component_id

static void free_store_snapshot(room_canvas_store_snapshot_t *snapshot)
{
    for (size_t i = 0; i < snapshot->component_count; ++i) {
        free(snapshot->components[i].id);
        cJSON_Delete(snapshot->components[i].component);
    }
    cJSON_Delete(snapshot->data_model);
    free(snapshot->surface_id);
    free(snapshot->root_component_id);
    memset(snapshot, 0, sizeof(*snapshot));
}

static esp_err_t copy_store_snapshot(room_canvas_store_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->component_count = room_canvas_component_count;
    for (size_t i = 0; i < room_canvas_component_count; ++i) {
        snapshot->components[i].id = strdup(room_canvas_components[i].id);
        snapshot->components[i].component = cJSON_Duplicate(room_canvas_components[i].component, true);
        snapshot->components[i].weight = room_canvas_components[i].weight;
        if (snapshot->components[i].id == NULL ||
            snapshot->components[i].component == NULL) {
            free_store_snapshot(snapshot);
            return ESP_ERR_NO_MEM;
        }
    }
    snapshot->data_model = room_canvas_data_model != NULL ? cJSON_Duplicate(room_canvas_data_model, true) : NULL;
    snapshot->surface_id = room_canvas_surface_id != NULL ? strdup(room_canvas_surface_id) : NULL;
    snapshot->root_component_id = room_canvas_root_component_id != NULL
        ? strdup(room_canvas_root_component_id)
        : NULL;
    if ((room_canvas_data_model != NULL && snapshot->data_model == NULL) ||
        (room_canvas_surface_id != NULL && snapshot->surface_id == NULL) ||
        (room_canvas_root_component_id != NULL && snapshot->root_component_id == NULL)) {
        free_store_snapshot(snapshot);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void restore_store_snapshot(room_canvas_store_snapshot_t *snapshot)
{
    clear_store();
    room_canvas_component_count = snapshot->component_count;
    for (size_t i = 0; i < room_canvas_component_count; ++i) {
        room_canvas_components[i] = snapshot->components[i];
        memset(&snapshot->components[i], 0, sizeof(snapshot->components[i]));
    }
    room_canvas_data_model = snapshot->data_model;
    room_canvas_surface_id = snapshot->surface_id;
    room_canvas_root_component_id = snapshot->root_component_id;
    memset(snapshot, 0, sizeof(*snapshot));
}

#define components room_canvas_components
#define component_count room_canvas_component_count
#define data_model room_canvas_data_model
#define surface_id room_canvas_surface_id
#define root_component_id room_canvas_root_component_id

room_canvas_component_t *room_canvas_find_component(const char *id)
{
    for (size_t i = 0; i < component_count; ++i) {
        if (strcmp(components[i].id, id) == 0) {
            return &components[i];
        }
    }
    return NULL;
}

cJSON *room_canvas_component_type_item(const room_canvas_component_t *entry)
{
    return cJSON_IsObject(entry->component) ? entry->component->child : NULL;
}

bool room_canvas_component_is_image(const room_canvas_component_t *entry)
{
    cJSON *type = component_type_item(entry);
    return type != NULL && type->string != NULL && strcmp(type->string, "Image") == 0;
}

static esp_err_t validate_component_depth(
    const char *id,
    size_t depth,
    size_t *node_budget,
    esp_openclaw_node_error_t *out_error)
{
    if (*node_budget == 0) {
        return set_error(
            out_error,
            "INVALID_PARAMS",
            "A2UI graph expands past the 256-node bound",
            ESP_ERR_INVALID_SIZE);
    }
    --*node_budget;
    if (depth > ROOM_CANVAS_MAX_DEPTH) {
        return set_error(
            out_error,
            "INVALID_PARAMS",
            "A2UI component nesting exceeds the 8-level bound",
            ESP_ERR_INVALID_SIZE);
    }
    room_canvas_component_t *entry = find_component(id);
    cJSON *type = entry != NULL ? component_type_item(entry) : NULL;
    if (type == NULL || type->string == NULL || !cJSON_IsObject(type)) {
        return ESP_OK;
    }
    if (strcmp(type->string, "Card") == 0) {
        cJSON *child = cJSON_GetObjectItemCaseSensitive(type, "child");
        return cJSON_IsString(child) && child->valuestring != NULL
            ? validate_component_depth(
                child->valuestring,
                depth + 1,
                node_budget,
                out_error)
            : ESP_OK;
    }
    if (strcmp(type->string, "Column") != 0 &&
        strcmp(type->string, "Row") != 0 &&
        strcmp(type->string, "List") != 0) {
        return ESP_OK;
    }
    cJSON *children = cJSON_GetObjectItemCaseSensitive(type, "children");
    cJSON *list = cJSON_IsObject(children)
        ? cJSON_GetObjectItemCaseSensitive(children, "explicitList")
        : NULL;
    cJSON *child = NULL;
    cJSON_ArrayForEach(child, list) {
        if (cJSON_IsString(child) && child->valuestring != NULL) {
            esp_err_t err = validate_component_depth(
                child->valuestring,
                depth + 1,
                node_budget,
                out_error);
            if (err != ESP_OK) {
                return err;
            }
        }
    }
    return ESP_OK;
}

static size_t count_images_in_store(void)
{
    size_t count = 0;
    for (size_t i = 0; i < component_count; ++i) {
        count += component_is_image(&components[i]) ? 1 : 0;
    }
    return count;
}

static char *decode_json_pointer_token(const char *start, size_t len)
{
    char *token = malloc(len + 1);
    if (token == NULL) {
        return NULL;
    }
    size_t written = 0;
    for (size_t i = 0; i < len; ++i) {
        if (start[i] == '~' && i + 1 < len) {
            if (start[i + 1] == '0') {
                token[written++] = '~';
                ++i;
                continue;
            }
            if (start[i + 1] == '1') {
                token[written++] = '/';
                ++i;
                continue;
            }
        }
        token[written++] = start[i];
    }
    token[written] = '\0';
    return token;
}

static cJSON *resolve_data_path(const char *path)
{
    if (data_model == NULL || path == NULL) {
        return NULL;
    }
    if (path[0] == '\0' || strcmp(path, "/") == 0) {
        return data_model;
    }
    if (path[0] != '/') {
        return NULL;
    }

    cJSON *current = data_model;
    const char *cursor = path + 1;
    while (*cursor != '\0') {
        const char *slash = strchr(cursor, '/');
        size_t len = slash != NULL ? (size_t)(slash - cursor) : strlen(cursor);
        char *token = decode_json_pointer_token(cursor, len);
        if (token == NULL) {
            return NULL;
        }
        if (cJSON_IsObject(current)) {
            current = cJSON_GetObjectItemCaseSensitive(current, token);
        } else if (cJSON_IsArray(current)) {
            char *end = NULL;
            long index = strtol(token, &end, 10);
            current = end != token && *end == '\0' && index >= 0
                ? cJSON_GetArrayItem(current, (int)index)
                : NULL;
        } else {
            current = NULL;
        }
        free(token);
        if (current == NULL || slash == NULL) {
            return current;
        }
        cursor = slash + 1;
    }
    return current;
}

static cJSON *resolve_value(const cJSON *value)
{
    if (!cJSON_IsObject(value)) {
        return NULL;
    }
    cJSON *path = cJSON_GetObjectItemCaseSensitive(value, "path");
    if (cJSON_IsString(path) && path->valuestring != NULL) {
        return resolve_data_path(path->valuestring);
    }
    cJSON *literal = cJSON_GetObjectItemCaseSensitive(value, "literalString");
    if (literal != NULL) {
        return literal;
    }
    literal = cJSON_GetObjectItemCaseSensitive(value, "literalNumber");
    if (literal != NULL) {
        return literal;
    }
    return cJSON_GetObjectItemCaseSensitive(value, "literalBoolean");
}

const char *room_canvas_resolve_string(const cJSON *value)
{
    cJSON *resolved = resolve_value(value);
    return cJSON_IsString(resolved) && resolved->valuestring != NULL
        ? resolved->valuestring
        : "";
}

const char *room_canvas_resolve_text(const cJSON *value, char *buffer, size_t buffer_size)
{
    cJSON *resolved = resolve_value(value);
    if (cJSON_IsString(resolved) && resolved->valuestring != NULL) {
        return resolved->valuestring;
    }
    if (cJSON_IsNumber(resolved)) {
        snprintf(buffer, buffer_size, "%.15g", resolved->valuedouble);
        return buffer;
    }
    if (cJSON_IsBool(resolved)) {
        return cJSON_IsTrue(resolved) ? "true" : "false";
    }
    return "";
}

bool room_canvas_resolve_boolean(const cJSON *value)
{
    cJSON *resolved = resolve_value(value);
    return cJSON_IsTrue(resolved) ||
           (cJSON_IsNumber(resolved) && resolved->valuedouble != 0);
}

static esp_err_t remember_surface_id(cJSON *action)
{
    cJSON *id = cJSON_GetObjectItemCaseSensitive(action, "surfaceId");
    if (!cJSON_IsString(id) || id->valuestring == NULL || id->valuestring[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    char *copy = strdup(id->valuestring);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    free(surface_id);
    surface_id = copy;
    return ESP_OK;
}

static esp_err_t apply_surface_update(
    cJSON *action,
    esp_openclaw_node_error_t *out_error)
{
    esp_err_t err = remember_surface_id(action);
    cJSON *updates = cJSON_GetObjectItemCaseSensitive(action, "components");
    if (err == ESP_ERR_INVALID_ARG || !cJSON_IsArray(updates)) {
        return set_error(
            out_error,
            "INVALID_PARAMS",
            "surfaceUpdate requires string surfaceId and a components array",
            ESP_ERR_INVALID_ARG);
    }
    if (err != ESP_OK) {
        return set_error(out_error, "INTERNAL", "not enough memory for the A2UI surface id", err);
    }

    cJSON *update = NULL;
    cJSON_ArrayForEach(update, updates) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(update, "id");
        cJSON *component = cJSON_GetObjectItemCaseSensitive(update, "component");
        cJSON *weight = cJSON_GetObjectItemCaseSensitive(update, "weight");
        if (!cJSON_IsObject(update) || !cJSON_IsString(id) ||
            id->valuestring == NULL || id->valuestring[0] == '\0' ||
            !cJSON_IsObject(component) || cJSON_GetArraySize(component) != 1 ||
            (weight != NULL && !cJSON_IsNumber(weight))) {
            return set_error(
                out_error,
                "INVALID_PARAMS",
                "each surfaceUpdate component requires string id and one component type",
                ESP_ERR_INVALID_ARG);
        }
        room_canvas_component_t *entry = find_component(id->valuestring);
        if (entry == NULL) {
            if (component_count >= ROOM_CANVAS_MAX_COMPONENTS) {
                return set_error(
                    out_error,
                    "INVALID_PARAMS",
                    "A2UI surface exceeds the 96-component bound",
                    ESP_ERR_INVALID_SIZE);
            }
            entry = &components[component_count];
            entry->id = strdup(id->valuestring);
            if (entry->id == NULL) {
                return set_error(out_error, "INTERNAL", "not enough memory for an A2UI component id", ESP_ERR_NO_MEM);
            }
            ++component_count;
        }
        cJSON *copy = cJSON_Duplicate(component, true);
        if (copy == NULL) {
            return set_error(out_error, "INTERNAL", "not enough memory for an A2UI component", ESP_ERR_NO_MEM);
        }
        cJSON_Delete(entry->component);
        entry->component = copy;
        entry->weight = cJSON_IsNumber(weight) ? weight->valuedouble : 0;
    }

    if (count_images_in_store() > ROOM_CANVAS_MAX_IMAGES) {
        return set_error(
            out_error,
            "INVALID_PARAMS",
            "A2UI surface exceeds the 6-image bound",
            ESP_ERR_INVALID_SIZE);
    }
    return ESP_OK;
}

static esp_err_t apply_begin_rendering(
    cJSON *action,
    esp_openclaw_node_error_t *out_error)
{
    esp_err_t err = remember_surface_id(action);
    cJSON *root = cJSON_GetObjectItemCaseSensitive(action, "root");
    if (err == ESP_ERR_INVALID_ARG || !cJSON_IsString(root) ||
        root->valuestring == NULL || root->valuestring[0] == '\0') {
        return set_error(
            out_error,
            "INVALID_PARAMS",
            "beginRendering requires string surfaceId and root",
            ESP_ERR_INVALID_ARG);
    }
    if (err != ESP_OK) {
        return set_error(out_error, "INTERNAL", "not enough memory for the A2UI surface id", err);
    }
    char *copy = strdup(root->valuestring);
    if (copy == NULL) {
        return set_error(out_error, "INTERNAL", "not enough memory for the A2UI root id", ESP_ERR_NO_MEM);
    }
    free(root_component_id);
    root_component_id = copy;
    return ESP_OK;
}

static esp_err_t set_data_model_path(
    const char *path,
    cJSON *contents,
    esp_openclaw_node_error_t *out_error)
{
    cJSON *copy = cJSON_Duplicate(contents, true);
    if (copy == NULL) {
        return set_error(out_error, "INTERNAL", "not enough memory for the A2UI data model", ESP_ERR_NO_MEM);
    }
    if (path == NULL || path[0] == '\0' || strcmp(path, "/") == 0) {
        cJSON_Delete(data_model);
        data_model = copy;
        return ESP_OK;
    }
    if (path[0] != '/') {
        cJSON_Delete(copy);
        return set_error(out_error, "INVALID_PARAMS", "dataModelUpdate path must be a JSON pointer", ESP_ERR_INVALID_ARG);
    }
    if (!cJSON_IsObject(data_model)) {
        cJSON_Delete(data_model);
        data_model = cJSON_CreateObject();
        if (data_model == NULL) {
            cJSON_Delete(copy);
            return set_error(out_error, "INTERNAL", "not enough memory for the A2UI data model", ESP_ERR_NO_MEM);
        }
    }

    cJSON *current = data_model;
    const char *cursor = path + 1;
    for (;;) {
        const char *slash = strchr(cursor, '/');
        size_t len = slash != NULL ? (size_t)(slash - cursor) : strlen(cursor);
        char *token = decode_json_pointer_token(cursor, len);
        if (token == NULL) {
            cJSON_Delete(copy);
            return set_error(out_error, "INTERNAL", "not enough memory for the A2UI data path", ESP_ERR_NO_MEM);
        }
        if (slash == NULL) {
            cJSON_DeleteItemFromObjectCaseSensitive(current, token);
            cJSON_AddItemToObject(current, token, copy);
            free(token);
            return ESP_OK;
        }
        cJSON *next = cJSON_GetObjectItemCaseSensitive(current, token);
        if (!cJSON_IsObject(next)) {
            cJSON_DeleteItemFromObjectCaseSensitive(current, token);
            next = cJSON_CreateObject();
            if (next == NULL) {
                free(token);
                cJSON_Delete(copy);
                return set_error(out_error, "INTERNAL", "not enough memory for the A2UI data path", ESP_ERR_NO_MEM);
            }
            cJSON_AddItemToObject(current, token, next);
        }
        free(token);
        current = next;
        cursor = slash + 1;
    }
}

static esp_err_t apply_data_model_update(
    cJSON *action,
    esp_openclaw_node_error_t *out_error)
{
    cJSON *surface = cJSON_GetObjectItemCaseSensitive(action, "surfaceId");
    cJSON *path = cJSON_GetObjectItemCaseSensitive(action, "path");
    cJSON *contents = cJSON_GetObjectItemCaseSensitive(action, "contents");
    if ((surface != NULL && (!cJSON_IsString(surface) || surface->valuestring == NULL)) ||
        (path != NULL && (!cJSON_IsString(path) || path->valuestring == NULL)) ||
        contents == NULL) {
        return set_error(
            out_error,
            "INVALID_PARAMS",
            "dataModelUpdate requires contents and optional string surfaceId and path",
            ESP_ERR_INVALID_ARG);
    }
    if (surface != NULL) {
        esp_err_t err = remember_surface_id(action);
        if (err != ESP_OK) {
            return set_error(
                out_error,
                err == ESP_ERR_NO_MEM ? "INTERNAL" : "INVALID_PARAMS",
                "dataModelUpdate surfaceId must be a non-empty string",
                err);
        }
    }
    return set_data_model_path(
        path != NULL ? path->valuestring : NULL,
        contents,
        out_error);
}

static esp_err_t apply_a2ui_message(
    cJSON *message,
    bool *out_begin_rendering,
    esp_openclaw_node_error_t *out_error)
{
    if (!cJSON_IsObject(message)) {
        return set_error(out_error, "INVALID_PARAMS", "each A2UI message must be an object", ESP_ERR_INVALID_ARG);
    }
    if (cJSON_GetObjectItemCaseSensitive(message, "version") != NULL ||
        cJSON_GetObjectItemCaseSensitive(message, "createSurface") != NULL) {
        return set_error(
            out_error,
            "INVALID_PARAMS",
            "A2UI v0.8 only; version and createSurface messages are not supported",
            ESP_ERR_INVALID_ARG);
    }
    if (cJSON_GetArraySize(message) != 1) {
        return set_error(
            out_error,
            "INVALID_PARAMS",
            "each A2UI v0.8 message must contain exactly one action key",
            ESP_ERR_INVALID_ARG);
    }

    cJSON *action = message->child;
    if (action == NULL || action->string == NULL || !cJSON_IsObject(action)) {
        return set_error(out_error, "INVALID_PARAMS", "A2UI action values must be objects", ESP_ERR_INVALID_ARG);
    }
    if (strcmp(action->string, "surfaceUpdate") == 0) {
        return apply_surface_update(action, out_error);
    }
    if (strcmp(action->string, "beginRendering") == 0) {
        esp_err_t err = apply_begin_rendering(action, out_error);
        if (err == ESP_OK) {
            *out_begin_rendering = true;
        }
        return err;
    }
    if (strcmp(action->string, "dataModelUpdate") == 0) {
        return apply_data_model_update(action, out_error);
    }
    if (strcmp(action->string, "deleteSurface") == 0) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(action, "surfaceId");
        if (!cJSON_IsString(id) || id->valuestring == NULL || id->valuestring[0] == '\0') {
            return set_error(out_error, "INVALID_PARAMS", "deleteSurface requires string surfaceId", ESP_ERR_INVALID_ARG);
        }
        clear_store();
        return ESP_OK;
    }
    return set_error(
        out_error,
        "INVALID_PARAMS",
        "A2UI v0.8 only; expected beginRendering, surfaceUpdate, dataModelUpdate, or deleteSurface",
        ESP_ERR_INVALID_ARG);
}

static esp_err_t validate_store_size(esp_openclaw_node_error_t *out_error)
{
    char *scratch = large_alloc(ROOM_CANVAS_MAX_STORE_BYTES + 6);
    if (scratch == NULL) {
        return set_error(
            out_error,
            "INTERNAL",
            "not enough memory to validate the A2UI store",
            ESP_ERR_NO_MEM);
    }

    size_t total = 0;
    for (size_t i = 0; i < component_count; ++i) {
        if (!cJSON_PrintPreallocated(
                components[i].component,
                scratch,
                ROOM_CANVAS_MAX_STORE_BYTES + 6,
                false)) {
            heap_caps_free(scratch);
            return set_error(
                out_error,
                "INVALID_PARAMS",
                "A2UI store exceeds the 262144-byte bound",
                ESP_ERR_INVALID_SIZE);
        }
        size_t item_size = strlen(components[i].id) + strlen(scratch) + 8;
        if (item_size > ROOM_CANVAS_MAX_STORE_BYTES - total) {
            heap_caps_free(scratch);
            return set_error(
                out_error,
                "INVALID_PARAMS",
                "A2UI store exceeds the 262144-byte bound",
                ESP_ERR_INVALID_SIZE);
        }
        total += item_size;
    }
    if (data_model != NULL) {
        if (!cJSON_PrintPreallocated(
                data_model,
                scratch,
                ROOM_CANVAS_MAX_STORE_BYTES + 6,
                false) ||
            strlen(scratch) > ROOM_CANVAS_MAX_STORE_BYTES - total) {
            heap_caps_free(scratch);
            return set_error(
                out_error,
                "INVALID_PARAMS",
                "A2UI store exceeds the 262144-byte bound",
                ESP_ERR_INVALID_SIZE);
        }
        total += strlen(scratch);
    }
    size_t metadata_size =
        (surface_id != NULL ? strlen(surface_id) : 0) +
        (root_component_id != NULL ? strlen(root_component_id) : 0);
    heap_caps_free(scratch);
    if (metadata_size > ROOM_CANVAS_MAX_STORE_BYTES - total) {
        return set_error(
            out_error,
            "INVALID_PARAMS",
            "A2UI store exceeds the 262144-byte bound",
            ESP_ERR_INVALID_SIZE);
    }
    return ESP_OK;
}

static esp_err_t apply_a2ui_array(
    const cJSON *messages,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    room_canvas_store_snapshot_t snapshot = {0};
    esp_err_t snapshot_err = copy_store_snapshot(&snapshot);
    if (snapshot_err != ESP_OK) {
        return set_error(
            out_error,
            "INTERNAL",
            "not enough memory to update the A2UI store safely",
            snapshot_err);
    }
    bool begin_rendering = false;
    cJSON *message = NULL;
    cJSON_ArrayForEach(message, messages) {
        esp_err_t err = apply_a2ui_message(message, &begin_rendering, out_error);
        if (err != ESP_OK) {
            restore_store_snapshot(&snapshot);
            return err;
        }
    }
    esp_err_t store_err = validate_store_size(out_error);
    if (store_err != ESP_OK) {
        restore_store_snapshot(&snapshot);
        return store_err;
    }
    if (root_component_id != NULL) {
        size_t node_budget = ROOM_CANVAS_MAX_RENDER_NODES;
        esp_err_t err = validate_component_depth(
            root_component_id,
            1,
            &node_budget,
            out_error);
        if (err != ESP_OK) {
            restore_store_snapshot(&snapshot);
            return err;
        }
    }
    esp_err_t err = render_a2ui_result(
        begin_rendering || canvas_active,
        out_payload_json,
        out_error);
    if (err != ESP_OK) {
        restore_store_snapshot(&snapshot);
        return err;
    }
    free_store_snapshot(&snapshot);
    return ESP_OK;
}

/* Tap on empty Canvas returns to status; hold opens local diagnostics. */
static void canvas_screen_clicked(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_LONG_PRESSED) {
        lv_indev_t *indev = lv_indev_active();
        if (indev != NULL) lv_indev_wait_release(indev);
        room_diagnostics_open();
        return;
    }
    if (code != LV_EVENT_CLICKED || room_diagnostics_is_open()) return;
    char *payload = NULL;
    esp_openclaw_node_error_t error = {0};
    if (room_canvas_hide(&payload, &error) == ESP_OK) {
        free(payload);
    }
    /* Same contract as the status-screen tap: a user-initiated exit must not
     * land on the idle-dark screen and look like a dead panel. */
    room_canvas_emit_action(ROOM_CANVAS_ACTION_REQUEST_FACE_HINT, 10000);
}

/*
 * Tap/BOOT view cycling: status <-> canvas when agent content exists. With no
 * content, the status tap wakes the face instead (handled in room_ui); this is
 * a no-op then. Safe from LVGL event callbacks: the display lock is a
 * recursive mutex and neither branch deletes the dispatching object.
 */
void room_canvas_view_toggle(void)
{
    char *payload = NULL;
    esp_openclaw_node_error_t error = {0};
    if (canvas_active) {
        if (room_canvas_hide(&payload, &error) == ESP_OK) {
            free(payload);
        }
        room_canvas_emit_action(ROOM_CANVAS_ACTION_REQUEST_FACE_HINT, 10000);
        return;
    }
    taskENTER_CRITICAL(&diagnostics_mux);
    room_canvas_retained_kind_t retained = diagnostics_snapshot.retained_kind;
    taskEXIT_CRITICAL(&diagnostics_mux);
    if (retained == ROOM_CANVAS_RETAINED_IMAGE) {
        if (room_board_display_lock(ROOM_CANVAS_DISPLAY_LOCK_MS)) {
            activate_canvas_locked();
            room_board_display_unlock();
            room_board_display_brightness_set(ROOM_CANVAS_ACTIVE_BRIGHTNESS);
            room_canvas_emit_action(ROOM_CANVAS_ACTION_RENDER_CHANGED, 0);
        }
        return;
    }
    if (root_component_id != NULL) {
        if (room_canvas_present(NULL, &payload, &error) == ESP_OK) free(payload);
        return;
    }
    /* No agent canvas content: wake the face so the tap always answers. */
    room_canvas_emit_action(ROOM_CANVAS_ACTION_REQUEST_FACE_HINT, 10000);
}

esp_err_t room_canvas_init(void)
{
    if (!room_board_display_lock(ROOM_CANVAS_DISPLAY_LOCK_MS)) {
        ESP_LOGE(TAG, "failed to lock display for canvas initialization");
        return ESP_ERR_TIMEOUT;
    }
    status_screen = lv_screen_active();
    canvas_screen = lv_obj_create(NULL);
    if (canvas_screen == NULL) {
        room_board_display_unlock();
        ESP_LOGE(TAG, "failed to create canvas screen");
        return ESP_ERR_NO_MEM;
    }
    style_canvas_screen(canvas_safe_pad());
    lv_obj_add_event_cb(canvas_screen, canvas_screen_clicked, LV_EVENT_ALL, NULL);
    room_board_display_unlock();
    return ESP_OK;
}

/*
 * The gateway mints the canvas surface URL with a ~10-minute idle expiry and
 * slides it only on use, so a quiet room's first present after a long gap
 * used to fail FETCH_FAILED until the next reconnect. Command handlers run on
 * the component task and cannot wait for a gateway response (the response
 * would be processed by the very task that is waiting), so instead of a
 * refresh-and-retry inside the fetch, a periodic RPC keeps the token warm
 * from the esp_timer task. The response callback stores the fresh URL in the
 * component, which stays the single read path.
 */
void room_canvas_set_gateway_http_base(const char *base_url)
{
    char *copy = base_url != NULL && base_url[0] != '\0' ? strdup(base_url) : NULL;
    if (copy != NULL) {
        size_t len = strlen(copy);
        while (len > 0 && copy[len - 1] == '/') {
            copy[--len] = '\0';
        }
    }
    free(gateway_http_base);
    gateway_http_base = copy;
}

bool room_canvas_is_active(void)
{
    return canvas_active;
}

void room_canvas_get_diagnostics(room_canvas_diagnostics_snapshot_t *snapshot)
{
    if (snapshot == NULL) return;
    taskENTER_CRITICAL(&diagnostics_mux);
    *snapshot = diagnostics_snapshot;
    taskEXIT_CRITICAL(&diagnostics_mux);
}

esp_err_t room_canvas_present(
    const char *url,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    if (canvas_screen == NULL) {
        return set_error(out_error, "UNAVAILABLE", "the canvas display is not initialized", ESP_ERR_INVALID_STATE);
    }
    if (url != NULL) {
        room_canvas_image_t image = {0};
        esp_err_t err = fetch_image(url, &image, out_error);
        if (err != ESP_OK) {
            return err;
        }
        return show_present_image(&image, out_payload_json, out_error);
    }
    return render_a2ui_result(true, out_payload_json, out_error);
}

esp_err_t room_canvas_hide(
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    if (canvas_screen == NULL || status_screen == NULL) {
        return set_error(out_error, "UNAVAILABLE", "the canvas display is not initialized", ESP_ERR_INVALID_STATE);
    }
    char *payload = strdup("{\"hidden\":true}");
    if (payload == NULL) {
        return set_error(out_error, "INTERNAL", "not enough memory for the command result", ESP_ERR_NO_MEM);
    }
    if (!room_board_display_lock(ROOM_CANVAS_DISPLAY_LOCK_MS)) {
        free(payload);
        return set_error(out_error, "UNAVAILABLE", "the display is busy; retry canvas.hide", ESP_ERR_TIMEOUT);
    }
    canvas_active = false;
    taskENTER_CRITICAL(&diagnostics_mux);
    diagnostics_snapshot.active = false;
    taskEXIT_CRITICAL(&diagnostics_mux);
    lv_screen_load(status_screen);
    room_board_display_unlock();
    /* Restore the live Talk state; forcing idle would blank an active call. */
    room_canvas_emit_action(ROOM_CANVAS_ACTION_RENDER_CHANGED, 0);
    *out_payload_json = payload;
    return ESP_OK;
}

esp_err_t room_canvas_a2ui_push_jsonl(
    const char *jsonl,
    size_t jsonl_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    if (jsonl_len > ROOM_CANVAS_MAX_JSONL_BYTES) {
        return set_error(
            out_error,
            "INVALID_PARAMS",
            "A2UI input exceeds the 262144-byte bound",
            ESP_ERR_INVALID_SIZE);
    }
    cJSON *messages = cJSON_CreateArray();
    if (messages == NULL) {
        return set_error(out_error, "INTERNAL", "not enough memory for A2UI JSONL", ESP_ERR_NO_MEM);
    }

    const char *cursor = jsonl;
    const char *end = jsonl + jsonl_len;
    while (cursor < end) {
        const char *line_end = memchr(cursor, '\n', (size_t)(end - cursor));
        if (line_end == NULL) {
            line_end = end;
        }
        const char *start = cursor;
        while (start < line_end && isspace((unsigned char)*start)) {
            ++start;
        }
        const char *trimmed_end = line_end;
        while (trimmed_end > start && isspace((unsigned char)trimmed_end[-1])) {
            --trimmed_end;
        }
        if (trimmed_end > start) {
            const char *parse_end = NULL;
            cJSON *message = cJSON_ParseWithLengthOpts(
                start,
                (size_t)(trimmed_end - start),
                &parse_end,
                false);
            if (message == NULL || parse_end != trimmed_end) {
                cJSON_Delete(message);
                cJSON_Delete(messages);
                return set_error(
                    out_error,
                    "INVALID_PARAMS",
                    "each non-empty A2UI JSONL line must contain one complete JSON object",
                    ESP_ERR_INVALID_ARG);
            }
            cJSON_AddItemToArray(messages, message);
        }
        cursor = line_end < end ? line_end + 1 : end;
    }
    esp_err_t err = apply_a2ui_array(messages, out_payload_json, out_error);
    cJSON_Delete(messages);
    return err;
}

esp_err_t room_canvas_a2ui_push_messages(
    const cJSON *messages,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    char *serialized = cJSON_PrintUnformatted(messages);
    if (serialized == NULL) {
        return set_error(out_error, "INTERNAL", "not enough memory to validate A2UI messages", ESP_ERR_NO_MEM);
    }
    size_t serialized_len = strlen(serialized);
    free(serialized);
    if (serialized_len > ROOM_CANVAS_MAX_JSONL_BYTES) {
        return set_error(
            out_error,
            "INVALID_PARAMS",
            "A2UI input exceeds the 262144-byte bound",
            ESP_ERR_INVALID_SIZE);
    }
    return apply_a2ui_array(messages, out_payload_json, out_error);
}

esp_err_t room_canvas_a2ui_reset(
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    char *payload = strdup("{\"reset\":true}");
    if (payload == NULL) {
        return set_error(out_error, "INTERNAL", "not enough memory for the command result", ESP_ERR_NO_MEM);
    }
    if (!room_board_display_lock(ROOM_CANVAS_DISPLAY_LOCK_MS)) {
        free(payload);
        return set_error(out_error, "UNAVAILABLE", "the display is busy; retry canvas.a2ui.reset", ESP_ERR_TIMEOUT);
    }
    lv_obj_clean(canvas_screen);
    clear_images_locked();
    clear_store();
    record_retained_kind(ROOM_CANVAS_RETAINED_NONE);
    if (canvas_active) {
        show_placeholder_locked();
    }
    room_board_display_unlock();
    if (canvas_active) {
        room_board_display_brightness_set(ROOM_CANVAS_ACTIVE_BRIGHTNESS);
    }
    *out_payload_json = payload;
    return ESP_OK;
}
