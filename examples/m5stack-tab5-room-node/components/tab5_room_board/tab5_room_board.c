#include "tab5_room_board.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>

#include "bsp/esp-bsp.h"
#include "cJSON.h"
#include "driver/i2s_tdm.h"
#include "driver/i2s_std.h"
#include "driver/ppa.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7121.h"
#include "esp_lcd_touch_st7123.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_jpeg_enc.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "esp_vfs_fat.h"
#include "linux/videodev2.h"
#include "mbedtls/base64.h"

#define TAG "tab5_room_board"
#define ST712X_TOUCH_ADDRESS 0x55
#define GT911_BACKUP_ADDRESS 0x14
#define PANEL_RESET_DELAY_MS 100
#define TOUCH_STARTUP_DELAY_MS 500
#define INA226_ADDRESS 0x41
#define RX8130_ADDRESS 0x32
#define CAMERA_SENSOR_WIDTH 1280U
#define CAMERA_SENSOR_HEIGHT 720U
#define CAMERA_MAX_PIXELS (1024U * 1024U)
#define CAMERA_DIMENSION_ALIGNMENT 8U
#define CAMERA_ALIGN_UP(value, alignment) (((value) + (alignment) - 1) & ~((alignment) - 1))

_Static_assert(
    BSP_CAMERA_ROTATION == 0 || BSP_CAMERA_ROTATION == 90 ||
    BSP_CAMERA_ROTATION == 180 || BSP_CAMERA_ROTATION == 270,
    "BSP camera rotation must be a PPA-supported right angle");
_Static_assert(
    CAMERA_SENSOR_WIDTH * CAMERA_SENSOR_HEIGHT <= CAMERA_MAX_PIXELS,
    "Tab5 camera output must fit the one-megapixel command contract");

#if CONFIG_ESP_HOSTED_SDIO_PIN_D1 == CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE
#error "Tab5 SDIO D1 and C6 reset must use different GPIOs"
#endif

#define RETURN_NULL_ON_ERROR(expr, message) do { \
    esp_err_t _err = (expr); \
    if (_err != ESP_OK) { \
        ESP_LOGE(TAG, "%s: %s", (message), esp_err_to_name(_err)); \
        return NULL; \
    } \
} while (0)

typedef enum {
    TAB5_PANEL_UNKNOWN,
    TAB5_PANEL_ILI9881C_GT911,
    TAB5_PANEL_ST7123,
    TAB5_PANEL_ST7121,
} tab5_panel_t;

static tab5_panel_t panel_type;
static esp_ldo_channel_handle_t dsi_ldo;
static sensor_handle_t imu_handle;
static portMUX_TYPE imu_mux = portMUX_INITIALIZER_UNLOCKED;
static axis3_t imu_acceleration_g;
static axis3_t imu_angular_velocity_dps;
static bool imu_acceleration_valid;
static bool imu_angular_velocity_valid;
static bool sd_mounted;
static esp_err_t usb_status = ESP_ERR_NOT_SUPPORTED;
#if CONFIG_OPENCLAW_TAB5_RS485
static esp_err_t rs485_status = ESP_ERR_NOT_SUPPORTED;
#endif
static i2c_master_dev_handle_t ina226;
static i2c_master_dev_handle_t rx8130;
/* The room-node camera lease bounds this board to one blocking transaction;
 * the SRM client and its alignment contract live for the board lifetime. */
static ppa_client_handle_t camera_ppa_srm;
static size_t camera_ppa_alignment;

static esp_err_t init_camera_transformer(void)
{
    if (camera_ppa_srm != NULL) return ESP_OK;
    ESP_RETURN_ON_ERROR(
        esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &camera_ppa_alignment),
        TAG,
        "camera PSRAM alignment");
    ESP_RETURN_ON_FALSE(
        camera_ppa_alignment != 0,
        ESP_ERR_INVALID_STATE,
        TAG,
        "camera PSRAM alignment is zero");
    const ppa_client_config_t config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    return ppa_register_client(&config, &camera_ppa_srm);
}

static esp_err_t add_i2c_device(uint8_t address, i2c_master_dev_handle_t *device)
{
    if (*device != NULL) {
        return ESP_OK;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, device);
}

static esp_err_t read_registers(
    i2c_master_dev_handle_t device,
    uint8_t reg,
    uint8_t *data,
    size_t len)
{
    return i2c_master_transmit_receive(device, &reg, 1, data, len, 100);
}

static bool valid_bcd(uint8_t value, uint8_t max)
{
    uint8_t decoded = (uint8_t)(((value >> 4) * 10) + (value & 0x0f));
    return (value & 0x0f) <= 9 && (value >> 4) <= 9 && decoded <= max;
}

static uint8_t bcd_decode(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10) + (value & 0x0f));
}

static tab5_panel_t detect_panel(void)
{
    ESP_ERROR_CHECK(bsp_i2c_init());
    ESP_ERROR_CHECK(bsp_feature_enable(BSP_FEATURE_LCD, false));
    ESP_ERROR_CHECK(bsp_feature_enable(BSP_FEATURE_TOUCH, false));
    vTaskDelay(pdMS_TO_TICKS(PANEL_RESET_DELAY_MS));
    ESP_ERROR_CHECK(bsp_feature_enable(BSP_FEATURE_LCD, true));
    ESP_ERROR_CHECK(bsp_feature_enable(BSP_FEATURE_TOUCH, true));
    vTaskDelay(pdMS_TO_TICKS(TOUCH_STARTUP_DELAY_MS));
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (i2c_master_probe(bus, ST712X_TOUCH_ADDRESS, 100) != ESP_OK) {
        if (i2c_master_probe(bus, GT911_BACKUP_ADDRESS, 100) == ESP_OK) {
            return TAB5_PANEL_ILI9881C_GT911;
        }
        ESP_LOGW(TAG, "panel pre-probe found no known touch controller; deferring to maintained BSP");
        return TAB5_PANEL_UNKNOWN;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_ST7123_CONFIG();
    io_cfg.scl_speed_hz = 100000;
    uint8_t version = 0;
    if (esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io) == ESP_OK) {
        (void)esp_lcd_panel_io_rx_param(io, 0x0000, &version, 1);
        esp_lcd_panel_io_del(io);
    }
    ESP_LOGI(TAG, "ST712x touch firmware revision %u", version);
    return version == 1 ? TAB5_PANEL_ST7121 : TAB5_PANEL_ST7123;
}

static lv_display_t *start_st7121_display(void)
{
    RETURN_NULL_ON_ERROR(bsp_feature_enable(BSP_FEATURE_LCD, true), "LCD power");
    RETURN_NULL_ON_ERROR(bsp_display_brightness_init(), "backlight init");
    const esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = BSP_MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = BSP_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    RETURN_NULL_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &dsi_ldo), "DSI LDO");

    esp_lcd_dsi_bus_handle_t dsi = NULL;
    const esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = 965,
    };
    RETURN_NULL_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &dsi), "DSI bus");

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    RETURN_NULL_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi, &dbi_cfg, &io), "DBI IO");

    const esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 70,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 2,
        .video_timing = {
            .h_size = BSP_LCD_H_RES,
            .v_size = BSP_LCD_V_RES,
            .hsync_pulse_width = 2,
            .hsync_back_porch = 40,
            .hsync_front_porch = 40,
            .vsync_pulse_width = 20,
            .vsync_back_porch = 24,
            .vsync_front_porch = 200,
        },
        .flags.use_dma2d = true,
    };
    const st7121_vendor_config_t vendor = {
        .mipi_config = {.dsi_bus = dsi, .dpi_config = &dpi_cfg},
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor,
    };
    esp_lcd_panel_handle_t panel = NULL;
    RETURN_NULL_ON_ERROR(esp_lcd_new_panel_st7121(io, &panel_cfg, &panel), "ST7121 panel");
    RETURN_NULL_ON_ERROR(esp_lcd_panel_reset(panel), "panel reset");
    RETURN_NULL_ON_ERROR(esp_lcd_panel_init(panel), "panel init");
    RETURN_NULL_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), "panel on");

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_affinity = 1;
    RETURN_NULL_ON_ERROR(lvgl_port_init(&port_cfg), "LVGL port");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = BSP_LCD_H_RES * 40,
        .double_buffer = true,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .monochrome = false,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        .flags = {.buff_dma = true, .buff_spiram = false, .sw_rotate = true},
    };
    const lvgl_port_display_dsi_cfg_t dsi_cfg = {.flags.avoid_tearing = false};
    lv_display_t *display = lvgl_port_add_disp_dsi(&display_cfg, &dsi_cfg);
    if (display == NULL) {
        return NULL;
    }

    esp_lcd_panel_io_handle_t touch_io = NULL;
    const esp_lcd_panel_io_i2c_config_t touch_io_cfg = ESP_LCD_TOUCH_IO_I2C_ST7123_CONFIG();
    if (esp_lcd_new_panel_io_i2c(bsp_i2c_get_handle(), &touch_io_cfg, &touch_io) == ESP_OK) {
        const esp_lcd_touch_config_t touch_cfg = {
            .x_max = BSP_LCD_H_RES,
            .y_max = BSP_LCD_V_RES,
            .rst_gpio_num = GPIO_NUM_NC,
            .int_gpio_num = BSP_LCD_TOUCH_INT,
            .levels = {.reset = 0, .interrupt = 0},
        };
        esp_lcd_touch_handle_t touch = NULL;
        if (esp_lcd_touch_new_i2c_st7123(touch_io, &touch_cfg, &touch) == ESP_OK) {
            const lvgl_port_touch_cfg_t lv_touch_cfg = {.disp = display, .handle = touch};
            (void)lvgl_port_add_touch(&lv_touch_cfg);
        }
    }
    return display;
}

static lv_display_t *tab5_display_start(void *ctx)
{
    (void)ctx;
    panel_type = detect_panel();
    bsp_display_cfg_t display_cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * 40,
        .double_buffer = true,
        .flags = {.buff_dma = false, .buff_spiram = true, .sw_rotate = true},
    };
    /* Hosted SDIO/Wi-Fi is CPU0-heavy during startup; keep software-rotated
     * LVGL on CPU1 so both idle tasks retain watchdog service time. */
    display_cfg.lvgl_port_cfg.task_affinity = 1;
    lv_display_t *display = panel_type == TAB5_PANEL_ST7121
        ? start_st7121_display()
        : bsp_display_start_with_config(&display_cfg);
    if (display != NULL) {
        bsp_display_rotate(display, LV_DISPLAY_ROTATION_90);
    }
    return display;
}

static bool display_lock(void *ctx, uint32_t timeout_ms)
{
    (void)ctx;
    return panel_type == TAB5_PANEL_ST7121
        ? lvgl_port_lock(timeout_ms)
        : bsp_display_lock(timeout_ms);
}

static void display_unlock(void *ctx)
{
    (void)ctx;
    if (panel_type == TAB5_PANEL_ST7121) {
        lvgl_port_unlock();
    } else {
        bsp_display_unlock();
    }
}

static esp_err_t display_brightness(void *ctx, int percent)
{
    (void)ctx;
    return bsp_display_brightness_set(percent);
}

static esp_err_t tab5_audio_open(void *ctx, esp_openclaw_room_audio_handles_t *handles)
{
    (void)ctx;
    ESP_RETURN_ON_ERROR(
        bsp_feature_enable(BSP_FEATURE_SPEAKER, true),
        TAG,
        "speaker power enable");
    i2s_chan_handle_t tx = NULL;
    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "I2S channels");

    const i2s_std_config_t tx_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK, .bclk = BSP_I2S_SCLK, .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT, .din = BSP_I2S_DSIN,
        },
    };
    const i2s_tdm_config_t rx_cfg = {
        .clk_cfg = {
            .sample_rate_hz = 48000,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3,
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .bit_shift = true,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM,
        },
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK, .bclk = BSP_I2S_SCLK, .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT, .din = BSP_I2S_DSIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &tx_cfg), TAG, "I2S playback");
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(rx, &rx_cfg), TAG, "four-slot TDM capture");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "I2S TX enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "I2S RX enable");
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&(audio_codec_i2s_cfg_t){
        .port = CONFIG_BSP_I2S_NUM, .tx_handle = tx, .rx_handle = rx,
    });
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_NO_MEM, TAG, "codec data interface");
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "I2C");

    const audio_codec_ctrl_if_t *speaker_ctrl = audio_codec_new_i2c_ctrl(&(audio_codec_i2c_cfg_t){
        .port = BSP_I2C_NUM, .addr = ES8388_CODEC_DEFAULT_ADDR, .bus_handle = bsp_i2c_get_handle(),
    });
    const audio_codec_if_t *speaker = es8388_codec_new(&(es8388_codec_cfg_t){
        .ctrl_if = speaker_ctrl, .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC, .master_mode = false,
    });
    handles->playback = esp_codec_dev_new(&(esp_codec_dev_cfg_t){
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = speaker, .data_if = data,
    });

    const audio_codec_ctrl_if_t *mic_ctrl = audio_codec_new_i2c_ctrl(&(audio_codec_i2c_cfg_t){
        .port = BSP_I2C_NUM, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = bsp_i2c_get_handle(),
    });
    const audio_codec_if_t *microphone = es7210_codec_new(&(es7210_codec_cfg_t){
        .ctrl_if = mic_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    });
    handles->record = esp_codec_dev_new(&(esp_codec_dev_cfg_t){
        .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = microphone, .data_if = data,
    });
    return handles->record != NULL && handles->playback != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static void imu_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (data == NULL) return;
    const sensor_data_t *sample = data;
    taskENTER_CRITICAL(&imu_mux);
    if (id == SENSOR_ACCE_DATA_READY) {
        imu_acceleration_g = sample->acce;
        imu_acceleration_valid = true;
    } else if (id == SENSOR_GYRO_DATA_READY) {
        imu_angular_velocity_dps = sample->gyro;
        imu_angular_velocity_valid = true;
    }
    taskEXIT_CRITICAL(&imu_mux);
}

static esp_err_t prepare_runtime(void *ctx)
{
    (void)ctx;
    bsp_sensor_config_t imu_cfg = {.type = IMU_ID, .mode = MODE_POLLING, .period = 250};
    esp_err_t err = bsp_sensor_init(&imu_cfg, &imu_handle);
    if (err == ESP_OK) {
        iot_sensor_handler_register(imu_handle, imu_event, NULL);
        err = iot_sensor_start(imu_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BMI270 sensor hub unavailable: %s", esp_err_to_name(err));
    }

    esp_err_t sd_err = bsp_sdcard_mount();
    sd_mounted = sd_err == ESP_OK || sd_err == ESP_ERR_INVALID_STATE;
#if CONFIG_OPENCLAW_TAB5_USB_HOST
    usb_status = bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true);
#else
    const uart_config_t uart_cfg = {
        .baud_rate = CONFIG_OPENCLAW_TAB5_RS485_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    rs485_status = uart_driver_install(UART_NUM_1, 256, 0, 0, NULL, 0);
    if (rs485_status == ESP_OK) rs485_status = uart_param_config(UART_NUM_1, &uart_cfg);
    if (rs485_status == ESP_OK) rs485_status = uart_set_pin(UART_NUM_1, 20, 21, 34, UART_PIN_NO_CHANGE);
    if (rs485_status == ESP_OK) rs485_status = uart_set_mode(UART_NUM_1, UART_MODE_RS485_HALF_DUPLEX);
#endif
    return ESP_OK;
}

static esp_err_t prepare_network(void *ctx)
{
    (void)ctx;
    esp_err_t err = bsp_feature_enable(BSP_FEATURE_WIFI, true);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return err;
}

static bool empty_object(const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    bool ok = cJSON_IsObject(root) && root->child == NULL;
    cJSON_Delete(root);
    return ok;
}

static esp_err_t hardware_status(
    esp_openclaw_node_handle_t node, void *context, const char *params_json,
    size_t params_len, char **out, esp_openclaw_node_error_t *error)
{
    (void)node;
    (void)context;
    if (!empty_object(params_json, params_len)) {
        error->code = "INVALID_PARAMS";
        error->message = "hardware.status accepts only {}";
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *imu = cJSON_AddObjectToObject(root, "imu");
    axis3_t acceleration;
    axis3_t angular_velocity;
    bool acceleration_valid;
    bool angular_velocity_valid;
    taskENTER_CRITICAL(&imu_mux);
    acceleration = imu_acceleration_g;
    angular_velocity = imu_angular_velocity_dps;
    acceleration_valid = imu_acceleration_valid;
    angular_velocity_valid = imu_angular_velocity_valid;
    taskEXIT_CRITICAL(&imu_mux);
    if (acceleration_valid || angular_velocity_valid) {
        cJSON_AddStringToObject(imu, "status", "ok");
        if (acceleration_valid) {
            cJSON *accel = cJSON_AddObjectToObject(imu, "accelerationMetersPerSecondSquared");
            cJSON_AddNumberToObject(accel, "x", acceleration.x * 9.80665);
            cJSON_AddNumberToObject(accel, "y", acceleration.y * 9.80665);
            cJSON_AddNumberToObject(accel, "z", acceleration.z * 9.80665);
        }
        if (angular_velocity_valid) {
            const double degrees_to_radians = 0.017453292519943295;
            cJSON *gyro = cJSON_AddObjectToObject(imu, "angularVelocityRadiansPerSecond");
            cJSON_AddNumberToObject(gyro, "x", angular_velocity.x * degrees_to_radians);
            cJSON_AddNumberToObject(gyro, "y", angular_velocity.y * degrees_to_radians);
            cJSON_AddNumberToObject(gyro, "z", angular_velocity.z * degrees_to_radians);
        }
    } else {
        cJSON_AddStringToObject(imu, "status", "unavailable");
        cJSON_AddStringToObject(imu, "code", "NO_SAMPLE");
    }
    cJSON *rtc = cJSON_AddObjectToObject(root, "rtc");
    uint8_t rtc_time[7] = {0};
    uint8_t rtc_flags = 0xff;
    esp_err_t rtc_err = add_i2c_device(RX8130_ADDRESS, &rx8130);
    if (rtc_err == ESP_OK) rtc_err = read_registers(rx8130, 0x10, rtc_time, sizeof(rtc_time));
    if (rtc_err == ESP_OK) rtc_err = read_registers(rx8130, 0x1d, &rtc_flags, 1);
    bool rtc_valid = rtc_err == ESP_OK && (rtc_flags & (1 << 1)) == 0 &&
        valid_bcd(rtc_time[0] & 0x7f, 59) && valid_bcd(rtc_time[1] & 0x7f, 59) &&
        valid_bcd(rtc_time[2] & 0x3f, 23) && valid_bcd(rtc_time[4] & 0x3f, 31) &&
        valid_bcd(rtc_time[5] & 0x1f, 12) && valid_bcd(rtc_time[6], 99);
    cJSON_AddStringToObject(rtc, "status", rtc_err == ESP_OK ? "ok" : "unavailable");
    cJSON_AddBoolToObject(rtc, "valid", rtc_valid);
    if (rtc_valid) {
        char iso[32];
        snprintf(
            iso, sizeof(iso), "20%02u-%02u-%02uT%02u:%02u:%02u",
            bcd_decode(rtc_time[6]), bcd_decode(rtc_time[5] & 0x1f),
            bcd_decode(rtc_time[4] & 0x3f), bcd_decode(rtc_time[2] & 0x3f),
            bcd_decode(rtc_time[1] & 0x7f), bcd_decode(rtc_time[0] & 0x7f));
        cJSON_AddStringToObject(rtc, "time", iso);
    } else {
        cJSON_AddStringToObject(rtc, "code", rtc_err == ESP_OK ? "VOLTAGE_LOW" : "READ_FAILED");
    }
    cJSON *power = cJSON_AddObjectToObject(root, "powerPath");
    uint8_t bus_raw[2] = {0};
    uint8_t shunt_raw[2] = {0};
    esp_err_t power_err = add_i2c_device(INA226_ADDRESS, &ina226);
    if (power_err == ESP_OK) power_err = read_registers(ina226, 0x02, bus_raw, 2);
    if (power_err == ESP_OK) power_err = read_registers(ina226, 0x01, shunt_raw, 2);
    if (power_err == ESP_OK) {
        uint16_t bus_value = ((uint16_t)bus_raw[0] << 8) | bus_raw[1];
        int16_t shunt_value = (int16_t)(((uint16_t)shunt_raw[0] << 8) | shunt_raw[1]);
        double volts = bus_value * 0.00125;
        double amps = (shunt_value * 0.0000025) / 0.005;
        cJSON_AddStringToObject(power, "status", "ok");
        cJSON_AddNumberToObject(power, "voltageVolts", volts);
        cJSON_AddNumberToObject(power, "currentAmps", amps);
        cJSON_AddNumberToObject(power, "powerWatts", volts * amps);
        cJSON_AddNullToObject(power, "source");
        cJSON_AddNullToObject(power, "charging");
    } else {
        cJSON_AddStringToObject(power, "status", "unavailable");
        cJSON_AddStringToObject(power, "code", "READ_FAILED");
    }
    cJSON *sd = cJSON_AddObjectToObject(root, "sd");
    cJSON_AddStringToObject(sd, "status", sd_mounted ? "ok" : "unavailable");
    cJSON_AddBoolToObject(sd, "mounted", sd_mounted);
    if (sd_mounted) {
        uint64_t total = 0;
        uint64_t free_bytes = 0;
        if (esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &free_bytes) == ESP_OK) {
            cJSON_AddNumberToObject(sd, "capacityBytes", (double)total);
            cJSON_AddNumberToObject(sd, "freeBytes", (double)free_bytes);
        }
    } else {
        cJSON_AddStringToObject(sd, "code", "NOT_MOUNTED");
    }
    cJSON *usb = cJSON_AddObjectToObject(root, "usbHost");
#if CONFIG_OPENCLAW_TAB5_USB_HOST
    cJSON_AddStringToObject(usb, "status", usb_status == ESP_OK ? "ok" : "unavailable");
    cJSON_AddArrayToObject(usb, "devices");
    if (usb_status != ESP_OK) cJSON_AddStringToObject(usb, "code", "HOST_START_FAILED");
#else
    cJSON_AddStringToObject(usb, "status", "disabled");
    cJSON_AddStringToObject(usb, "code", "GPIO20_RS485_CONFLICT");
#endif
    cJSON *rs485 = cJSON_AddObjectToObject(root, "rs485");
#if CONFIG_OPENCLAW_TAB5_RS485
    cJSON_AddStringToObject(rs485, "status", rs485_status == ESP_OK ? "ok" : "unavailable");
    cJSON_AddNumberToObject(rs485, "baud", CONFIG_OPENCLAW_TAB5_RS485_BAUD);
    cJSON_AddBoolToObject(rs485, "ready", rs485_status == ESP_OK);
    if (rs485_status != ESP_OK) cJSON_AddStringToObject(rs485, "code", "UART_INIT_FAILED");
#else
    cJSON_AddStringToObject(rs485, "status", "disabled");
    cJSON_AddStringToObject(rs485, "code", "GPIO20_USB_CONFLICT");
#endif
    *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (*out == NULL || strlen(*out) > 8192) {
        free(*out);
        *out = NULL;
        error->code = "INTERNAL";
        error->message = "hardware.status exceeded its 8 KiB bound";
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t camera_list(
    esp_openclaw_node_handle_t node, void *context, const char *params_json,
    size_t params_len, char **out, esp_openclaw_node_error_t *error)
{
    (void)node;
    (void)context;
    if (!empty_object(params_json, params_len)) {
        error->code = "INVALID_PARAMS";
        error->message = "camera.list accepts only {}";
        return ESP_ERR_INVALID_ARG;
    }
    *out = strdup("{\"devices\":[{\"id\":\"tab5-front\",\"name\":\"Tab5 Camera\",\"position\":\"front\",\"deviceType\":\"camera\"}]}");
    return *out != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

typedef struct {
    int fd;
    uint8_t *buffers[2];
    size_t lengths[2];
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    struct v4l2_buffer frame;
} camera_frame_t;

static void close_camera_frame(camera_frame_t *camera)
{
    if (camera->fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        (void)ioctl(camera->fd, VIDIOC_STREAMOFF, &type);
        for (size_t i = 0; i < 2; ++i) {
            if (camera->buffers[i] != NULL) {
                munmap(camera->buffers[i], camera->lengths[i]);
            }
        }
        close(camera->fd);
    }
    memset(camera, 0, sizeof(*camera));
    camera->fd = -1;
}

static esp_err_t capture_rgb565_frame(int delay_ms, camera_frame_t *camera)
{
    static bool camera_started;
    if (!camera_started) {
        ESP_RETURN_ON_ERROR(bsp_camera_start(NULL), TAG, "camera start");
        camera_started = true;
    }
    memset(camera, 0, sizeof(*camera));
    camera->fd = open(BSP_CAMERA_DEVICE, O_RDONLY);
    if (camera->fd < 0) return ESP_FAIL;

    struct v4l2_format format = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    if (ioctl(camera->fd, VIDIOC_G_FMT, &format) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT failed");
        goto fail;
    }
    if (format.fmt.pix.width != CAMERA_SENSOR_WIDTH ||
        format.fmt.pix.height != CAMERA_SENSOR_HEIGHT) {
        ESP_LOGE(
            TAG,
            "unsupported camera default: %lux%lu (expected %ux%u)",
            (unsigned long)format.fmt.pix.width,
            (unsigned long)format.fmt.pix.height,
            CAMERA_SENSOR_WIDTH,
            CAMERA_SENSOR_HEIGHT);
        goto fail;
    }
    /* maxWidth constrains the post-rotation JPEG, never the SC202CS mode. */
    struct v4l2_format requested = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix = {
            .width = format.fmt.pix.width,
            .height = format.fmt.pix.height,
            .pixelformat = V4L2_PIX_FMT_RGB565,
        },
    };
    if (ioctl(camera->fd, VIDIOC_S_FMT, &requested) != 0) {
        ESP_LOGE(TAG, "VIDIOC_S_FMT RGB565 %ux%u failed", CAMERA_SENSOR_WIDTH, CAMERA_SENSOR_HEIGHT);
        goto fail;
    }
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(camera->fd, VIDIOC_G_FMT, &format) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT after RGB565 negotiation failed");
        goto fail;
    }
    uint32_t expected_stride = CAMERA_SENSOR_WIDTH * sizeof(uint16_t);
    uint32_t negotiated_stride = format.fmt.pix.bytesperline;
    if (format.fmt.pix.width != CAMERA_SENSOR_WIDTH ||
        format.fmt.pix.height != CAMERA_SENSOR_HEIGHT ||
        (format.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565 &&
         format.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565X) ||
        (negotiated_stride != 0 && negotiated_stride != expected_stride)) {
        ESP_LOGE(
            TAG,
            "unsupported negotiated camera format: %lux%lu fourcc=0x%08lx stride=%lu (expected RGB565 %ux%u stride=%lu)",
            (unsigned long)format.fmt.pix.width,
            (unsigned long)format.fmt.pix.height,
            (unsigned long)format.fmt.pix.pixelformat,
            (unsigned long)format.fmt.pix.bytesperline,
            CAMERA_SENSOR_WIDTH,
            CAMERA_SENSOR_HEIGHT,
            (unsigned long)expected_stride);
        goto fail;
    }
    camera->width = format.fmt.pix.width;
    camera->height = format.fmt.pix.height;
    /* ESP Video leaves bytesperline zero for its tightly packed RGB565 buffers. */
    camera->stride = negotiated_stride != 0 ? negotiated_stride : expected_stride;
    camera->format = format.fmt.pix.pixelformat;

    struct v4l2_requestbuffers request = {
        .count = 2,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(camera->fd, VIDIOC_REQBUFS, &request) != 0 || request.count < 2) goto fail;
    for (uint32_t i = 0; i < 2; ++i) {
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        if (ioctl(camera->fd, VIDIOC_QUERYBUF, &buffer) != 0) goto fail;
        if (buffer.length < (size_t)camera->stride * camera->height) {
            ESP_LOGE(TAG, "camera MMAP buffer is too short: %lu", (unsigned long)buffer.length);
            goto fail;
        }
        camera->lengths[i] = buffer.length;
        camera->buffers[i] = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, camera->fd, buffer.m.offset);
        if (camera->buffers[i] == MAP_FAILED) {
            camera->buffers[i] = NULL;
            goto fail;
        }
        if (ioctl(camera->fd, VIDIOC_QBUF, &buffer) != 0) goto fail;
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(camera->fd, VIDIOC_STREAMON, &type) != 0) goto fail;
    int64_t deadline_us = esp_timer_get_time() + (int64_t)delay_ms * 1000;
    for (;;) {
        camera->frame = (struct v4l2_buffer){
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        if (ioctl(camera->fd, VIDIOC_DQBUF, &camera->frame) != 0 ||
            camera->frame.index >= 2) {
            goto fail;
        }
        if (esp_timer_get_time() >= deadline_us) break;
        /* Keep the two-buffer pipeline moving while delayMs elapses. Sleeping
         * here lets both buffers fill and stalls the sensor before capture. */
        if (ioctl(camera->fd, VIDIOC_QBUF, &camera->frame) != 0) goto fail;
    }
    size_t frame_size = (size_t)camera->stride * camera->height;
    if (camera->frame.bytesused != 0 && camera->frame.bytesused < frame_size) {
        ESP_LOGE(
            TAG,
            "camera frame is too short: %lu bytes (expected at least %lu)",
            (unsigned long)camera->frame.bytesused,
            (unsigned long)frame_size);
        goto fail;
    }
    return ESP_OK;
fail:
    close_camera_frame(camera);
    return ESP_FAIL;
}

typedef struct {
    uint8_t *data;
    size_t data_size;
    uint32_t width;
    uint32_t height;
} camera_transformed_frame_t;

static esp_err_t camera_ppa_rotation(ppa_srm_rotation_angle_t *rotation)
{
    switch (BSP_CAMERA_ROTATION) {
    case 0: *rotation = PPA_SRM_ROTATION_ANGLE_0; return ESP_OK;
    case 90: *rotation = PPA_SRM_ROTATION_ANGLE_90; return ESP_OK;
    case 180: *rotation = PPA_SRM_ROTATION_ANGLE_180; return ESP_OK;
    case 270: *rotation = PPA_SRM_ROTATION_ANGLE_270; return ESP_OK;
    default: return ESP_ERR_NOT_SUPPORTED;
    }
}

static esp_err_t transform_camera_frame(
    const camera_frame_t *camera,
    int max_width,
    camera_transformed_frame_t *transformed)
{
    memset(transformed, 0, sizeof(*transformed));
    ESP_RETURN_ON_FALSE(
        camera_ppa_srm != NULL && camera_ppa_alignment != 0,
        ESP_ERR_INVALID_STATE,
        TAG,
        "camera PPA client is unavailable");

    ppa_srm_rotation_angle_t rotation;
    ESP_RETURN_ON_ERROR(camera_ppa_rotation(&rotation), TAG, "unsupported camera rotation");
    bool swaps_dimensions = rotation == PPA_SRM_ROTATION_ANGLE_90 ||
        rotation == PPA_SRM_ROTATION_ANGLE_270;
    uint32_t natural_width = swaps_dimensions ? camera->height : camera->width;
    uint32_t natural_height = swaps_dimensions ? camera->width : camera->height;
    ESP_RETURN_ON_FALSE(
        (uint64_t)natural_width * natural_height <= CAMERA_MAX_PIXELS,
        ESP_ERR_NOT_SUPPORTED,
        TAG,
        "rotated camera frame exceeds one megapixel");

    uint32_t output_width = natural_width;
    if ((uint32_t)max_width < output_width) output_width = (uint32_t)max_width;
    output_width &= ~(CAMERA_DIMENSION_ALIGNMENT - 1U);
    uint32_t output_height = (uint32_t)(
        ((uint64_t)natural_height * output_width / natural_width) &
        ~(uint64_t)(CAMERA_DIMENSION_ALIGNMENT - 1U));
    ESP_RETURN_ON_FALSE(
        output_width != 0 && output_height != 0 &&
        (uint64_t)output_width * output_height <= CAMERA_MAX_PIXELS,
        ESP_ERR_INVALID_SIZE,
        TAG,
        "camera transform dimensions are invalid");

    size_t data_size = (size_t)output_width * output_height * 3U;
    size_t allocation_size = CAMERA_ALIGN_UP(data_size, camera_ppa_alignment);
    uint8_t *output = heap_caps_aligned_calloc(
        camera_ppa_alignment,
        1,
        allocation_size,
        MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(output != NULL, ESP_ERR_NO_MEM, TAG, "camera transform buffer allocation failed");

    float scale_x = (float)output_width / camera->width;
    float scale_y = (float)output_height / camera->height;
    if (swaps_dimensions) {
        scale_x = (float)output_height / camera->width;
        scale_y = (float)output_width / camera->height;
    }
    const ppa_srm_oper_config_t config = {
        .in = {
            .buffer = camera->buffers[camera->frame.index],
            .pic_w = camera->width,
            .pic_h = camera->height,
            .block_w = camera->width,
            .block_h = camera->height,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = output,
            .buffer_size = allocation_size,
            .pic_w = output_width,
            .pic_h = output_height,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
        },
        .rotation_angle = rotation,
        .scale_x = scale_x,
        .scale_y = scale_y,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t result = ppa_do_scale_rotate_mirror(camera_ppa_srm, &config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "camera PPA transform failed: %s", esp_err_to_name(result));
        heap_caps_free(output);
        return result;
    }
    transformed->data = output;
    transformed->data_size = data_size;
    transformed->width = output_width;
    transformed->height = output_height;
    return ESP_OK;
}

static esp_err_t camera_snap(
    esp_openclaw_node_handle_t node, void *context, const char *params_json,
    size_t params_len, char **out, esp_openclaw_node_error_t *error)
{
    (void)node;
    (void)context;
    cJSON *params = cJSON_ParseWithLength(params_json, params_len);
    cJSON *facing = cJSON_GetObjectItemCaseSensitive(params, "facing");
    cJSON *device_id = cJSON_GetObjectItemCaseSensitive(params, "deviceId");
    cJSON *width_item = cJSON_GetObjectItemCaseSensitive(params, "maxWidth");
    cJSON *quality_item = cJSON_GetObjectItemCaseSensitive(params, "quality");
    cJSON *format_item = cJSON_GetObjectItemCaseSensitive(params, "format");
    cJSON *delay_item = cJSON_GetObjectItemCaseSensitive(params, "delayMs");
    bool closed = true;
    for (cJSON *field = cJSON_IsObject(params) ? params->child : NULL;
         field != NULL;
         field = field->next) {
        const char *key = field->string;
        if (key == NULL ||
            (strcmp(key, "facing") != 0 && strcmp(key, "deviceId") != 0 &&
             strcmp(key, "maxWidth") != 0 && strcmp(key, "quality") != 0 &&
             strcmp(key, "format") != 0 && strcmp(key, "delayMs") != 0)) {
            closed = false;
            break;
        }
    }
    int max_width = cJSON_IsNumber(width_item) ? width_item->valueint : 1024;
    double quality_value = cJSON_IsNumber(quality_item) ? quality_item->valuedouble : 0.8;
    int delay_ms = cJSON_IsNumber(delay_item) ? delay_item->valueint : 0;
    const char *format = cJSON_IsString(format_item) ? format_item->valuestring : "jpg";
    bool valid = cJSON_IsObject(params) && closed &&
        (facing == NULL || (cJSON_IsString(facing) && facing->valuestring != NULL)) &&
        (device_id == NULL || (cJSON_IsString(device_id) && device_id->valuestring != NULL)) &&
        (width_item == NULL || (cJSON_IsNumber(width_item) &&
            width_item->valuedouble == (double)width_item->valueint)) &&
        (quality_item == NULL || cJSON_IsNumber(quality_item)) &&
        (format_item == NULL || (cJSON_IsString(format_item) && format_item->valuestring != NULL)) &&
        (delay_item == NULL || (cJSON_IsNumber(delay_item) &&
            delay_item->valuedouble == (double)delay_item->valueint)) &&
        max_width >= 64 && max_width <= 2048 &&
        quality_value >= 0.01 && quality_value <= 1.0 && delay_ms >= 0 && delay_ms <= 10000 &&
        (strcmp(format, "jpg") == 0 || strcmp(format, "jpeg") == 0) &&
        (facing == NULL || (strlen(facing->valuestring) <= 64 &&
            strcmp(facing->valuestring, "front") == 0)) &&
        (device_id == NULL || (strlen(device_id->valuestring) <= 64 &&
            strcmp(device_id->valuestring, "tab5-front") == 0));
    if (!valid) {
        cJSON_Delete(params);
        error->code = "INVALID_PARAMS";
        error->message = "camera.snap accepts front/tab5-front JPEG, maxWidth 64..2048, quality 0.01..1, delayMs 0..10000";
        return ESP_ERR_INVALID_ARG;
    }

    if (!esp_openclaw_room_node_try_acquire_camera()) {
        cJSON_Delete(params);
        error->code = "BUSY";
        error->message = "camera capture is serialized against an active Talk session";
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t indicator_err = esp_openclaw_room_node_camera_indicator_begin();
    if (indicator_err != ESP_OK) {
        esp_openclaw_room_node_release_camera();
        cJSON_Delete(params);
        error->code = "PRIVACY_INDICATOR_UNAVAILABLE";
        error->message = "capture refused because the visible camera indicator could not be armed";
        return indicator_err;
    }
    camera_frame_t camera = {.fd = -1};
    esp_err_t result = capture_rgb565_frame(delay_ms, &camera);
    if (result != ESP_OK) {
        esp_openclaw_room_node_camera_indicator_end();
        esp_openclaw_room_node_release_camera();
        cJSON_Delete(params);
        error->code = "UNAVAILABLE";
        error->message = "Tab5 camera or V4L2 pipeline is unavailable";
        return result;
    }
    camera_transformed_frame_t transformed;
    result = transform_camera_frame(&camera, max_width, &transformed);
    if (result != ESP_OK) {
        close_camera_frame(&camera);
        esp_openclaw_room_node_camera_indicator_end();
        esp_openclaw_room_node_release_camera();
        cJSON_Delete(params);
        error->code = "UNAVAILABLE";
        error->message = "Tab5 camera PPA rotation/downscale is unavailable";
        return result;
    }
    int quality_percent = (int)(quality_value * 100.0);
    uint8_t *jpeg = NULL;
    int jpeg_size = 0;
    esp_err_t jpeg_result = ESP_OK;
    const char *jpeg_failure_message = NULL;
    while (quality_percent >= 1) {
        jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
        cfg.width = (int)transformed.width;
        cfg.height = (int)transformed.height;
        cfg.src_type = JPEG_PIXEL_FORMAT_RGB888;
        cfg.subsampling = JPEG_SUBSAMPLE_420;
        cfg.quality = (uint8_t)quality_percent;
        cfg.task_enable = false;
        jpeg_enc_handle_t encoder = NULL;
        size_t capacity = transformed.data_size + 1024;
        jpeg = malloc(capacity);
        if (jpeg == NULL) {
            ESP_LOGE(TAG, "JPEG output allocation failed: %lu bytes", (unsigned long)capacity);
            jpeg_result = ESP_ERR_NO_MEM;
            jpeg_failure_message = "Tab5 JPEG output buffer allocation failed";
            break;
        }
        jpeg_error_t enc_err = jpeg_enc_open(&cfg, &encoder);
        if (enc_err != JPEG_ERR_OK || encoder == NULL) {
            ESP_LOGE(TAG, "JPEG encoder open failed: error=%d handle=%p", (int)enc_err, encoder);
            jpeg_result = ESP_FAIL;
            jpeg_failure_message = "Tab5 JPEG encoder could not be opened";
            free(jpeg);
            jpeg = NULL;
            break;
        }
        enc_err = jpeg_enc_process(
            encoder, transformed.data, (int)transformed.data_size,
            jpeg, (int)capacity, &jpeg_size);
        jpeg_enc_close(encoder);
        if (enc_err != JPEG_ERR_OK || jpeg_size <= 0) {
            ESP_LOGE(TAG, "JPEG encode failed: error=%d output=%d", (int)enc_err, jpeg_size);
            jpeg_result = ESP_FAIL;
            jpeg_failure_message = "Tab5 JPEG encoder failed to process the camera frame";
            free(jpeg);
            jpeg = NULL;
            jpeg_size = 0;
            break;
        }
        if (jpeg_size <= 740 * 1024) break;
        ESP_LOGW(TAG, "JPEG output too large at quality %d: %d bytes", quality_percent, jpeg_size);
        free(jpeg);
        jpeg = NULL;
        jpeg_size = 0;
        quality_percent = quality_percent > 15 ? quality_percent - 15 : 0;
    }
    uint32_t captured_width = transformed.width;
    uint32_t captured_height = transformed.height;
    heap_caps_free(transformed.data);
    close_camera_frame(&camera);
    esp_openclaw_room_node_camera_indicator_end();
    esp_openclaw_room_node_release_camera();
    cJSON_Delete(params);
    if (jpeg_result != ESP_OK) {
        error->code = "INTERNAL";
        error->message = jpeg_failure_message;
        return jpeg_result;
    }
    if (jpeg == NULL || jpeg_size <= 0) {
        error->code = "PAYLOAD_TOO_LARGE";
        error->message = "camera JPEG could not fit the 1 MiB command payload; request a smaller maxWidth";
        return ESP_ERR_INVALID_SIZE;
    }
    size_t base64_size = 4 * (((size_t)jpeg_size + 2) / 3);
    size_t payload_size = base64_size + 128;
    char *payload = malloc(payload_size);
    if (payload == NULL) {
        free(jpeg);
        return ESP_ERR_NO_MEM;
    }
    int prefix = snprintf(payload, payload_size, "{\"format\":\"jpg\",\"base64\":\"");
    size_t written = 0;
    if (mbedtls_base64_encode((unsigned char *)payload + prefix, payload_size - (size_t)prefix,
            &written, jpeg, (size_t)jpeg_size) != 0) {
        free(jpeg);
        free(payload);
        return ESP_FAIL;
    }
    free(jpeg);
    snprintf(payload + prefix + written, payload_size - (size_t)prefix - written,
        "\",\"width\":%u,\"height\":%u}", (unsigned)captured_width, (unsigned)captured_height);
    *out = payload;
    return ESP_OK;
}

static esp_err_t storage_metrics(void *ctx, uint64_t *total, uint64_t *free_bytes)
{
    (void)ctx;
    if (!sd_mounted) return ESP_ERR_NOT_FOUND;
    return esp_vfs_fat_info(BSP_SD_MOUNT_POINT, total, free_bytes);
}

static bool storage_available(void *ctx)
{
    (void)ctx;
    return sd_mounted;
}

static esp_err_t register_commands(void *ctx, esp_openclaw_node_handle_t node)
{
    (void)ctx;
    ESP_RETURN_ON_ERROR(esp_openclaw_node_register_capability(node, "hardware"), TAG, "hardware capability");
    ESP_RETURN_ON_ERROR(esp_openclaw_node_register_capability(node, "camera"), TAG, "camera capability");
    const esp_openclaw_node_command_t commands[] = {
        {.name = "hardware.status", .handler = hardware_status},
        {.name = "camera.list", .handler = camera_list},
        {.name = "camera.snap", .handler = camera_snap},
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        ESP_RETURN_ON_ERROR(
            esp_openclaw_node_register_command(node, &commands[i]),
            TAG,
            "register %s",
            commands[i].name);
    }
    return ESP_OK;
}

esp_err_t tab5_room_board_config(esp_openclaw_room_node_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config required");
    ESP_RETURN_ON_ERROR(init_camera_transformer(), TAG, "camera PPA client");
    *config = (esp_openclaw_room_node_config_t){
        .display_name = "OpenClaw M5Stack Tab5 Room Node",
        .model_identifier = "m5stack-tab5",
        .display = {
            .start = tab5_display_start,
            .lock = display_lock,
            .unlock = display_unlock,
            .set_brightness = display_brightness,
            .native_width = 1280,
            .native_height = 720,
            .safe_inset = 24,
            /* The full-screen procedural face exceeds this rotated pipeline's
             * watchdog budget; text states keep the product responsive. */
            .animated_face = false,
            .animation_frame_ms = 50,
        },
        .audio = {
            .open = tab5_audio_open,
            /* Keep four-slot TDM, but feed MIC-L + speaker reference to AFE.
             * Dual-mic BSS starves IDLE0 on early 360 MHz P4 silicon. */
            .afe_layout = "MR",
            .record_channels = 4,
            .channel_mask = 0x3,
            .playback_volume = 75,
        },
        .services = {
            .prepare_runtime = prepare_runtime,
            .prepare_network = prepare_network,
            .register_commands = register_commands,
        },
        .storage = {
            .public_root = BSP_SD_MOUNT_POINT,
            .is_available = storage_available,
            .get_metrics = storage_metrics,
        },
    };
    return ESP_OK;
}
