// SPDX-License-Identifier: Apache-2.0

#include "compact_board.h"
#include "compact_board_text.h"
#include "compact_workspace.h"
#include "compact_input.h"
#include "compact_reader_console.h"
#include "compact_workspace_console.h"
#include "lvgl.h"
#include "esp_openclaw_reader.h"
#include "driver/ledc.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_openclaw_node_wifi.h"
#include "esp_openclaw_node_example_json.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#if CONFIG_EXAMPLE_BOARD_M5STICKC
#include "esp_lcd_st7735.h"
#endif

static const char *TAG = "compact_board";
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_flush_done;
static uint16_t *s_pixels;
static bool s_message_visible;
static char s_view[24] = "home";
static unsigned s_input_count;
static uint32_t s_workspace_revision;
static compact_console_input_t s_console_input;
static char s_console_selection[97];
static int s_message_line_height;
static int64_t s_last_input_ms;
static lv_display_t *s_lv_display;
static int s_brightness;
static atomic_bool s_connected;
static SemaphoreHandle_t s_state_mutex;
static TaskHandle_t s_display_task;
static bool s_ready;
static uint32_t s_rendered_generation;
static uint32_t s_render_count;
static int64_t s_last_render_ms;
typedef struct {
    char heading[COMPACT_TEXT_COLUMNS + 1];
    char text[COMPACT_TEXT_MAX + 1];
    char lines[COMPACT_TEXT_ROWS][COMPACT_TEXT_COLUMNS + 1];
    bool large_text;
    bool custom_message;
    uint32_t generation;
} compact_board_content_t;
static compact_board_content_t s_content;

#if CONFIG_EXAMPLE_BOARD_M5STICKC
enum { WIDTH = 160, HEIGHT = 80, SCALE = 1, MOSI = 15, SCLK = 13, CS = 5, DC = 23, RST = 18 };
static const char *BOARD_NAME = "M5STICKC";
static i2c_master_bus_handle_t s_power_bus;
static i2c_master_dev_handle_t s_power_chip;

static esp_err_t board_power_on(void)
{
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t axp;
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0, .sda_io_num = 21, .scl_io_num = 22,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &bus), TAG, "power bus");
    const i2c_device_config_t device = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x34, .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &device, &axp);
    if (err != ESP_OK) {
        i2c_del_master_bus(bus);
        return err;
    }
    // AXP192 LDO2 drives the backlight; LDO3 supplies the display. Preserve other rails.
    uint8_t reg = 0x12, rails = 0;
    err = i2c_master_transmit_receive(axp, &reg, 1, &rails, 1, 100);
    if (err == ESP_OK) {
        const uint8_t voltage[] = {0x28, 0xcc}; // Both display rails at 3.0 V.
        err = i2c_master_transmit(axp, voltage, sizeof(voltage), 100);
    }
    if (err == ESP_OK) {
        const uint8_t enable[] = {0x12, rails | 0x0c};
        err = i2c_master_transmit(axp, enable, sizeof(enable), 100);
    }
    if (err == ESP_OK) { s_power_bus = bus; s_power_chip = axp; }
    else { i2c_master_bus_rm_device(axp); i2c_del_master_bus(bus); }
    return err;
}
static esp_err_t board_brightness(int percent)
{
    uint8_t reg = 0x28, voltage = 0;
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_power_chip, &reg, 1, &voltage, 1, 100), TAG, "backlight voltage");
    uint8_t level = (uint8_t)(2 + percent / 10);
    const uint8_t value[] = {0x28, (uint8_t)((voltage & 0x0f) | (level << 4))};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_power_chip, value, sizeof(value), 100), TAG, "backlight brightness");
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_brightness = percent;
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}
#else
enum { WIDTH = 320, HEIGHT = 170, SCALE = 2, MOSI = 11, SCLK = 12, CS = 10, DC = 13, RST = 9 };
static const char *BOARD_NAME = "T-EMBED";

static esp_err_t board_power_on(void)
{
    const gpio_config_t pins = {
        .pin_bit_mask = (1ULL << 46) | (1ULL << 15),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&pins), TAG, "power pins");
    ESP_RETURN_ON_ERROR(gpio_set_level(15, 0), TAG, "backlight off");
    ESP_RETURN_ON_ERROR(gpio_set_level(46, 1), TAG, "power on");
    const ledc_timer_config_t timer = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK};
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    const ledc_channel_config_t channel = {.gpio_num = 15, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0};
    return ledc_channel_config(&channel);
}
static esp_err_t board_brightness(int percent)
{
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (uint32_t)percent * 1023 / 100), TAG, "backlight duty");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0), TAG, "backlight update");
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_brightness = percent;
    xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}
#endif

static bool flush_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event, void *ctx)
{
    (void)io; (void)event;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &wake);
    return wake == pdTRUE;
}

static uint32_t tick_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    size_t count = (size_t)(area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    lv_draw_sw_rgb565_swap(pixels, count);
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, pixels);
    if (err != ESP_OK || xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_ready = false;
        xSemaphoreGive(s_state_mutex);
        ESP_LOGE(TAG, "display transfer failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    ++s_render_count;
    s_last_render_ms = esp_timer_get_time() / 1000;
    xSemaphoreGive(s_state_mutex);
    lv_display_flush_ready(display);
}

static void display_task(void *ctx)
{
    (void)ctx;
    lv_init();
    lv_tick_set_cb(tick_ms);
    s_lv_display = lv_display_create(WIDTH, HEIGHT);
    if (s_lv_display == NULL) { vTaskDelete(NULL); return; }
    lv_display_set_color_format(s_lv_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_lv_display, s_pixels, NULL, WIDTH * 20 * sizeof(*s_pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_lv_display, flush);
#if CONFIG_EXAMPLE_BOARD_T_EMBED
    compact_workspace_start(WIDTH, HEIGHT, true, board_brightness);
#else
    compact_workspace_start(WIDTH, HEIGHT, false, board_brightness);
#endif
    compact_input_t input = {0};
    for (;;) {
        esp_openclaw_node_wifi_status_t wifi;
        esp_openclaw_node_wifi_get_status(&wifi);
        bool connected = wifi.connected && atomic_load(&s_connected);
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        compact_board_content_t content = s_content;
        xSemaphoreGive(s_state_mutex);
        if (content.custom_message) compact_workspace_message(content.heading, content.text, content.large_text, content.generation);
        uint32_t now = tick_ms();
        compact_workspace_poll(&wifi, connected, now);
#if CONFIG_EXAMPLE_BOARD_M5STICKC
        bool a = !gpio_get_level(37), b = !gpio_get_level(39);
        bool encoder = false;
        uint8_t quadrature = 3;
#else
        bool a = !gpio_get_level(0), b = false;
        bool encoder = true;
        uint8_t quadrature = (gpio_get_level(2) << 1) | gpio_get_level(1);
#endif
        compact_input_event_t event = compact_input_sample(&input, now, compact_workspace_revision(), a, b, encoder, quadrature);
        bool physical_event = event.move || event.select || event.home;
        if (physical_event) {
            compact_workspace_input(event);
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            ++s_input_count; s_last_input_ms = now;
            xSemaphoreGive(s_state_mutex);
        }
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        compact_console_input_t console = s_console_input;
        xSemaphoreGive(s_state_mutex);
        bool console_applied = console.pending && !physical_event && console.revision == compact_workspace_revision();
        if (console_applied) {
            compact_input_cancel_pending(&input);
            compact_workspace_input(console.event);
        }
        compact_workspace_poll(&wifi, connected, now);
        lv_timer_handler();
        lv_refr_now(s_lv_display);
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_ready = true;
        s_message_visible = content.custom_message && compact_workspace_message_visible();
        s_message_line_height = compact_workspace_message_line_height();
        if (s_message_visible) s_rendered_generation = content.generation;
        snprintf(s_view, sizeof(s_view), "%s", compact_workspace_view());
        s_workspace_revision = compact_workspace_revision();
        snprintf(s_console_selection, sizeof(s_console_selection), "%s", compact_workspace_selection());
        if (console.pending) compact_console_complete(&s_console_input, console.request_id, console_applied);
        xSemaphoreGive(s_state_mutex);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
    }
}

static esp_err_t display_status(char **out_payload_json, bool local_console)
{
    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool large_text = s_content.large_text;
    static const char *const console_outcomes[] = {"none", "queued", "applied", "stale"};
    bool ok = cJSON_AddBoolToObject(payload, "ready", s_ready) &&
        cJSON_AddStringToObject(payload, "heading", s_content.heading) &&
        cJSON_AddStringToObject(payload, "text", s_content.text) &&
        cJSON_AddStringToObject(payload, "textSize", large_text ? "large" : "normal") &&
        cJSON_AddStringToObject(payload, "view", s_view) &&
        cJSON_AddNumberToObject(payload, "physicalInputCount", s_input_count) &&
        cJSON_AddNumberToObject(payload, "workspaceRevision", s_workspace_revision) &&
        cJSON_AddNumberToObject(payload, "consoleInputCount", s_console_input.applied_count) &&
        cJSON_AddNumberToObject(payload, "consoleRequestId", s_console_input.request_id) &&
        cJSON_AddStringToObject(payload, "consoleOutcome", console_outcomes[s_console_input.outcome]) &&
        cJSON_AddNumberToObject(payload, "lastInputMs", (double)s_last_input_ms) &&
        cJSON_AddNumberToObject(payload, "brightness", s_brightness) &&
        cJSON_AddStringToObject(payload, "layout", "scroll") &&
        cJSON_AddNumberToObject(payload, "lineHeight", s_message_line_height) &&
        cJSON_AddNumberToObject(payload, "generation", s_content.generation) &&
        cJSON_AddNumberToObject(payload, "renderedGeneration", s_rendered_generation) &&
        cJSON_AddBoolToObject(payload, "rendered", s_ready && s_message_visible && s_rendered_generation == s_content.generation) &&
        cJSON_AddNumberToObject(payload, "renderCount", s_render_count) &&
        cJSON_AddNumberToObject(payload, "lastRenderMs", (double)s_last_render_ms);
    if (local_console) ok = ok && cJSON_AddStringToObject(payload, "selection", s_console_selection);
    xSemaphoreGive(s_state_mutex);
    ok = ok && cJSON_AddNumberToObject(payload, "headingMaxLength", COMPACT_TEXT_COLUMNS) &&
        cJSON_AddNumberToObject(payload, "textMaxLength", large_text ? COMPACT_LARGE_COLUMNS * COMPACT_LARGE_ROWS : COMPACT_TEXT_MAX) &&
        cJSON_AddNumberToObject(payload, "textColumns", large_text ? COMPACT_LARGE_COLUMNS : COMPACT_TEXT_COLUMNS) &&
        cJSON_AddNumberToObject(payload, "textRows", large_text ? COMPACT_LARGE_ROWS : COMPACT_TEXT_ROWS) &&
        cJSON_AddNumberToObject(payload, "width", WIDTH) &&
        cJSON_AddNumberToObject(payload, "height", HEIGHT);
    if (!ok) {
        cJSON_Delete(payload);
        return ESP_ERR_NO_MEM;
    }
    return esp_openclaw_node_example_take_json_payload(payload, out_payload_json);
}

esp_err_t compact_board_console_status(char **out_json) { return display_status(out_json, true); }

esp_err_t compact_board_console_input(compact_input_event_t event, uint32_t revision, uint32_t *request_id)
{
    if (request_id == NULL) return ESP_ERR_INVALID_ARG;
    *request_id = 0;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool accepted = s_ready && compact_console_submit(&s_console_input, event, revision, s_workspace_revision, request_id);
    if (accepted) xTaskNotifyGive(s_display_task);
    xSemaphoreGive(s_state_mutex);
    return accepted ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t handle_display_status(esp_openclaw_node_handle_t node, void *context,
    const char *params_json, size_t params_len, char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node; (void)context; (void)params_json; (void)params_len; (void)out_error;
    return display_status(out_payload_json, false);
}

static esp_err_t handle_display_show(esp_openclaw_node_handle_t node, void *context,
    const char *params_json, size_t params_len, char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node; (void)context; (void)params_len;
    cJSON *params = NULL;
    ESP_RETURN_ON_ERROR(esp_openclaw_node_example_parse_json_params(params_json, &params, out_error),
                        TAG, "invalid display params");
    cJSON *heading = cJSON_GetObjectItemCaseSensitive(params, "heading");
    cJSON *text = cJSON_GetObjectItemCaseSensitive(params, "text");
    cJSON *size = cJSON_GetObjectItemCaseSensitive(params, "textSize");
    compact_board_content_t content = {0};
    if (size != NULL && (!cJSON_IsString(size) ||
        (strcmp(size->valuestring, "normal") != 0 && strcmp(size->valuestring, "large") != 0))) {
        cJSON_Delete(params);
        out_error->code = "INVALID_PARAMS";
        out_error->message = "textSize must be normal or large";
        return ESP_ERR_INVALID_ARG;
    }
    content.large_text = size != NULL && strcmp(size->valuestring, "large") == 0;
    content.custom_message = true;
    size_t rows = content.large_text ? COMPACT_LARGE_ROWS : COMPACT_TEXT_ROWS;
    size_t columns = content.large_text ? COMPACT_LARGE_COLUMNS : COMPACT_TEXT_COLUMNS;
    char heading_line[1][COMPACT_TEXT_COLUMNS + 1];
    if (!cJSON_IsObject(params) || !cJSON_IsString(heading) || !cJSON_IsString(text) ||
        !compact_board_text_layout(heading->valuestring, 1, heading_line) ||
        strlen(text->valuestring) > rows * columns ||
        !compact_board_text_layout_width(text->valuestring, rows, columns, content.lines)) {
        cJSON_Delete(params);
        out_error->code = "INVALID_PARAMS";
        out_error->message = "heading must fit 25 columns; text must fit the selected size using supported ASCII characters";
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(content.heading, heading_line[0], sizeof(content.heading));
    snprintf(content.text, sizeof(content.text), "%s", text->valuestring);
    cJSON_Delete(params);
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (!s_ready) {
        xSemaphoreGive(s_state_mutex);
        out_error->code = "UNAVAILABLE";
        out_error->message = "display renderer is not ready";
        return ESP_ERR_INVALID_STATE;
    }
    content.generation = s_content.generation + 1;
    s_content = content;
    // The renderer publishes failure under this lock before deleting its task.
    xTaskNotifyGive(s_display_task);
    xSemaphoreGive(s_state_mutex);
    return display_status(out_payload_json, false);
}

esp_err_t compact_board_register_commands(esp_openclaw_node_handle_t node)
{
    const esp_openclaw_node_command_t show = {.name = "display.show", .handler = handle_display_show};
    const esp_openclaw_node_command_t status = {.name = "display.status", .handler = handle_display_status};
    ESP_RETURN_ON_ERROR(esp_openclaw_node_register_capability(node, "display"), TAG, "display capability");
    ESP_RETURN_ON_ERROR(esp_openclaw_node_register_command(node, &show), TAG, "display.show");
    ESP_RETURN_ON_ERROR(esp_openclaw_node_register_command(node, &status), TAG, "display.status");
    const esp_openclaw_reader_config_t reader = {.display_name = BOARD_NAME,
#if CONFIG_EXAMPLE_BOARD_M5STICKC
        .model_identifier = "m5stack-m5stickc",
#else
        .model_identifier = "lilygo-t-embed",
#endif
        .tls_server_name = CONFIG_EXAMPLE_GATEWAY_TLS_SERVER_NAME};
    esp_err_t err = esp_openclaw_reader_start(node, &reader);
    if (err != ESP_OK) ESP_LOGW(TAG, "reader unavailable: %s", esp_err_to_name(err));
    ESP_RETURN_ON_ERROR(compact_reader_console_register(), TAG, "reader console");
    return compact_workspace_console_register();
}

void compact_board_set_connected(bool connected)
{
    atomic_store(&s_connected, connected);
    esp_openclaw_reader_node_event(connected ? ESP_OPENCLAW_NODE_EVENT_CONNECTED : ESP_OPENCLAW_NODE_EVENT_DISCONNECTED);
}

esp_err_t compact_board_start(void)
{
    ESP_RETURN_ON_ERROR(board_power_on(), TAG, "board power");
    s_flush_done = xSemaphoreCreateBinary();
    s_state_mutex = xSemaphoreCreateMutex();
    s_pixels = heap_caps_calloc(WIDTH * 20, sizeof(*s_pixels), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_flush_done || !s_state_mutex || !s_pixels) return ESP_ERR_NO_MEM;
    snprintf(s_content.heading, sizeof(s_content.heading), "%s", BOARD_NAME);
    snprintf(s_content.text, sizeof(s_content.text), "%s", compact_board_home_text(false, false));
    compact_board_text_layout(s_content.text, COMPACT_TEXT_ROWS, s_content.lines);

    const spi_bus_config_t spi = {
        .mosi_io_num = MOSI, .miso_io_num = -1, .sclk_io_num = SCLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = WIDTH * HEIGHT * sizeof(*s_pixels),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &spi, SPI_DMA_CH_AUTO), TAG, "display SPI");
    esp_lcd_panel_io_handle_t io;
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = DC, .cs_gpio_num = CS, .pclk_hz = 20000000,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8, .trans_queue_depth = 1,
        .on_color_trans_done = flush_done, .user_ctx = s_flush_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io), TAG, "display IO");
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = RST,
#if CONFIG_EXAMPLE_BOARD_M5STICKC
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        .bits_per_pixel = 16,
    };
#if CONFIG_EXAMPLE_BOARD_M5STICKC
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7735(io, &panel_config, &s_panel), TAG, "ST7735");
#else
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_config, &s_panel), TAG, "ST7789");
#endif
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "display reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "display init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, true), TAG, "display rotation");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, false, true), TAG, "display mirror");
#if CONFIG_EXAMPLE_BOARD_M5STICKC
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 1, 26), TAG, "display gap");
#else
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 0, 35), TAG, "display gap");
#endif
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "display inversion");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "display on");
    const gpio_config_t input = {
#if CONFIG_EXAMPLE_BOARD_M5STICKC
        .pin_bit_mask = (1ULL << 37) | (1ULL << 39),
        .pull_up_en = GPIO_PULLUP_DISABLE,
#else
        .pin_bit_mask = (1ULL << 0) | (1ULL << 1) | (1ULL << 2),
        .pull_up_en = GPIO_PULLUP_ENABLE,
#endif
        .mode = GPIO_MODE_INPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&input), TAG, "board inputs");
    if (xTaskCreate(display_task, "board_display", 8192, NULL, 3, &s_display_task) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "%s status display ready", BOARD_NAME);
    return ESP_OK;
}
