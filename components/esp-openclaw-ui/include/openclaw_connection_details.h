#pragma once

#include <stddef.h>
#include "esp_openclaw_node_wifi.h"

/* Bounded, read-only network details for both device layouts. */
void openclaw_wifi_details_format(char *text, size_t size,
                                 const esp_openclaw_node_wifi_status_t *wifi);
