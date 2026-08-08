/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_openclaw_node_internal.h"

#include <stdlib.h>
#include <string.h>

static esp_err_t require_idle_registration_state(esp_openclaw_node_handle_t node)
{
    esp_openclaw_node_lock_state(node);
    bool idle = node->state == ESP_OPENCLAW_NODE_INTERNAL_IDLE &&
                node->pending_control == ESP_OPENCLAW_NODE_PENDING_CONTROL_REQUEST_NONE;
    esp_openclaw_node_unlock_state(node);
    return idle ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void esp_openclaw_node_cleanup_registry(esp_openclaw_node_handle_t node)
{
    for (size_t i = 0; i < node->capability_count; ++i) {
        free(node->capabilities[i]);
        node->capabilities[i] = NULL;
    }
    node->capability_count = 0;

    for (size_t i = 0; i < node->scope_count; ++i) {
        free(node->scopes[i]);
        node->scopes[i] = NULL;
    }
    node->scope_count = 0;

    for (size_t i = 0; i < node->command_count; ++i) {
        free(node->commands[i].name);
        node->commands[i].name = NULL;
        node->commands[i].handler = NULL;
        node->commands[i].handler_v2 = NULL;
        node->commands[i].context = NULL;
    }
    node->command_count = 0;
}

esp_openclaw_node_registered_command_t *esp_openclaw_node_find_command(
    esp_openclaw_node_handle_t node,
    const char *name)
{
    for (size_t i = 0; i < node->command_count; ++i) {
        if (node->commands[i].name != NULL &&
            strcmp(node->commands[i].name, name) == 0) {
            return &node->commands[i];
        }
    }
    return NULL;
}

esp_err_t esp_openclaw_node_dispatch_command(
    esp_openclaw_node_handle_t node,
    const char *command,
    const char *params_json,
    size_t params_len,
    const esp_openclaw_node_command_invocation_t *invocation,
    char **out_payload_json,
    const char **out_error_code,
    const char **out_error_message)
{
    *out_payload_json = NULL;
    *out_error_code = "INVALID_REQUEST";
    *out_error_message = "command failed";

    esp_openclaw_node_registered_command_t *registered =
        esp_openclaw_node_find_command(node, command);
    if (registered == NULL) {
        *out_error_code = "UNSUPPORTED_COMMAND";
        *out_error_message = "unsupported command";
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_openclaw_node_error_t error = {
        .code = "INVALID_REQUEST",
        .message = "command failed",
    };
    esp_err_t err = registered->handler_v2 != NULL
        ? registered->handler_v2(
              node,
              registered->context,
              invocation,
              params_json,
              params_len,
              out_payload_json,
              &error)
        : registered->handler(
              node,
              registered->context,
              params_json,
              params_len,
              out_payload_json,
              &error);
    *out_error_code = error.code;
    *out_error_message = error.message;
    return err;
}

void esp_openclaw_node_add_registered_string_array(
    cJSON *parent,
    const char *name,
    char *const *items,
    size_t count)
{
    cJSON *array = cJSON_CreateArray();
    for (size_t i = 0; i < count; ++i) {
        if (items[i] != NULL) {
            cJSON_AddItemToArray(array, cJSON_CreateString(items[i]));
        }
    }
    cJSON_AddItemToObject(parent, name, array);
}

void esp_openclaw_node_add_registered_command_array(
    cJSON *parent,
    const char *name,
    esp_openclaw_node_handle_t node)
{
    cJSON *array = cJSON_CreateArray();
    for (size_t i = 0; i < node->command_count; ++i) {
        if (node->commands[i].name != NULL) {
            cJSON_AddItemToArray(
                array,
                cJSON_CreateString(node->commands[i].name));
        }
    }
    cJSON_AddItemToObject(parent, name, array);
}

esp_err_t esp_openclaw_node_register_capability_internal(
    esp_openclaw_node_handle_t node,
    const char *capability)
{
    if (node == NULL || capability == NULL || capability[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (require_idle_registration_state(node) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < node->capability_count; ++i) {
        if (strcmp(node->capabilities[i], capability) == 0) {
            return ESP_OK;
        }
    }
    if (node->capability_count >= ESP_OPENCLAW_NODE_MAX_CAPABILITIES) {
        return ESP_ERR_NO_MEM;
    }

    char *copy = esp_openclaw_node_duplicate_string(capability);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    node->capabilities[node->capability_count++] = copy;
    return ESP_OK;
}

esp_err_t esp_openclaw_node_register_scope_internal(
    esp_openclaw_node_handle_t node,
    const char *scope)
{
    if (node == NULL || scope == NULL || scope[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (require_idle_registration_state(node) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < node->scope_count; ++i) {
        if (strcmp(node->scopes[i], scope) == 0) {
            return ESP_OK;
        }
    }
    if (node->scope_count >= ESP_OPENCLAW_NODE_MAX_SCOPES) {
        return ESP_ERR_NO_MEM;
    }

    char *copy = esp_openclaw_node_duplicate_string(scope);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    node->scopes[node->scope_count++] = copy;
    return ESP_OK;
}

esp_err_t esp_openclaw_node_register_command_internal(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_command_t *command)
{
    if (node == NULL || command == NULL || command->name == NULL ||
        command->name[0] == '\0' || command->handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (require_idle_registration_state(node) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (esp_openclaw_node_find_command(node, command->name) != NULL) {
        return ESP_OK;
    }
    if (node->command_count >= ESP_OPENCLAW_NODE_MAX_COMMANDS) {
        return ESP_ERR_NO_MEM;
    }

    esp_openclaw_node_registered_command_t *slot = &node->commands[node->command_count];
    slot->name = esp_openclaw_node_duplicate_string(command->name);
    if (slot->name == NULL) {
        return ESP_ERR_NO_MEM;
    }
    slot->handler = command->handler;
    slot->handler_v2 = NULL;
    slot->context = command->context;
    node->command_count += 1;
    return ESP_OK;
}


esp_err_t esp_openclaw_node_register_command_v2_internal(
    esp_openclaw_node_handle_t node,
    const esp_openclaw_node_command_v2_t *command)
{
    if (node == NULL || command == NULL || command->name == NULL ||
        command->name[0] == '\0' || command->handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (require_idle_registration_state(node) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (esp_openclaw_node_find_command(node, command->name) != NULL) {
        return ESP_OK;
    }
    if (node->command_count >= ESP_OPENCLAW_NODE_MAX_COMMANDS) {
        return ESP_ERR_NO_MEM;
    }

    esp_openclaw_node_registered_command_t *slot = &node->commands[node->command_count];
    slot->name = esp_openclaw_node_duplicate_string(command->name);
    if (slot->name == NULL) {
        return ESP_ERR_NO_MEM;
    }
    slot->handler = NULL;
    slot->handler_v2 = command->handler;
    slot->context = command->context;
    node->command_count += 1;
    return ESP_OK;
}
