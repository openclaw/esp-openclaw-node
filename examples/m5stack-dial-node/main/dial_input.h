#pragma once

#include "lvgl.h"

typedef enum {
    DIAL_INPUT_OPEN,
    DIAL_INPUT_HOME,
    DIAL_INPUT_CHANGE,
    DIAL_INPUT_CONFIRM,
    DIAL_INPUT_PREVIOUS,
    DIAL_INPUT_NEXT,
    DIAL_INPUT_BACK,
    DIAL_INPUT_HOME_MENU,
    DIAL_INPUT_HOME_AGENTS,
} dial_input_event_t;

typedef void (*dial_input_handler_t)(dial_input_event_t event, int value, bool pointer, void *context);

typedef struct {
    lv_obj_t *screen, *arc, *back;
    lv_obj_t *home_menu, *home_agents, *home_primary;
    lv_indev_t *encoder;
    lv_group_t *group;
    dial_input_handler_t handler;
    void *context;
    lv_point_t press_point;
    bool control, pointer_moved;
} dial_input_t;

void dial_input_init(dial_input_t *input, lv_indev_t *encoder, lv_obj_t *screen,
                     lv_obj_t *arc, dial_input_handler_t handler, void *context);
void dial_input_set_control(dial_input_t *input, bool control);
/* Bind once after creating Home. The screen remains the encoder focus. */
void dial_input_bind_home(dial_input_t *input, lv_obj_t *menu, lv_obj_t *agents, lv_obj_t *primary);
