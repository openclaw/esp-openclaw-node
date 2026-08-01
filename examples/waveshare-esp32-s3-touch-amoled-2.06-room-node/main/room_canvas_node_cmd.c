#include "room_canvas_node_cmd.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "room_canvas.h"

#define TAG "room_canvas_cmd"

static esp_err_t invalid_params(
    esp_openclaw_node_error_t *out_error,
    const char *message)
{
    out_error->code = "INVALID_PARAMS";
    out_error->message = message;
    return ESP_ERR_INVALID_ARG;
}

static cJSON *parse_params(
    const char *params_json,
    size_t params_len,
    esp_openclaw_node_error_t *out_error)
{
    cJSON *params = cJSON_ParseWithLength(params_json, params_len);
    if (!cJSON_IsObject(params)) {
        cJSON_Delete(params);
        invalid_params(out_error, "command params must be a JSON object");
        return NULL;
    }
    return params;
}

static esp_err_t handle_present(
    esp_openclaw_node_handle_t node,
    void *context,
    const char *params_json,
    size_t params_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node;
    (void)context;
    cJSON *params = parse_params(params_json, params_len, out_error);
    if (params == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *url = cJSON_GetObjectItemCaseSensitive(params, "url");
    cJSON *placement = cJSON_GetObjectItemCaseSensitive(params, "placement");
    if ((url != NULL && (!cJSON_IsString(url) || url->valuestring == NULL ||
                         url->valuestring[0] == '\0')) ||
        (placement != NULL && !cJSON_IsObject(placement))) {
        cJSON_Delete(params);
        return invalid_params(
            out_error,
            "canvas.present url must be a non-empty string and placement must be an object");
    }

    esp_err_t err = room_canvas_present(
        url != NULL ? url->valuestring : NULL,
        out_payload_json,
        out_error);
    cJSON_Delete(params);
    return err;
}

static esp_err_t handle_navigate(
    esp_openclaw_node_handle_t node,
    void *context,
    const char *params_json,
    size_t params_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node;
    (void)context;
    cJSON *params = parse_params(params_json, params_len, out_error);
    if (params == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *url = cJSON_GetObjectItemCaseSensitive(params, "url");
    if (!cJSON_IsString(url) || url->valuestring == NULL ||
        url->valuestring[0] == '\0') {
        cJSON_Delete(params);
        return invalid_params(out_error, "canvas.navigate requires a non-empty string url");
    }

    esp_err_t err = room_canvas_present(url->valuestring, out_payload_json, out_error);
    cJSON_Delete(params);
    return err;
}

static esp_err_t handle_hide(
    esp_openclaw_node_handle_t node,
    void *context,
    const char *params_json,
    size_t params_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node;
    (void)context;
    cJSON *params = parse_params(params_json, params_len, out_error);
    if (params == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(params);
    return room_canvas_hide(out_payload_json, out_error);
}

static esp_err_t handle_snapshot(
    esp_openclaw_node_handle_t node,
    void *context,
    const char *params_json,
    size_t params_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node;
    (void)context;
    cJSON *params = parse_params(params_json, params_len, out_error);
    if (params == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *format = cJSON_GetObjectItemCaseSensitive(params, "format");
    cJSON *max_width = cJSON_GetObjectItemCaseSensitive(params, "maxWidth");
    cJSON *quality = cJSON_GetObjectItemCaseSensitive(params, "quality");
    bool format_valid = format == NULL ||
        (cJSON_IsString(format) && format->valuestring != NULL &&
         (strcmp(format->valuestring, "png") == 0 ||
          strcmp(format->valuestring, "jpeg") == 0));
    bool width_valid = max_width == NULL ||
        (cJSON_IsNumber(max_width) && max_width->valuedouble >= 1 &&
         max_width->valuedouble == (double)max_width->valueint);
    bool quality_valid = quality == NULL ||
        (cJSON_IsNumber(quality) && quality->valuedouble >= 0.0 &&
         quality->valuedouble <= 1.0);
    if (!format_valid || !width_valid || !quality_valid) {
        cJSON_Delete(params);
        return invalid_params(
            out_error,
            "canvas.snapshot format must be png or jpeg, maxWidth a positive integer, and quality between 0 and 1");
    }

    esp_err_t err = room_canvas_snapshot(
        max_width != NULL ? max_width->valueint : 0,
        quality != NULL ? quality->valuedouble : 0.80,
        out_payload_json,
        out_error);
    cJSON_Delete(params);
    return err;
}

static esp_err_t handle_push_jsonl(
    esp_openclaw_node_handle_t node,
    void *context,
    const char *params_json,
    size_t params_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node;
    (void)context;
    cJSON *params = parse_params(params_json, params_len, out_error);
    if (params == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *jsonl = cJSON_GetObjectItemCaseSensitive(params, "jsonl");
    if (!cJSON_IsString(jsonl) || jsonl->valuestring == NULL) {
        cJSON_Delete(params);
        return invalid_params(out_error, "canvas.a2ui.pushJSONL requires a string jsonl field");
    }

    size_t jsonl_len = strlen(jsonl->valuestring);
    esp_err_t err = room_canvas_a2ui_push_jsonl(
        jsonl->valuestring,
        jsonl_len,
        out_payload_json,
        out_error);
    cJSON_Delete(params);
    return err;
}

static esp_err_t handle_push(
    esp_openclaw_node_handle_t node,
    void *context,
    const char *params_json,
    size_t params_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node;
    (void)context;
    cJSON *params = parse_params(params_json, params_len, out_error);
    if (params == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *messages = cJSON_GetObjectItemCaseSensitive(params, "messages");
    cJSON *jsonl = cJSON_GetObjectItemCaseSensitive(params, "jsonl");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (messages != NULL) {
        if (!cJSON_IsArray(messages)) {
            cJSON_Delete(params);
            return invalid_params(out_error, "canvas.a2ui.push messages must be an array");
        }
        err = room_canvas_a2ui_push_messages(messages, out_payload_json, out_error);
    } else if (cJSON_IsString(jsonl) && jsonl->valuestring != NULL) {
        err = room_canvas_a2ui_push_jsonl(
            jsonl->valuestring,
            strlen(jsonl->valuestring),
            out_payload_json,
            out_error);
    } else {
        err = invalid_params(
            out_error,
            "canvas.a2ui.push requires messages or jsonl");
    }
    cJSON_Delete(params);
    return err;
}

static esp_err_t handle_reset(
    esp_openclaw_node_handle_t node,
    void *context,
    const char *params_json,
    size_t params_len,
    char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node;
    (void)context;
    cJSON *params = parse_params(params_json, params_len, out_error);
    if (params == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(params);
    return room_canvas_a2ui_reset(out_payload_json, out_error);
}

esp_err_t room_canvas_register_node_commands(esp_openclaw_node_handle_t node)
{
    static const esp_openclaw_node_command_t COMMANDS[] = {
        {.name = "canvas.present", .handler = handle_present},
        {.name = "canvas.navigate", .handler = handle_navigate},
        {.name = "canvas.hide", .handler = handle_hide},
        {.name = "canvas.snapshot", .handler = handle_snapshot},
        {.name = "canvas.a2ui.pushJSONL", .handler = handle_push_jsonl},
        {.name = "canvas.a2ui.push", .handler = handle_push},
        {.name = "canvas.a2ui.reset", .handler = handle_reset},
    };
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); ++i) {
        ESP_RETURN_ON_ERROR(
            esp_openclaw_node_register_command(node, &COMMANDS[i]),
            TAG,
            "register %s",
            COMMANDS[i].name);
    }
    return ESP_OK;
}
