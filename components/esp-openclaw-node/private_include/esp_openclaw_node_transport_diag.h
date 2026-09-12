/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

/* Private, synchronous observation points; implementations must not change transport state. */
void esp_openclaw_node_transport_start_begin(const char *role);
void esp_openclaw_node_transport_start_end(const char *role, esp_err_t result);
