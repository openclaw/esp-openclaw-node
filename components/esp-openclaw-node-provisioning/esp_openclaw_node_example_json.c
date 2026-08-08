/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_openclaw_node_example_json.h"

#include <stdlib.h>

esp_err_t esp_openclaw_node_example_parse_json_params(
    const char *params_json,
    cJSON **out_params,
    esp_openclaw_node_error_t *out_error)
{
    if (out_params == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_params = NULL;
    cJSON *params = cJSON_Parse(params_json != NULL ? params_json : "{}");
    if (params == NULL) {
        if (out_error != NULL) {
            out_error->code = "INVALID_PARAMS";
            out_error->message = "paramsJSON is not valid JSON";
        }
        return ESP_ERR_INVALID_ARG;
    }

    *out_params = params;
    return ESP_OK;
}

esp_err_t esp_openclaw_node_example_take_json_payload(
    cJSON *payload,
    char **out_payload_json)
{
    if (payload == NULL || out_payload_json == NULL) {
        cJSON_Delete(payload);
        return ESP_ERR_INVALID_ARG;
    }

    char *json = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    *out_payload_json = json;
    return ESP_OK;
}
