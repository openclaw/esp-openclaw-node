#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/m5dial.h"
#include "cJSON.h"
#include "dial_control.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_openclaw_node.h"
#include "esp_openclaw_node_common_device_node_cmd.h"
#include "esp_openclaw_node_example_json.h"
#include "esp_openclaw_node_example_repl.h"
#include "esp_openclaw_node_example_saved_session_reconnect.h"
#include "esp_openclaw_node_wifi.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "nvs_flash.h"

static const char *TAG = "openclaw_dial";
static esp_openclaw_node_handle_t node;
static esp_openclaw_node_example_saved_session_reconnect_t reconnect;
static atomic_bool connected;
static atomic_bool tone_active;
static esp_timer_handle_t tone_timer;
static const char *commands[] = {"display.show", "display.clear", "display.status", "display.brightness",
    "dial.configure", "dial.status", "input.status", "face.set", "buzzer.play"};

typedef enum { VIEW_FACE, VIEW_CONTROL, VIEW_MESSAGE } dial_view_t;
typedef struct {
    uint32_t generation;
    int value;
    char action[65];
    char unit[17];
    char session[ESP_OPENCLAW_NODE_MAX_SESSION_KEY_LEN + 1];
} dial_action_t;
typedef struct { uint32_t generation; bool ok; } action_result_t;
static QueueHandle_t actions, results;
static struct {
    dial_control_t control;
    dial_view_t view;
    char heading[65], text[513], label[65], unit[17], action[65];
    char session[ESP_OPENCLAW_NODE_MAX_SESSION_KEY_LEN + 1];
    const char *mood;
    uint32_t generation, changes, presses, touches;
    int brightness;
    bool action_pending;
    lv_obj_t *arc, *title, *value, *message, *footer, *eyes[2], *mouth;
    lv_group_t *group;
} ui;

static esp_err_t invalid(esp_openclaw_node_error_t *error, const char *message)
{
    error->code = "INVALID_PARAMS";
    error->message = message;
    return ESP_ERR_INVALID_ARG;
}

static bool text_field(cJSON *params, const char *key, size_t maximum, const char **value)
{
    cJSON *field = cJSON_GetObjectItemCaseSensitive(params, key);
    if (!cJSON_IsString(field) || strlen(field->valuestring) > maximum) return false;
    *value = field->valuestring;
    return true;
}

static bool integer_field(cJSON *params, const char *key, int low, int high, int *value)
{
    cJSON *field = cJSON_GetObjectItemCaseSensitive(params, key);
    if (!cJSON_IsNumber(field) || !isfinite(field->valuedouble) ||
        field->valuedouble < low || field->valuedouble > high ||
        trunc(field->valuedouble) != field->valuedouble) return false;
    *value = (int)field->valuedouble;
    return true;
}

static bool only_fields(cJSON *params, const char *const *names, size_t count)
{
    const cJSON *field;
    cJSON_ArrayForEach(field, params) {
        bool found = false;
        for (size_t i = 0; i < count; ++i) {
            if (strcmp(field->string, names[i]) == 0) found = true;
        }
        if (!found) return false;
        for (const cJSON *other = field->next; other != NULL; other = other->next) {
            if (strcmp(field->string, other->string) == 0) return false;
        }
    }
    return true;
}

static void show_view(dial_view_t view)
{
    ui.view = view;
    lv_obj_set_flag(ui.arc, LV_OBJ_FLAG_HIDDEN, view != VIEW_CONTROL);
    lv_obj_set_flag(ui.value, LV_OBJ_FLAG_HIDDEN, view != VIEW_CONTROL);
    lv_obj_set_flag(ui.message, LV_OBJ_FLAG_HIDDEN, view != VIEW_MESSAGE);
    for (int i = 0; i < 2; ++i) lv_obj_set_flag(ui.eyes[i], LV_OBJ_FLAG_HIDDEN, view != VIEW_FACE);
    lv_obj_set_flag(ui.mouth, LV_OBJ_FLAG_HIDDEN, view != VIEW_FACE);
    lv_label_set_text(ui.title, view == VIEW_MESSAGE ? ui.heading : view == VIEW_CONTROL ? ui.label : "OpenClaw");
    lv_group_focus_obj(view == VIEW_CONTROL ? ui.arc : lv_screen_active());
    lv_group_set_editing(ui.group, true);
}

static void update_value(void)
{
    lv_label_set_text_fmt(ui.value, "%d%s", dial_control_value(&ui.control), ui.unit);
}

static void action_response(esp_openclaw_node_handle_t client,
                            const esp_openclaw_node_gateway_result_t *result, void *context)
{
    (void)client;
    action_result_t completion = {*(uint32_t *)context, result->ok};
    free(context);
    xQueueOverwrite(results, &completion);
}

static void action_worker(void *context)
{
    (void)context;
    dial_action_t action;
    for (;;) {
        xQueueReceive(actions, &action, portMAX_DELAY);
        char message[192], key[80];
        snprintf(message, sizeof(message), "DIAL_INPUT action=%s value=%d unit=%s", action.action, action.value, action.unit);
        snprintf(key, sizeof(key), "dial-%s-%lld", esp_openclaw_node_get_device_id(node), (long long)esp_timer_get_time());
        cJSON *payload = cJSON_CreateObject();
        bool built = payload != NULL && cJSON_AddStringToObject(payload, "message", message) != NULL &&
            cJSON_AddStringToObject(payload, "sessionKey", action.session) != NULL &&
            cJSON_AddStringToObject(payload, "key", key) != NULL &&
            cJSON_AddBoolToObject(payload, "deliver", false) != NULL;
        char *payload_json = built ? cJSON_PrintUnformatted(payload) : NULL;
        cJSON_Delete(payload);
        cJSON *params = cJSON_CreateObject();
        built = payload_json != NULL && params != NULL &&
            cJSON_AddStringToObject(params, "event", "agent.request") != NULL &&
            cJSON_AddStringToObject(params, "payloadJSON", payload_json) != NULL;
        char *json = built ? cJSON_PrintUnformatted(params) : NULL;
        cJSON_Delete(params);
        free(payload_json);
        uint32_t *generation = malloc(sizeof(*generation));
        esp_err_t err = ESP_ERR_NO_MEM;
        if (json != NULL && generation != NULL) {
            *generation = action.generation;
            err = esp_openclaw_node_gateway_request(node, "node.event", json, action_response, generation);
        }
        free(json);
        if (err != ESP_OK) {
            free(generation);
            action_result_t result = {action.generation, false};
            xQueueOverwrite(results, &result);
        }
    }
}

static void commit_action(void)
{
    ++ui.presses;
    if (ui.action_pending) return;
    if (ui.action[0] == '\0' || ui.session[0] == '\0') {
        lv_label_set_text(ui.footer, "No action configured");
        return;
    }
    if (!atomic_load(&connected)) {
        lv_label_set_text(ui.footer, "Gateway offline");
        return;
    }
    dial_action_t action = {.generation = ui.generation, .value = dial_control_value(&ui.control)};
    strlcpy(action.action, ui.action, sizeof(action.action));
    strlcpy(action.unit, ui.unit, sizeof(action.unit));
    strlcpy(action.session, ui.session, sizeof(action.session));
    if (xQueueSend(actions, &action, 0) == pdTRUE) {
        ui.action_pending = true;
        lv_label_set_text(ui.footer, "Sending action...");
    } else {
        lv_label_set_text(ui.footer, "Action busy");
    }
}

static void input_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_KEY && ui.view != VIEW_CONTROL) {
        uint32_t key = lv_event_get_key(event);
        if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) show_view(VIEW_CONTROL);
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        if (dial_control_move(&ui.control, lv_arc_get_value(ui.arc))) {
            ++ui.changes;
            update_value();
        }
    } else if (code == LV_EVENT_LONG_PRESSED) {
        show_view(VIEW_FACE);
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        lv_indev_t *input = lv_indev_active();
        if (input != NULL && lv_indev_get_type(input) == LV_INDEV_TYPE_POINTER) ++ui.touches;
        if (ui.view == VIEW_CONTROL) commit_action();
        else show_view(VIEW_CONTROL);
    }
}

static void encoder_long_press(lv_event_t *event)
{
    (void)event;
    /* Editable widgets consume encoder long presses before object callbacks. */
    lv_indev_t *encoder = bsp_display_get_input_dev();
    show_view(VIEW_FACE);
    lv_indev_reset(encoder, NULL);
    lv_indev_wait_release(encoder);
}

static void refresh_ui(lv_timer_t *timer)
{
    (void)timer;
    action_result_t result;
    if (xQueueReceive(results, &result, 0) == pdTRUE && result.generation == ui.generation) {
        ui.action_pending = false;
        lv_label_set_text(ui.footer, result.ok ? "Action sent" : "Action failed; press to retry");
    }
    static bool was_connected;
    bool online = atomic_load(&connected);
    if (online != was_connected) {
        was_connected = online;
        lv_label_set_text(ui.footer, online ? "Connected | turn + press" : "Offline | USB setup");
    }
    if (ui.view != VIEW_FACE) return;
    uint32_t phase = lv_tick_get() % 4400;
    int height = 42;
    if (strcmp(ui.mood, "sleepy") == 0) height = 10;
    else if (strcmp(ui.mood, "happy") == 0) height = 18;
    else if (strcmp(ui.mood, "sad") == 0) height = 24;
    else if (strcmp(ui.mood, "excited") == 0) height = 52;
    int gaze = (int)((lv_tick_get() / 1800) % 3) * 4 - 4;
    bool thinking = strcmp(ui.mood, "thinking") == 0;
    for (int i = 0; i < 2; ++i) {
        int eye_height = phase < 140 ? 4 : thinking && i == 0 ? 22 : height;
        lv_obj_set_size(ui.eyes[i], 29, eye_height);
        lv_obj_align(ui.eyes[i], LV_ALIGN_CENTER, (i == 0 ? -36 : 36) + gaze,
                     thinking ? -16 : strcmp(ui.mood, "sad") == 0 ? 0 : -8);
    }
}

static lv_obj_t *label(lv_obj_t *parent, int width, int y, const lv_font_t *font)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_obj_set_width(object, width);
    lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_align(object, LV_ALIGN_CENTER, 0, y);
    return object;
}

static esp_err_t start_display(void)
{
    bsp_display_cfg_t config = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = 240 * 20,
        .double_buffer = false,
        .flags = {.buff_dma = true, .buff_spiram = false},
    };
    lv_display_t *display = bsp_display_start_with_config(&config);
    if (display == NULL) return ESP_FAIL;
    ESP_RETURN_ON_ERROR(bsp_display_brightness_set(65), TAG, "backlight");
    if (!bsp_display_lock(1000)) return ESP_ERR_TIMEOUT;
    bsp_display_rotate(display, LV_DISPLAY_ROTATION_270);
    ui.control = (dial_control_t){0, 100, 1, 50};
    ui.brightness = 65;
    ui.mood = "neutral";
    strlcpy(ui.label, "Dial", sizeof(ui.label));
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x10151f), 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen, input_event, LV_EVENT_ALL, NULL);
    ui.arc = lv_arc_create(screen);
    lv_obj_set_size(ui.arc, 218, 218);
    lv_obj_center(ui.arc);
    lv_arc_set_range(ui.arc, 0, 100);
    lv_arc_set_value(ui.arc, 50);
    lv_obj_set_style_arc_color(ui.arc, lv_color_hex(0x58d5c4), LV_PART_INDICATOR);
    lv_obj_add_event_cb(ui.arc, input_event, LV_EVENT_ALL, NULL);
    ui.group = lv_group_create();
    lv_group_add_obj(ui.group, screen);
    lv_group_add_obj(ui.group, ui.arc);
    lv_indev_set_group(bsp_display_get_input_dev(), ui.group);
    lv_indev_add_event_cb(bsp_display_get_input_dev(), encoder_long_press, LV_EVENT_LONG_PRESSED, NULL);
    ui.title = label(screen, 158, -66, &lv_font_montserrat_16);
    ui.value = label(screen, 146, 0, &lv_font_montserrat_24);
    ui.message = label(screen, 170, 10, &lv_font_montserrat_16);
    lv_label_set_long_mode(ui.message, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(ui.message, 90);
    ui.footer = label(screen, 158, 70, &lv_font_montserrat_16);
    lv_label_set_text(ui.footer, "USB setup");
    for (int i = 0; i < 2; ++i) {
        ui.eyes[i] = lv_obj_create(screen);
        lv_obj_set_style_bg_color(ui.eyes[i], lv_color_hex(0x58d5c4), 0);
        lv_obj_set_style_border_width(ui.eyes[i], 0, 0);
        lv_obj_set_style_radius(ui.eyes[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_remove_flag(ui.eyes[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    ui.mouth = label(screen, 90, 30, &lv_font_montserrat_24);
    lv_label_set_text(ui.mouth, "~");
    update_value();
    show_view(VIEW_FACE);
    lv_timer_create(refresh_ui, 50, NULL);
    bsp_display_unlock();
    return ESP_OK;
}

static cJSON *status_payload(void)
{
    cJSON *result = cJSON_CreateObject();
    if (result == NULL) return NULL;
    cJSON_AddBoolToObject(result, "connected", atomic_load(&connected));
    cJSON_AddStringToObject(result, "view", ui.view == VIEW_FACE ? "face" : ui.view == VIEW_CONTROL ? "control" : "message");
    cJSON_AddStringToObject(result, "label", ui.label);
    cJSON_AddStringToObject(result, "unit", ui.unit);
    cJSON_AddNumberToObject(result, "value", dial_control_value(&ui.control));
    cJSON_AddNumberToObject(result, "min", ui.control.minimum);
    cJSON_AddNumberToObject(result, "max", ui.control.maximum);
    cJSON_AddNumberToObject(result, "step", ui.control.step);
    cJSON_AddNumberToObject(result, "changes", ui.changes);
    cJSON_AddNumberToObject(result, "presses", ui.presses);
    cJSON_AddNumberToObject(result, "touches", ui.touches);
    cJSON_AddNumberToObject(result, "brightness", ui.brightness);
    cJSON_AddBoolToObject(result, "buzzerPlaying", atomic_load(&tone_active));
    cJSON_AddBoolToObject(result, "actionConfigured", ui.action[0] != '\0');
    cJSON_AddBoolToObject(result, "actionPending", ui.action_pending);
    cJSON_AddStringToObject(result, "mood", ui.mood);
    cJSON_AddNumberToObject(result, "width", 240);
    cJSON_AddNumberToObject(result, "height", 240);
    return result;
}

static void stop_tone(void *context)
{
    (void)context;
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    atomic_store(&tone_active, false);
}

static esp_err_t handle_command(esp_openclaw_node_handle_t client, void *context,
    const esp_openclaw_node_command_invocation_t *invocation, const char *json,
    size_t length, char **out, esp_openclaw_node_error_t *error)
{
    (void)client;
    const char *command = context, *end = NULL;
    cJSON *params = cJSON_ParseWithLengthOpts(json, length, &end, false);
    if (!cJSON_IsObject(params)) {
        cJSON_Delete(params);
        return invalid(error, "parameters must be one JSON object");
    }
    while (end < json + length && isspace((unsigned char)*end)) ++end;
    if (end != json + length) {
        cJSON_Delete(params);
        return invalid(error, "unexpected data after parameters");
    }
    if (!bsp_display_lock(150)) {
        cJSON_Delete(params);
        error->code = "UNAVAILABLE";
        error->message = "display busy; retry the command";
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ESP_OK;
    if (strcmp(command, "display.show") == 0) {
        const char *heading, *text;
        const char *fields[] = {"heading", "text"};
        if (!only_fields(params, fields, 2) || !text_field(params, "heading", 64, &heading) ||
            !text_field(params, "text", 512, &text)) {
            err = invalid(error, "display.show needs heading (up to 64 bytes) and text (up to 512 bytes)");
        } else {
            strlcpy(ui.heading, heading, sizeof(ui.heading));
            strlcpy(ui.text, text, sizeof(ui.text));
            lv_label_set_text(ui.message, ui.text);
            show_view(VIEW_MESSAGE);
        }
    } else if (strcmp(command, "display.brightness") == 0) {
        const char *fields[] = {"percent"};
        int percent;
        if (!only_fields(params, fields, 1) || !integer_field(params, "percent", 0, 100, &percent)) {
            err = invalid(error, "percent must be an integer from 0 to 100");
        } else {
            err = bsp_display_brightness_set(percent);
            if (err == ESP_OK) ui.brightness = percent;
        }
    } else if (strcmp(command, "dial.configure") == 0) {
        const char *fields[] = {"label", "min", "max", "step", "value", "unit", "action"};
        const char *name, *unit = "", *action = "";
        int low, high, step, value;
        dial_control_t control;
        bool valid = only_fields(params, fields, 7) && text_field(params, "label", 64, &name) &&
            integer_field(params, "min", -10000, 10000, &low) && integer_field(params, "max", -10000, 10000, &high) &&
            integer_field(params, "step", 1, 20000, &step) && integer_field(params, "value", -10000, 10000, &value) &&
            dial_control_configure(&control, low, high, step, value);
        if (cJSON_HasObjectItem(params, "unit")) valid = text_field(params, "unit", 16, &unit) && valid;
        if (cJSON_HasObjectItem(params, "action")) valid = text_field(params, "action", 64, &action) && valid;
        if (!valid) {
            err = invalid(error, "dial range must contain 1..1000 equal steps; value must lie on that grid");
        } else if (action[0] != '\0' && (invocation == NULL || invocation->session_key == NULL || invocation->session_key[0] == '\0')) {
            err = invalid(error, "configure an action from an agent session so button presses have a destination");
        } else if (ui.action_pending) {
            error->code = "UNAVAILABLE";
            error->message = "wait for the pending dial action before replacing the control";
            err = ESP_ERR_INVALID_STATE;
        } else {
            ++ui.generation;
            ui.control = control;
            strlcpy(ui.label, name, sizeof(ui.label));
            strlcpy(ui.unit, unit, sizeof(ui.unit));
            strlcpy(ui.action, action, sizeof(ui.action));
            strlcpy(ui.session, action[0] != '\0' ? invocation->session_key : "", sizeof(ui.session));
            lv_arc_set_range(ui.arc, 0, dial_control_positions(&ui.control));
            lv_arc_set_value(ui.arc, ui.control.position);
            update_value();
            show_view(VIEW_CONTROL);
        }
    } else if (strcmp(command, "face.set") == 0) {
        const char *fields[] = {"mood"};
        const char *mood;
        const char *moods[] = {"neutral", "happy", "excited", "thinking", "sleepy", "sad"};
        const char *selected = NULL;
        if (only_fields(params, fields, 1) && text_field(params, "mood", 16, &mood)) {
            for (size_t i = 0; i < sizeof(moods) / sizeof(moods[0]); ++i) {
                if (strcmp(mood, moods[i]) == 0) selected = moods[i];
            }
        }
        if (selected == NULL) err = invalid(error, "mood must be neutral, happy, excited, thinking, sleepy, or sad");
        else {
            ui.mood = selected;
            lv_label_set_text(ui.mouth, strcmp(selected, "sad") == 0 ? "-" : strcmp(selected, "excited") == 0 ? "o" : "~");
            show_view(VIEW_FACE);
        }
    } else if (strcmp(command, "buzzer.play") == 0) {
        const char *fields[] = {"frequencyHz", "durationMs"};
        int frequency, duration;
        if (!only_fields(params, fields, 2) || !integer_field(params, "frequencyHz", 100, 4000, &frequency) ||
            !integer_field(params, "durationMs", 20, 2000, &duration)) {
            err = invalid(error, "frequencyHz must be 100..4000; durationMs must be 20..2000");
        } else if (atomic_exchange(&tone_active, true)) {
            error->code = "UNAVAILABLE";
            error->message = "buzzer is already playing";
            err = ESP_ERR_INVALID_STATE;
        } else {
            err = ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, frequency);
            if (err == ESP_OK) err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 512);
            if (err == ESP_OK) err = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            if (err == ESP_OK) err = esp_timer_start_once(tone_timer, (uint64_t)duration * 1000);
            if (err != ESP_OK) stop_tone(NULL);
        }
    } else if (cJSON_GetArraySize(params) != 0) {
        err = invalid(error, "this command accepts only {}");
    } else if (strcmp(command, "display.clear") == 0) {
        show_view(VIEW_FACE);
    }
    if (err == ESP_OK) err = esp_openclaw_node_example_take_json_payload(status_payload(), out);
    bsp_display_unlock();
    cJSON_Delete(params);
    return err;
}

static void node_event(esp_openclaw_node_handle_t client, esp_openclaw_node_event_t event,
                       const void *data, void *context)
{
    (void)client; (void)context;
    atomic_store(&connected, event == ESP_OPENCLAW_NODE_EVENT_CONNECTED);
    esp_openclaw_node_example_saved_session_reconnect_handle_event(&reconnect, event, data);
    ESP_LOGI(TAG, "Gateway %s", event == ESP_OPENCLAW_NODE_EVENT_CONNECTED ? "connected" : "offline");
}

static int local_command(int argc, char **argv)
{
    if (argc != 3) {
        printf("usage: local <command> <JSON object>\n");
        return 1;
    }
    const char *command = NULL;
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        if (strcmp(argv[1], commands[i]) == 0) command = commands[i];
    }
    if (command == NULL) {
        printf("unknown Dial command\n");
        return 1;
    }
    char *payload = NULL;
    esp_openclaw_node_error_t error = {0};
    esp_err_t err = handle_command(node, (void *)command, NULL, argv[2], strlen(argv[2]), &payload, &error);
    if (err == ESP_OK) printf("%s\n", payload);
    else printf("%s: %s\n", esp_err_to_name(err), error.message != NULL ? error.message : "command failed");
    free(payload);
    return err == ESP_OK ? 0 : 1;
}

void app_main(void)
{
    /* GPIO46 holds battery power. Backlight owns LEDC timer/channel 1; reserve
     * timer/channel 0 for the buzzer so tones cannot change screen brightness. */
    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_46, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_46, 1));
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    actions = xQueueCreate(1, sizeof(dial_action_t));
    results = xQueueCreate(1, sizeof(action_result_t));
    ESP_ERROR_CHECK(actions != NULL && results != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ledc_timer_config_t tone = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 1000, .clk_cfg = LEDC_AUTO_CLK};
    ledc_channel_config_t speaker = {.gpio_num = GPIO_NUM_3, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0};
    ESP_ERROR_CHECK(ledc_timer_config(&tone));
    ESP_ERROR_CHECK(ledc_channel_config(&speaker));
    esp_timer_create_args_t timer = {.callback = stop_tone, .name = "dial_tone"};
    ESP_ERROR_CHECK(esp_timer_create(&timer, &tone_timer));
    ESP_ERROR_CHECK(start_display());
    ESP_ERROR_CHECK(esp_openclaw_node_wifi_start());
    esp_openclaw_node_config_t config;
    esp_openclaw_node_config_init_default(&config);
    config.display_name = "OpenClaw M5Dial";
    config.model_identifier = "m5stack-dial";
    config.event_cb = node_event;
    ESP_ERROR_CHECK(esp_openclaw_node_create(&config, &node));
    static const esp_openclaw_node_device_commands_config_t device = {
        .device_name = "OpenClaw M5Dial", .model_identifier = "m5stack-dial"};
    ESP_ERROR_CHECK(esp_openclaw_node_register_device_commands(node, &device));
    const char *capabilities[] = {"display", "dial", "face", "buzzer"};
    for (size_t i = 0; i < sizeof(capabilities) / sizeof(capabilities[0]); ++i) {
        ESP_ERROR_CHECK(esp_openclaw_node_register_capability(node, capabilities[i]));
    }
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        esp_openclaw_node_command_v2_t command = {.name = commands[i], .handler = handle_command, .context = (void *)commands[i]};
        ESP_ERROR_CHECK(esp_openclaw_node_register_command_v2(node, &command));
    }
    ESP_ERROR_CHECK(xTaskCreate(action_worker, "dial_action", 4096, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_openclaw_node_example_saved_session_reconnect_start(&reconnect, node, "dial_reconnect"));
    ESP_ERROR_CHECK(esp_openclaw_node_example_repl_start(node));
    const esp_console_cmd_t local = {.command = "local", .help = "Run a Dial command over USB (no agent session)",
        .hint = "<command> <JSON object>", .func = local_command};
    ESP_ERROR_CHECK(esp_console_cmd_register(&local));
    ESP_LOGI(TAG, "ready: provision Wi-Fi and Gateway from the USB console");
}
