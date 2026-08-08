#pragma once

#include <stddef.h>
#include "cJSON.h"
#include "esp_err.h"
#include "esp_openclaw_node.h"
#include "lvgl.h"
#include "room_canvas.h"

#define ROOM_CANVAS_MAX_COMPONENTS 96
#define ROOM_CANVAS_MAX_IMAGES 6

typedef struct {
    char *id;
    cJSON *component;
    double weight;
} room_canvas_component_t;

extern lv_obj_t *room_canvas_screen;
extern esp_openclaw_node_handle_t room_canvas_node;
extern char *room_canvas_gateway_http_base;
extern volatile bool room_canvas_active;
extern room_canvas_component_t room_canvas_components[ROOM_CANVAS_MAX_COMPONENTS];
extern size_t room_canvas_component_count;
extern cJSON *room_canvas_data_model;
extern char *room_canvas_surface_id;
extern char *room_canvas_root_component_id;
typedef enum {
    ROOM_CANVAS_IMAGE_NONE = 0,
    ROOM_CANVAS_IMAGE_JPEG,
    ROOM_CANVAS_IMAGE_PNG,
} room_canvas_image_kind_t;

typedef struct {
    uint8_t *data;
    size_t data_len;
    room_canvas_image_kind_t kind;
    lv_image_dsc_t descriptor;
    lv_draw_buf_t *decoded;
    uint32_t width;
    uint32_t height;
    char *component_id;
} room_canvas_image_t;

extern room_canvas_image_t room_canvas_images[ROOM_CANVAS_MAX_IMAGES];
extern size_t room_canvas_image_count;

esp_err_t room_canvas_fetch_image(
    const char *url,
    room_canvas_image_t *image,
    esp_openclaw_node_error_t *error);
void room_canvas_release_image(room_canvas_image_t *image);
void room_canvas_release_image_array(room_canvas_image_t *array, size_t count);
bool room_canvas_validate_image(room_canvas_image_t *image);
room_canvas_component_t *room_canvas_find_component(const char *id);
cJSON *room_canvas_component_type_item(const room_canvas_component_t *entry);
bool room_canvas_component_is_image(const room_canvas_component_t *entry);
const char *room_canvas_resolve_string(const cJSON *value);
const char *room_canvas_resolve_text(const cJSON *value, char *buffer, size_t size);
bool room_canvas_resolve_boolean(const cJSON *value);
int room_canvas_safe_pad(void);
int room_canvas_width(void);
int room_canvas_height(void);
void room_canvas_style_screen(int32_t pad);
void room_canvas_show_placeholder(void);
void room_canvas_activate_locked(void);
void room_canvas_clear_images_locked(void);
void room_canvas_configure_image_object(
    lv_obj_t *object,
    lv_obj_t *parent,
    const room_canvas_image_t *image,
    bool fullscreen);
esp_err_t room_canvas_render_a2ui_result(
    bool force_active,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error);

esp_err_t room_canvas_set_error(
    esp_openclaw_node_error_t *error,
    const char *code,
    const char *message,
    esp_err_t err);
void *room_canvas_large_alloc(size_t size);
void *room_canvas_large_aligned_alloc(size_t alignment, size_t size);
void room_canvas_emit_action(room_canvas_action_t action, uint32_t value);
void room_canvas_bind_owner_session(const char *session_key);
bool room_canvas_copy_owner_session(char *buffer, size_t buffer_size);
