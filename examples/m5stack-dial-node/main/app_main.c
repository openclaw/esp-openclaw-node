#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/m5dial.h"
#include "cJSON.h"
#include "dial_action.h"
#include "dial_control.h"
#include "dial_home.h"
#include "dial_input.h"
#include "dial_navigation.h"
#include "dial_reader.h"
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
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "nvs_flash.h"

static const char *TAG = "openclaw_dial";
static esp_openclaw_node_handle_t node;
static esp_openclaw_node_example_saved_session_reconnect_t reconnect;
static atomic_bool connected;
static atomic_bool tone_active;
static esp_timer_handle_t tone_timer;
static const char *commands[] = {"display.show", "display.clear", "display.status", "display.brightness", "display.menu",
    "dial.configure", "dial.complete", "dial.status", "input.status", "face.set", "buzzer.play"};

typedef enum { VIEW_FACE, VIEW_CONTROL, VIEW_MESSAGE, VIEW_NAVIGATION } dial_view_t;
typedef struct {
    dial_action_token_t token;
    int value;
    char action[65];
    char unit[17];
    char session[ESP_OPENCLAW_NODE_MAX_SESSION_KEY_LEN + 1];
    char request_id[48];
} dial_action_t;
typedef struct { dial_action_token_t token; bool ok; } action_result_t;
static QueueHandle_t actions, results;
static SemaphoreHandle_t action_lock;
static dial_action_state_t action_state;
static struct {
    dial_control_t control;
    dial_view_t view;
    char heading[65], text[513], label[65], unit[17], action[65];
    char session[ESP_OPENCLAW_NODE_MAX_SESSION_KEY_LEN + 1];
    char request_id[48];
    dial_action_token_t last_action;
    int confirmed_value;
    bool action_completed, action_succeeded;
    char action_message[129];
    const char *mood;
    uint32_t changes, presses, touches;
    int brightness;
    lv_obj_t *arc, *title, *value, *message, *footer, *eyes[2], *mouth;
    dial_home_t home;
    dial_input_t input;
    dial_navigation_t navigation;
    dial_agents_snapshot_t *agents;
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
    if (view != VIEW_NAVIGATION) {
        free(ui.agents);
        ui.agents = NULL;
        ui.navigation.snapshot = NULL;
    }
    ui.view = view;
    lv_obj_set_hidden(ui.home.root, view != VIEW_FACE);
    lv_obj_set_hidden(ui.navigation.root, view != VIEW_NAVIGATION);
    lv_obj_set_hidden(ui.title, view == VIEW_FACE || view == VIEW_NAVIGATION);
    lv_obj_set_hidden(ui.footer, view == VIEW_FACE || view == VIEW_NAVIGATION);
    lv_obj_set_flag(ui.arc, LV_OBJ_FLAG_HIDDEN, view != VIEW_CONTROL);
    lv_obj_set_flag(ui.value, LV_OBJ_FLAG_HIDDEN, view != VIEW_CONTROL);
    lv_obj_set_flag(ui.message, LV_OBJ_FLAG_HIDDEN, view != VIEW_MESSAGE);
    for (int i = 0; i < 2; ++i) lv_obj_set_flag(ui.eyes[i], LV_OBJ_FLAG_HIDDEN, view != VIEW_FACE);
    lv_obj_set_flag(ui.mouth, LV_OBJ_FLAG_HIDDEN, view != VIEW_FACE);
    lv_label_set_text(ui.title, view == VIEW_MESSAGE ? ui.heading : view == VIEW_CONTROL ? ui.label : "OpenClaw");
    dial_input_set_control(&ui.input, view == VIEW_CONTROL);
}

static void navigation_event(dial_navigation_event_t event, size_t offset, void *context)
{
    (void)context;
    if (event == DIAL_NAV_HOME) show_view(VIEW_FACE);
    else if (event == DIAL_NAV_CONTROL) show_view(VIEW_CONTROL);
    else if (event == DIAL_NAV_REFRESH) {
        if (dial_reader_agents_refresh(offset) == ESP_OK) {
            dial_reader_agents_copy(ui.agents);
            dial_navigation_update(&ui.navigation, ui.agents);
        }
    } else dial_input_set_control(&ui.input, false);
}

static void open_navigation(bool agents)
{
    if (ui.agents == NULL) ui.agents = calloc(1, sizeof(*ui.agents));
    if (ui.agents == NULL) {
        strlcpy(ui.heading, "Menu unavailable", sizeof(ui.heading));
        lv_label_set_text(ui.message, "Not enough memory.\nHold to return Home.");
        show_view(VIEW_MESSAGE);
        return;
    }
    dial_reader_agents_copy(ui.agents);
    dial_navigation_update(&ui.navigation, ui.agents);
    show_view(VIEW_NAVIGATION);
    dial_navigation_open(&ui.navigation, agents);
}

static void update_value(void)
{
    lv_label_set_text_fmt(ui.value, "%d%s", dial_control_value(&ui.control), ui.unit);
}

static void publish_action_result(dial_action_token_t token, bool ok)
{
    xSemaphoreTake(action_lock, portMAX_DELAY);
    if (dial_action_matches(&action_state, token)) {
        action_result_t completion = {token, ok};
        xQueueOverwrite(results, &completion);
    }
    xSemaphoreGive(action_lock);
}

static void action_response(esp_openclaw_node_handle_t client,
                            const esp_openclaw_node_gateway_result_t *result, void *context)
{
    (void)client;
    publish_action_result(*(dial_action_token_t *)context, result->ok);
    free(context);
}

static void action_worker(void *context)
{
    (void)context;
    dial_action_t action;
    for (;;) {
        xQueueReceive(actions, &action, portMAX_DELAY);
        char message[256], key[80];
        snprintf(message, sizeof(message), "DIAL_INPUT requestId=%s action=%s value=%d unit=%s", action.request_id, action.action, action.value, action.unit);
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
        dial_action_token_t *token = malloc(sizeof(*token));
        esp_err_t err = ESP_ERR_NO_MEM;
        if (json != NULL && token != NULL) {
            *token = action.token;
            err = esp_openclaw_node_gateway_request_on_connection(
                node, token->connection_id, "node.event", json, action_response, token);
        }
        free(json);
        if (err != ESP_OK) {
            free(token);
            publish_action_result(action.token, false);
        }
    }
}

static void commit_action(void)
{
    ++ui.presses;
    xSemaphoreTake(action_lock, portMAX_DELAY);
    if (action_state.pending_attempt != 0) {
        xSemaphoreGive(action_lock);
        return;
    }
    if (action_state.configured_uri == NULL || ui.action[0] == '\0' || ui.session[0] == '\0') {
        xSemaphoreGive(action_lock);
        lv_label_set_text(ui.footer, "No action configured");
        return;
    }
    if (action_state.connection_id == 0) {
        xSemaphoreGive(action_lock);
        lv_label_set_text(ui.footer, "Gateway offline");
        return;
    }
    dial_action_t action = {.value = dial_control_value(&ui.control)};
    if (!dial_action_begin(&action_state, &action.token)) {
        xSemaphoreGive(action_lock);
        return;
    }
    strlcpy(action.action, ui.action, sizeof(action.action));
    strlcpy(action.unit, ui.unit, sizeof(action.unit));
    strlcpy(action.session, ui.session, sizeof(action.session));
    uint64_t nonce;
    esp_fill_random(&nonce, sizeof(nonce));
    snprintf(action.request_id, sizeof(action.request_id), "%016" PRIx64 "-%" PRIu64, nonce, action.token.attempt);
    if (xQueueSend(actions, &action, 0) == pdTRUE) {
        ui.last_action = action.token;
        ui.confirmed_value = action.value;
        ui.action_completed = false;
        ui.action_succeeded = false;
        ui.action_message[0] = '\0';
        strlcpy(ui.request_id, action.request_id, sizeof(ui.request_id));
        lv_label_set_text(ui.footer, "Sending action...");
    } else {
        dial_action_complete(&action_state, action.token);
        dial_action_resolve(&action_state, action.token);
        lv_label_set_text(ui.footer, "Action busy");
    }
    xSemaphoreGive(action_lock);
}

static void input_event(dial_input_event_t event, int value, bool pointer, void *context)
{
    (void)context;
    if (event == DIAL_INPUT_CHANGE) {
        if (dial_control_move(&ui.control, value)) {
            ++ui.changes;
            update_value();
        }
    } else if (event == DIAL_INPUT_HOME) {
        show_view(VIEW_FACE);
    } else if (ui.view == VIEW_NAVIGATION) {
        if (event == DIAL_INPUT_PREVIOUS) dial_navigation_move(&ui.navigation, -1);
        else if (event == DIAL_INPUT_NEXT) dial_navigation_move(&ui.navigation, 1);
        else if (event == DIAL_INPUT_OPEN) dial_navigation_activate(&ui.navigation);
    } else {
        if (pointer) ++ui.touches;
        if (event == DIAL_INPUT_CONFIRM) commit_action();
        else open_navigation(false);
    }
}

static void refresh_ui(lv_timer_t *timer)
{
    (void)timer;
    xSemaphoreTake(action_lock, portMAX_DELAY);
    action_result_t result;
    if (xQueueReceive(results, &result, 0) == pdTRUE && dial_action_complete(&action_state, result.token)) {
        if (!result.ok) dial_action_resolve(&action_state, result.token);
        lv_label_set_text(ui.footer, result.ok ? "Action sent" : "Action failed; press to retry");
    }
    static uint32_t last_connection;
    if (action_state.connection_id != last_connection) {
        last_connection = action_state.connection_id;
        lv_label_set_text(ui.footer, last_connection == 0 ? "Offline | USB setup" :
            ui.action[0] != '\0' && action_state.configured_uri == NULL ? "Configure action again" :
            "Connected | turn + press");
    }
    bool assigned = action_state.configured_uri != NULL;
    bool online = action_state.connection_id != 0;
    const char *activity = ui.request_id[0] != '\0' &&
        ui.last_action.connection_id == action_state.connection_id
        ? lv_label_get_text(ui.footer) : NULL;
    xSemaphoreGive(action_lock);
    if (ui.view == VIEW_NAVIGATION) {
        dial_reader_agents_copy(ui.agents);
        dial_navigation_update(&ui.navigation, ui.agents);
        return;
    }
    if (ui.view != VIEW_FACE) return;
    static uint32_t status_at;
    static bool status_sampled;
    uint32_t now = lv_tick_get();
    if (!status_sampled || now - status_at >= 1000) {
        dial_home_render(&ui.home, esp_openclaw_node_wifi_is_connected(), online, assigned, activity);
        status_at = now;
        status_sampled = true;
    }
    uint32_t phase = lv_tick_get() % 4400;
    int height = 28;
    if (strcmp(ui.mood, "sleepy") == 0) height = 10;
    else if (strcmp(ui.mood, "happy") == 0) height = 18;
    else if (strcmp(ui.mood, "sad") == 0) height = 24;
    else if (strcmp(ui.mood, "excited") == 0) height = 32;
    int gaze = (int)((lv_tick_get() / 1800) % 3) * 4 - 4;
    bool thinking = strcmp(ui.mood, "thinking") == 0;
    for (int i = 0; i < 2; ++i) {
        int eye_height = phase < 140 ? 4 : thinking && i == 0 ? 22 : height;
        lv_obj_set_size(ui.eyes[i], 19, eye_height);
        lv_obj_align(ui.eyes[i], LV_ALIGN_CENTER, (i == 0 ? -21 : 21) + gaze,
                     thinking ? -17 : strcmp(ui.mood, "sad") == 0 ? -9 : -13);
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
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x15101b), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0x362031), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    ui.arc = lv_arc_create(screen);
    lv_obj_set_size(ui.arc, 218, 218);
    lv_obj_center(ui.arc);
    lv_arc_set_range(ui.arc, 0, 100);
    lv_arc_set_value(ui.arc, 50);
    lv_obj_set_style_arc_color(ui.arc, lv_color_hex(0x58d5c4), LV_PART_INDICATOR);
    dial_input_init(&ui.input, bsp_display_get_input_dev(), screen, ui.arc, input_event, NULL);
    ui.title = label(screen, 158, -66, &lv_font_montserrat_16);
    ui.value = label(screen, 146, 0, &lv_font_montserrat_24);
    ui.message = label(screen, 170, 10, &lv_font_montserrat_16);
    lv_label_set_long_mode(ui.message, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(ui.message, 90);
    ui.footer = label(screen, 158, 70, &lv_font_montserrat_16);
    lv_label_set_text(ui.footer, "USB setup");
    dial_home_create(&ui.home, screen);
    dial_navigation_create(&ui.navigation, screen, navigation_event, NULL);
    for (int i = 0; i < 2; ++i) {
        ui.eyes[i] = lv_obj_create(screen);
        lv_obj_set_style_bg_color(ui.eyes[i], lv_color_hex(0x58d5c4), 0);
        lv_obj_set_style_border_width(ui.eyes[i], 0, 0);
        lv_obj_set_style_radius(ui.eyes[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_remove_flag(ui.eyes[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    ui.mouth = label(screen, 60, 12, &lv_font_montserrat_24);
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
    cJSON_AddStringToObject(result, "view", ui.view == VIEW_FACE ? "face" : ui.view == VIEW_CONTROL ? "control" : ui.view == VIEW_NAVIGATION ? "menu" : "message");
    if (ui.view == VIEW_NAVIGATION) {
        cJSON_AddStringToObject(result, "page", ui.navigation.view == DIAL_NAV_MENU ? "menu" :
            ui.navigation.view == DIAL_NAV_AGENTS ? "agents" : "agent");
    }
    if (ui.view == VIEW_MESSAGE) {
        cJSON_AddStringToObject(result, "heading", ui.heading);
        cJSON_AddStringToObject(result, "text", ui.text);
    }
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
    xSemaphoreTake(action_lock, portMAX_DELAY);
    cJSON_AddBoolToObject(result, "actionConfigured", action_state.configured_uri != NULL);
    cJSON_AddBoolToObject(result, "actionPending", action_state.pending_attempt != 0);
    if (ui.request_id[0] != '\0' && ui.last_action.connection_id == action_state.connection_id &&
        (action_state.result_attempt == ui.last_action.attempt || ui.action_completed)) {
        cJSON_AddStringToObject(result, "requestId", ui.request_id);
        cJSON_AddNumberToObject(result, "confirmedValue", ui.confirmed_value);
        cJSON_AddBoolToObject(result, "actionCompleted", ui.action_completed);
        if (ui.action_completed) {
            cJSON_AddBoolToObject(result, "actionSucceeded", ui.action_succeeded);
            cJSON_AddStringToObject(result, "actionMessage", ui.action_message);
        }
    }
    xSemaphoreGive(action_lock);
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
    if (strcmp(command, "display.menu") == 0) {
        const char *fields[] = {"page"};
        const char *page = "menu";
        if (!only_fields(params, fields, 1) ||
            (cJSON_HasObjectItem(params, "page") && !text_field(params, "page", 6, &page)) ||
            (strcmp(page, "menu") != 0 && strcmp(page, "agents") != 0)) {
            err = invalid(error, "page must be menu or agents");
        } else open_navigation(strcmp(page, "agents") == 0);
    } else if (strcmp(command, "display.show") == 0) {
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
    } else if (strcmp(command, "dial.complete") == 0) {
        const char *fields[] = {"requestId", "success", "text"};
        const char *request_id, *text;
        cJSON *success = cJSON_GetObjectItemCaseSensitive(params, "success");
        if (!only_fields(params, fields, 3) || !text_field(params, "requestId", 47, &request_id) ||
            !text_field(params, "text", 128, &text) || !cJSON_IsBool(success)) {
            err = invalid(error, "dial.complete needs requestId, boolean success, and text (up to 128 bytes)");
        } else {
            xSemaphoreTake(action_lock, portMAX_DELAY);
            if (invocation == NULL || invocation->session_key == NULL ||
                strcmp(invocation->session_key, ui.session) != 0 ||
                strcmp(request_id, ui.request_id) != 0 ||
                !dial_action_resolve(&action_state, ui.last_action)) {
                error->code = "UNAVAILABLE";
                error->message = "the press is no longer current or belongs to another agent session";
                err = ESP_ERR_INVALID_STATE;
            } else {
                ui.action_completed = true;
                ui.action_succeeded = cJSON_IsTrue(success);
                strlcpy(ui.action_message, text, sizeof(ui.action_message));
                lv_label_set_text(ui.footer, text[0] != '\0' ? text : ui.action_succeeded ? "Action complete" : "Action failed");
            }
            xSemaphoreGive(action_lock);
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
        } else {
            xSemaphoreTake(action_lock, portMAX_DELAY);
            if (!dial_action_configure(&action_state, action[0] != '\0')) {
                error->code = "UNAVAILABLE";
                error->message = "finish the pending action and connect to a Gateway before configuring an action";
                err = ESP_ERR_INVALID_STATE;
            } else {
                ui.control = control;
                ui.request_id[0] = '\0';
                ui.action_completed = false;
                strlcpy(ui.label, name, sizeof(ui.label));
                strlcpy(ui.unit, unit, sizeof(ui.unit));
                strlcpy(ui.action, action, sizeof(ui.action));
                strlcpy(ui.session, action[0] != '\0' ? invocation->session_key : "", sizeof(ui.session));
                lv_arc_set_range(ui.arc, 0, dial_control_positions(&ui.control));
                lv_arc_set_value(ui.arc, ui.control.position);
                update_value();
                show_view(VIEW_CONTROL);
            }
            xSemaphoreGive(action_lock);
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
    (void)context;
    bool online = event == ESP_OPENCLAW_NODE_EVENT_CONNECTED;
    char *uri = online ? esp_openclaw_node_dup_gateway_uri(client) : NULL;
    uint32_t id = online ? esp_openclaw_node_get_connection_id(client) : 0;
    xSemaphoreTake(action_lock, portMAX_DELAY);
    dial_action_connect(&action_state, id, uri);
    atomic_store(&connected, action_state.connection_id != 0);
    xSemaphoreGive(action_lock);
    free(uri);
    dial_reader_node_event(event);
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

static int reader_command(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("usage: reader\n");
        return 1;
    }
    cJSON *status = dial_reader_status();
    char *json = status != NULL ? cJSON_PrintUnformatted(status) : NULL;
    cJSON_Delete(status);
    if (json == NULL) return 1;
    printf("%s\n", json);
    free(json);
    return 0;
}

static int agents_command(int argc, char **argv)
{
    if (argc > 1) {
        char *end = NULL;
        errno = 0;
        unsigned long offset = argc == 3 ? strtoul(argv[2], &end, 10) : 0;
        if (argc > 3 || strcmp(argv[1], "refresh") != 0 ||
            (argc == 3 && (argv[2][0] < '0' || argv[2][0] > '9' || *end != '\0' || errno != 0))) {
            printf("usage: agents [refresh [offset]]\n");
            return 1;
        }
        if (dial_reader_agents_refresh(offset) != ESP_OK) return 1;
    }
    dial_agents_snapshot_t *snapshot = malloc(sizeof(*snapshot));
    if (snapshot == NULL) return 1;
    dial_reader_agents_copy(snapshot);
    static const char *states[] = {"idle", "loading", "ready", "empty", "offline", "connecting", "access-needed", "error", "too-large"};
    cJSON *result = cJSON_CreateObject();
    cJSON *agents = result != NULL ? cJSON_AddArrayToObject(result, "agents") : NULL;
    bool ok = agents != NULL && cJSON_AddStringToObject(result, "state", states[snapshot->state]) != NULL &&
        cJSON_AddNumberToObject(result, "revision", snapshot->revision) != NULL &&
        cJSON_AddNumberToObject(result, "offset", snapshot->page.offset) != NULL &&
        cJSON_AddNumberToObject(result, "total", snapshot->page.total) != NULL;
    for (size_t i = 0; ok && i < snapshot->page.count; ++i) {
        const esp_openclaw_agent_t *agent = &snapshot->page.agents[i];
        cJSON *row = cJSON_CreateObject();
        if (row == NULL) { ok = false; break; }
        if (!cJSON_AddItemToArray(agents, row)) { cJSON_Delete(row); ok = false; break; }
        ok = cJSON_AddStringToObject(row, "id", agent->id) != NULL &&
            cJSON_AddStringToObject(row, "name", agent->name) != NULL && cJSON_AddStringToObject(row, "emoji", agent->emoji) != NULL;
    }
    free(snapshot);
    char *json = ok ? cJSON_PrintUnformatted(result) : NULL;
    cJSON_Delete(result);
    if (json == NULL) return 1;
    printf("%s\n", json);
    free(json);
    return 0;
}

static int conversations_command(int argc, char **argv)
{
    dial_conversations_snapshot_t *snapshot = malloc(sizeof(*snapshot));
    if (snapshot == NULL) return 1;
    esp_err_t err = ESP_OK;
    if (argc == 3 && strcmp(argv[1], "open") == 0) {
        dial_agents_snapshot_t *agents = malloc(sizeof(*agents));
        if (agents == NULL) { free(snapshot); return 1; }
        dial_reader_agents_copy(agents);
        err = dial_reader_conversations_open(argv[2], agents->revision);
        free(agents);
    } else if (argc == 3 && (strcmp(argv[1], "refresh") == 0 || strcmp(argv[1], "preview") == 0)) {
        char *end;
        errno = 0;
        unsigned long index = strtoul(argv[2], &end, 10);
        if (argv[2][0] < '0' || argv[2][0] > '9' || *end != '\0' || errno != 0 || index > 2147483647u)
            err = ESP_ERR_INVALID_ARG;
        else if (strcmp(argv[1], "refresh") == 0) err = dial_reader_conversations_refresh(index);
        else {
            dial_reader_conversations_copy(snapshot);
            err = dial_reader_preview_open(index, snapshot->revision);
        }
    } else if (argc == 2 && strcmp(argv[1], "close") == 0) dial_reader_conversations_close();
    else if (argc != 1) err = ESP_ERR_INVALID_ARG;
    if (err != ESP_OK) {
        free(snapshot);
        printf("%s: conversations [open <agent> | refresh <offset> | preview <index> | close]\n", esp_err_to_name(err));
        return 1;
    }
    dial_reader_conversations_copy(snapshot);
    static const char *states[] = {"idle", "loading", "ready", "empty", "offline", "connecting", "access-needed", "error", "too-large"};
    static const char *previews[] = {"ok", "empty", "missing", "error"};
    static const char *roles[] = {"user", "assistant", "tool", "system", "other"};
    cJSON *root = cJSON_CreateObject();
    cJSON *sessions = root != NULL ? cJSON_AddArrayToObject(root, "sessions") : NULL;
    cJSON *items = root != NULL ? cJSON_AddArrayToObject(root, "items") : NULL;
    bool ok = sessions != NULL && items != NULL &&
        cJSON_AddStringToObject(root, "state", states[snapshot->state]) != NULL &&
        cJSON_AddStringToObject(root, "agentId", snapshot->agent.id) != NULL &&
        cJSON_AddNumberToObject(root, "revision", snapshot->revision) != NULL &&
        cJSON_AddNumberToObject(root, "offset", snapshot->page.offset) != NULL &&
        cJSON_AddBoolToObject(root, "hasMore", snapshot->page.has_more) != NULL &&
        cJSON_AddNumberToObject(root, "nextOffset", snapshot->page.next_offset) != NULL &&
        cJSON_AddStringToObject(root, "previewState", states[snapshot->preview_state]) != NULL &&
        cJSON_AddStringToObject(root, "previewStatus", snapshot->preview_state == DIAL_AGENTS_READY ? previews[snapshot->preview.status] : "unavailable") != NULL &&
        cJSON_AddNumberToObject(root, "selected", snapshot->selected) != NULL;
    for (size_t i = 0; ok && i < snapshot->page.count; ++i) {
        const esp_openclaw_session_t *session = &snapshot->page.sessions[i];
        cJSON *row = cJSON_CreateObject();
        if (row == NULL) { ok = false; break; }
        if (!cJSON_AddItemToArray(sessions, row)) { cJSON_Delete(row); ok = false; break; }
        ok = cJSON_AddStringToObject(row, "key", session->key) != NULL &&
            cJSON_AddStringToObject(row, "title", session->title) != NULL &&
            cJSON_AddStringToObject(row, "lastMessage", session->last_message) != NULL;
    }
    for (size_t i = 0; ok && i < snapshot->preview.count; ++i) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) { ok = false; break; }
        if (!cJSON_AddItemToArray(items, item)) { cJSON_Delete(item); ok = false; break; }
        ok = cJSON_AddStringToObject(item, "role", roles[snapshot->preview.items[i].role]) != NULL &&
            cJSON_AddStringToObject(item, "text", snapshot->preview.items[i].text) != NULL;
    }
    free(snapshot);
    char *json = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (json == NULL) return 1;
    printf("%s\n", json);
    free(json);
    return 0;
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
    action_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(actions != NULL && results != NULL && action_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM);
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
    esp_err_t reader_err = dial_reader_start(node);
    if (reader_err != ESP_OK) ESP_LOGW(TAG, "read access unavailable: %s", esp_err_to_name(reader_err));
    ESP_ERROR_CHECK(esp_openclaw_node_example_saved_session_reconnect_start(&reconnect, node, "dial_reconnect"));
    ESP_ERROR_CHECK(esp_openclaw_node_example_repl_start(node));
    const esp_console_cmd_t local = {.command = "local", .help = "Run a Dial command over USB (no agent session)",
        .hint = "<command> <JSON object>", .func = local_command};
    ESP_ERROR_CHECK(esp_console_cmd_register(&local));
    const esp_console_cmd_t reader_status = {.command = "reader", .help = "Show this Dial's independent Gateway read access",
        .func = reader_command};
    ESP_ERROR_CHECK(esp_console_cmd_register(&reader_status));
    const esp_console_cmd_t agents = {.command = "agents", .help = "Read or refresh the permitted agent roster",
        .hint = "[refresh [offset]]", .func = agents_command};
    ESP_ERROR_CHECK(esp_console_cmd_register(&agents));
    const esp_console_cmd_t conversations = {.command = "conversations", .help = "Browse permitted conversations and previews",
        .hint = "[open <agent> | refresh <offset> | preview <index> | close]", .func = conversations_command};
    ESP_ERROR_CHECK(esp_console_cmd_register(&conversations));
    ESP_LOGI(TAG, "ready: provision Wi-Fi and Gateway from the USB console");
}
