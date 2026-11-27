// SPDX-License-Identifier: Apache-2.0

#include "compact_board.h"
#include "compact_board_text.h"

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
    i2c_master_bus_rm_device(axp);
    i2c_del_master_bus(bus);
    return err;
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
    return gpio_set_level(46, 1);
}
#endif

// Five columns per glyph, least significant bit at the top.
static const uint8_t FONT[][5] = {
    {0x3e,0x51,0x49,0x45,0x3e}, {0x00,0x42,0x7f,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4b,0x31},
    {0x18,0x14,0x12,0x7f,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3c,0x4a,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1e},
    {0x7e,0x11,0x11,0x11,0x7e}, {0x7f,0x49,0x49,0x49,0x36},
    {0x3e,0x41,0x41,0x41,0x22}, {0x7f,0x41,0x41,0x22,0x1c},
    {0x7f,0x49,0x49,0x49,0x41}, {0x7f,0x09,0x09,0x09,0x01},
    {0x3e,0x41,0x49,0x49,0x7a}, {0x7f,0x08,0x08,0x08,0x7f},
    {0x00,0x41,0x7f,0x41,0x00}, {0x20,0x40,0x41,0x3f,0x01},
    {0x7f,0x08,0x14,0x22,0x41}, {0x7f,0x40,0x40,0x40,0x40},
    {0x7f,0x02,0x0c,0x02,0x7f}, {0x7f,0x04,0x08,0x10,0x7f},
    {0x3e,0x41,0x41,0x41,0x3e}, {0x7f,0x09,0x09,0x09,0x06},
    {0x3e,0x41,0x51,0x21,0x5e}, {0x7f,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7f,0x01,0x01},
    {0x3f,0x40,0x40,0x40,0x3f}, {0x1f,0x20,0x40,0x20,0x1f},
    {0x3f,0x40,0x38,0x40,0x3f}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},
};

static const char PUNCTUATION[] = " .,-_:!?/+%()=";
static const uint8_t PUNCTUATION_FONT[][5] = {
    {0,0,0,0,0}, {0,0x60,0x60,0,0}, {0,0x50,0x30,0,0},
    {8,8,8,8,8}, {0x40,0x40,0x40,0x40,0x40}, {0,0x36,0x36,0,0},
    {0,0,0x5f,0,0}, {2,1,0x51,9,6}, {0x20,0x10,8,4,2},
    {8,8,0x3e,8,8}, {0x23,0x13,8,0x64,0x62}, {0,0x1c,0x22,0x41,0},
    {0,0x41,0x22,0x1c,0}, {0x14,0x14,0x14,0x14,0x14},
};

static void draw_text_at(int x, int y, int scale, const char *text, uint16_t color)
{
    // esp_lcd transmits RGB565 most significant byte first.
    color = (uint16_t)((color << 8) | (color >> 8));
    for (; *text && x + 5 * scale < WIDTH; ++text, x += 6 * scale) {
        unsigned char c = (unsigned char)*text;
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        const uint8_t *glyph = NULL;
        if (c >= '0' && c <= '9') glyph = FONT[c - '0'];
        if (c >= 'A' && c <= 'Z') glyph = FONT[10 + c - 'A'];
        const char *punctuation = strchr(PUNCTUATION, c);
        if (punctuation != NULL) glyph = PUNCTUATION_FONT[punctuation - PUNCTUATION];
        for (int col = 0; col < 5; ++col) {
            uint8_t bits = glyph ? glyph[col] : 0;
            for (int bit = 0; bit < 7; ++bit) {
                if (!(bits & (1U << bit))) continue;
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale; ++dx) {
                        int px = x + col * scale + dx, py = y + bit * scale + dy;
                        if (py < HEIGHT) s_pixels[py * WIDTH + px] = color;
                    }
                }
            }
        }
    }
}

static void draw_text(int row, const char *text, uint16_t color)
{
    draw_text_at(3 * SCALE, (3 + row * 10) * SCALE, SCALE, text, color);
}

static bool flush_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event, void *ctx)
{
    (void)io;
    (void)event;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &wake);
    return wake == pdTRUE;
}

static void display_task(void *ctx)
{
    (void)ctx;
    for (;;) {
        esp_openclaw_node_wifi_status_t wifi;
        esp_openclaw_node_wifi_get_status(&wifi);
        bool connected = wifi.connected && atomic_load(&s_connected);
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        if (!s_content.custom_message) {
            const char *home = compact_board_home_text(wifi.connected, connected);
            if (strcmp(home, s_content.text) != 0) {
                snprintf(s_content.text, sizeof(s_content.text), "%s", home);
                compact_board_text_layout(home, COMPACT_TEXT_ROWS, s_content.lines);
                ++s_content.generation;
            }
        }
        compact_board_content_t content = s_content;
        xSemaphoreGive(s_state_mutex);
        memset(s_pixels, 0, WIDTH * HEIGHT * sizeof(*s_pixels));
        draw_text(0, content.heading, 0x07ff);
        if (content.large_text) {
            for (int row = 0; row < COMPACT_LARGE_ROWS; ++row) {
                draw_text_at(3 * SCALE, (14 + row * 20) * SCALE, 2 * SCALE, content.lines[row], 0xffff);
            }
        } else {
            for (int row = 0; row < COMPACT_TEXT_ROWS; ++row) draw_text(row + 1, content.lines[row], 0xffff);
        }
        char network[COMPACT_TEXT_COLUMNS + 1];
        snprintf(network, sizeof(network), "WIFI %s", wifi.connected ? wifi.ip : "OFFLINE");
        draw_text(6, network, wifi.connected ? 0x07e0 : 0xfd20);
        draw_text(7, connected ? "GATEWAY CONNECTED" : "GATEWAY OFFLINE", 0xffff);
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_pixels);
        if (err != ESP_OK || xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
            // An incomplete DMA transfer still owns the framebuffer. Do not overwrite it.
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_ready = false;
            xSemaphoreGive(s_state_mutex);
            ESP_LOGE(TAG, "display transfer failed: %s", esp_err_to_name(err));
            vTaskDelete(NULL);
        }
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_ready = true;
        s_rendered_generation = content.generation;
        ++s_render_count;
        s_last_render_ms = esp_timer_get_time() / 1000;
        xSemaphoreGive(s_state_mutex);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }
}

static esp_err_t display_status(char **out_payload_json)
{
    cJSON *payload = cJSON_CreateObject();
    if (payload == NULL) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool large_text = s_content.large_text;
    bool ok = cJSON_AddBoolToObject(payload, "ready", s_ready) &&
        cJSON_AddStringToObject(payload, "heading", s_content.heading) &&
        cJSON_AddStringToObject(payload, "text", s_content.text) &&
        cJSON_AddStringToObject(payload, "textSize", large_text ? "large" : "normal") &&
        cJSON_AddStringToObject(payload, "view", s_content.custom_message ? "message" : "status") &&
        cJSON_AddNumberToObject(payload, "generation", s_content.generation) &&
        cJSON_AddNumberToObject(payload, "renderedGeneration", s_rendered_generation) &&
        cJSON_AddBoolToObject(payload, "rendered", s_ready && s_rendered_generation == s_content.generation) &&
        cJSON_AddNumberToObject(payload, "renderCount", s_render_count) &&
        cJSON_AddNumberToObject(payload, "lastRenderMs", (double)s_last_render_ms);
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

static esp_err_t handle_display_status(esp_openclaw_node_handle_t node, void *context,
    const char *params_json, size_t params_len, char **out_payload_json,
    esp_openclaw_node_error_t *out_error)
{
    (void)node; (void)context; (void)params_json; (void)params_len; (void)out_error;
    return display_status(out_payload_json);
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
    return display_status(out_payload_json);
}

esp_err_t compact_board_register_commands(esp_openclaw_node_handle_t node)
{
    const esp_openclaw_node_command_t show = {.name = "display.show", .handler = handle_display_show};
    const esp_openclaw_node_command_t status = {.name = "display.status", .handler = handle_display_status};
    ESP_RETURN_ON_ERROR(esp_openclaw_node_register_capability(node, "display"), TAG, "display capability");
    ESP_RETURN_ON_ERROR(esp_openclaw_node_register_command(node, &show), TAG, "display.show");
    return esp_openclaw_node_register_command(node, &status);
}

void compact_board_set_connected(bool connected)
{
    atomic_store(&s_connected, connected);
}

esp_err_t compact_board_start(void)
{
    ESP_RETURN_ON_ERROR(board_power_on(), TAG, "board power");
    s_flush_done = xSemaphoreCreateBinary();
    s_state_mutex = xSemaphoreCreateMutex();
    s_pixels = heap_caps_calloc(WIDTH * HEIGHT, sizeof(*s_pixels), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
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
#if CONFIG_EXAMPLE_BOARD_T_EMBED
    ESP_RETURN_ON_ERROR(gpio_set_level(15, 1), TAG, "backlight on");
#endif
    if (xTaskCreate(display_task, "board_display", 4096, NULL, 3, &s_display_task) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "%s status display ready", BOARD_NAME);
    return ESP_OK;
}
