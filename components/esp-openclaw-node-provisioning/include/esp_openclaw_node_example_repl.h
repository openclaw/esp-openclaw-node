/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "esp_openclaw_node.h"

/**
 * @brief Start the UART REPL used by the examples.
 *
 * @param[in] node OpenClaw Node instance exposed to the REPL commands.
 *
 * @return
 *      - `ESP_OK` on success
 *      - an ESP-IDF error code if the REPL cannot be started
 */
esp_err_t esp_openclaw_node_example_repl_start(esp_openclaw_node_handle_t node);
