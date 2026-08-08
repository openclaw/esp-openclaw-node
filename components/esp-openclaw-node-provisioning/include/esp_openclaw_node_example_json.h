/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "cJSON.h"
#include "esp_err.h"
#include "esp_openclaw_node.h"

/**
 * @brief Parse raw command params JSON for an example command handler.
 *
 * When @p params_json is `NULL`, this helper parses `"{}"` so example handlers
 * can treat omitted params as an empty object.
 *
 * @param[in] params_json UTF-8 JSON params from the OpenClaw command request.
 * @param[out] out_params Parsed cJSON tree owned by the caller on success.
 * @param[out] out_error Optional structured command error populated when the
 *             input is not valid JSON.
 *
 * @return
 *      - `ESP_OK` on success
 *      - `ESP_ERR_INVALID_ARG` when the output pointer is `NULL` or the JSON
 *        payload cannot be parsed
 */
esp_err_t esp_openclaw_node_example_parse_json_params(
    const char *params_json,
    cJSON **out_params,
    esp_openclaw_node_error_t *out_error);

/**
 * @brief Serialize a cJSON payload and transfer ownership of the input tree.
 *
 * "Take" means this helper consumes @p payload. It always deletes the supplied
 * cJSON tree before returning, whether serialization succeeds or fails. On
 * success, the caller owns the returned `malloc()`-compatible JSON buffer.
 *
 * @param[in] payload cJSON payload tree to serialize and consume.
 * @param[out] out_payload_json Serialized UTF-8 JSON payload string.
 *
 * @return
 *      - `ESP_OK` on success
 *      - `ESP_ERR_INVALID_ARG` when an argument is invalid
 *      - `ESP_ERR_NO_MEM` when serialization fails due to allocation failure
 */
esp_err_t esp_openclaw_node_example_take_json_payload(
    cJSON *payload,
    char **out_payload_json);
