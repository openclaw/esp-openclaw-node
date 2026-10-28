#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef struct {
    bool wifi_connected;
    bool signal_known;
    int8_t rssi;
    bool gateway_connected;
} openclaw_status_facts_t;

typedef struct {
    lv_obj_t *root;
    openclaw_status_facts_t facts;
} openclaw_status_bar_t;

/* The caller owns the struct for the lifetime of the widget and holds the display lock. */
void openclaw_status_bar_create(openclaw_status_bar_t *bar, lv_obj_t *parent);
void openclaw_status_bar_set(openclaw_status_bar_t *bar, const openclaw_status_facts_t *facts);
