#include "esp_openclaw_room_node.h"

#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#define TAG "waveshare_room"
#define WAVESHARE_DRAW_ROWS 16

static void (*toggle_room_view)(void);

static void boot_button_task(void *arg)
{
    (void)arg;
    bool was_pressed = false;
    for (;;) {
        bool pressed = gpio_get_level(GPIO_NUM_0) == 0;
        if (pressed && !was_pressed && toggle_room_view != NULL) {
            toggle_room_view();
        }
        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}

static esp_err_t waveshare_setup_local_input(void *ctx, void (*toggle_view)(void))
{
    (void)ctx;
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "BOOT button GPIO");
    toggle_room_view = toggle_view;
    return xTaskCreateWithCaps(
        boot_button_task,
        "boot_btn",
        2560,
        NULL,
        4,
        NULL,
        MALLOC_CAP_SPIRAM) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static void align_sh8601_flush(lv_event_t *event)
{
    lv_area_t *area = lv_event_get_param(event);
    if (area == NULL) {
        return;
    }
    area->x1 = 0;
    area->x2 = BSP_LCD_H_RES - 1;
    area->y1 &= ~1;
    area->y2 |= 1;
}

static lv_display_t *waveshare_display_start(void *ctx)
{
    (void)ctx;
    lv_display_t *display = bsp_display_start();
    if (display == NULL || !bsp_display_lock(0)) {
        return NULL;
    }

    /* SH8601 QSPI cannot DMA from PSRAM. Allocate the two partial buffers once
     * from internal DMA memory, and keep invalidations on even full-width rows. */
    size_t draw_bytes = (size_t)BSP_LCD_H_RES * WAVESHARE_DRAW_ROWS * sizeof(uint16_t);
    void *draw_a = heap_caps_aligned_alloc(
        CONFIG_LV_DRAW_BUF_ALIGN, draw_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *draw_b = heap_caps_aligned_alloc(
        CONFIG_LV_DRAW_BUF_ALIGN, draw_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (draw_a == NULL || draw_b == NULL) {
        heap_caps_free(draw_a);
        heap_caps_free(draw_b);
        bsp_display_unlock();
        ESP_LOGE(TAG, "both internal DMA draw buffers are required for SH8601");
        return NULL;
    }
    lv_display_set_buffers(
        display, draw_a, draw_b, (uint32_t)draw_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(display, align_sh8601_flush, LV_EVENT_INVALIDATE_AREA, NULL);
    bsp_display_unlock();
    return display;
}

static bool waveshare_display_lock(void *ctx, uint32_t timeout_ms)
{
    (void)ctx;
    return bsp_display_lock(timeout_ms);
}

static void waveshare_display_unlock(void *ctx)
{
    (void)ctx;
    bsp_display_unlock();
}

static esp_err_t waveshare_set_brightness(void *ctx, int percent)
{
    (void)ctx;
    return bsp_display_brightness_set(percent);
}

static esp_err_t waveshare_audio_open(
    void *ctx,
    esp_openclaw_room_audio_handles_t *handles)
{
    (void)ctx;
    ESP_RETURN_ON_FALSE(handles != NULL, ESP_ERR_INVALID_ARG, TAG, "audio handles required");
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "audio I2C bus init");
    i2c_master_bus_handle_t i2c = bsp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(i2c != NULL, ESP_ERR_INVALID_STATE, TAG, "audio I2C bus handle");

    i2s_chan_handle_t tx = NULL;
    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t channel_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    channel_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_cfg, &tx, &rx), TAG, "I2S channel create");

    const i2s_std_config_t i2s_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(24000),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &i2s_cfg), TAG, "I2S TX init");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &i2s_cfg), TAG, "I2S RX init");
    i2s_chan_info_t tx_info = {0};
    i2s_chan_info_t rx_info = {0};
    ESP_RETURN_ON_ERROR(i2s_channel_get_info(tx, &tx_info), TAG, "I2S TX channel info");
    ESP_RETURN_ON_ERROR(i2s_channel_get_info(rx, &rx_info), TAG, "I2S RX channel info");
    ESP_RETURN_ON_FALSE(
        tx_info.pair_chan == rx && rx_info.pair_chan == tx,
        ESP_ERR_INVALID_STATE,
        TAG,
        "I2S TX/RX failed to establish a reciprocal pair");
    ESP_LOGI(TAG, "I2S paired at 24 kHz with a symmetric stereo STD frame contract");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "I2S TX enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "I2S RX enable");

    audio_codec_i2s_cfg_t data_cfg = {
        .port = CONFIG_BSP_I2S_NUM,
        .rx_handle = rx,
        .tx_handle = tx,
    };
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&data_cfg);
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_NO_MEM, TAG, "I2S codec data interface");

    audio_codec_i2c_cfg_t speaker_ctrl_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c,
    };
    const audio_codec_ctrl_if_t *speaker_ctrl = audio_codec_new_i2c_ctrl(&speaker_ctrl_cfg);
    ESP_RETURN_ON_FALSE(
        speaker_ctrl != NULL, ESP_ERR_NO_MEM, TAG, "ES8311 control interface");
    const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio != NULL, ESP_ERR_NO_MEM, TAG, "ES8311 GPIO interface");
    esp_codec_dev_hw_gain_t gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3};
    es8311_codec_cfg_t speaker_cfg = {
        .ctrl_if = speaker_ctrl,
        .gpio_if = gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_POWER_AMP_IO,
        .use_mclk = true,
        .hw_gain = gain,
    };
    const audio_codec_if_t *speaker = es8311_codec_new(&speaker_cfg);
    ESP_RETURN_ON_FALSE(speaker != NULL, ESP_ERR_NO_MEM, TAG, "ES8311 codec interface");
    esp_codec_dev_cfg_t speaker_dev = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = speaker,
        .data_if = data,
    };
    handles->playback = esp_codec_dev_new(&speaker_dev);
    ESP_RETURN_ON_FALSE(
        handles->playback != NULL, ESP_ERR_NO_MEM, TAG, "ES8311 playback device");

    audio_codec_i2c_cfg_t microphone_ctrl_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c,
    };
    const audio_codec_ctrl_if_t *microphone_ctrl =
        audio_codec_new_i2c_ctrl(&microphone_ctrl_cfg);
    ESP_RETURN_ON_FALSE(
        microphone_ctrl != NULL, ESP_ERR_NO_MEM, TAG, "ES7210 control interface");
    es7210_codec_cfg_t microphone_cfg = {
        .ctrl_if = microphone_ctrl,
        /* Tested board geometry: near-end MIC1 plus speaker reference on MIC3. */
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC3,
    };
    const audio_codec_if_t *microphone = es7210_codec_new(&microphone_cfg);
    ESP_RETURN_ON_FALSE(
        microphone != NULL, ESP_ERR_NO_MEM, TAG, "ES7210 codec interface");
    esp_codec_dev_cfg_t microphone_dev = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = microphone,
        .data_if = data,
    };
    handles->record = esp_codec_dev_new(&microphone_dev);
    ESP_RETURN_ON_FALSE(
        handles->record != NULL, ESP_ERR_NO_MEM, TAG, "ES7210 capture device");
    return ESP_OK;
}

void app_main(void)
{
    const esp_openclaw_room_node_config_t config = {
        .display_name = "OpenClaw AMOLED Room Node",
        .model_identifier = "waveshare-esp32-s3-touch-amoled-2.06",
        .display = {
            .start = waveshare_display_start,
            .setup_local_input = waveshare_setup_local_input,
            .lock = waveshare_display_lock,
            .unlock = waveshare_display_unlock,
            .set_brightness = waveshare_set_brightness,
            .native_width = BSP_LCD_H_RES,
            .native_height = BSP_LCD_V_RES,
            .safe_inset = 32,
            .animated_face = true,
            .animation_frame_ms = 16,
        },
        .audio = {
            .open = waveshare_audio_open,
            .afe_layout = "MR",
            .record_channels = 2,
            .channel_mask = 0x3,
            .playback_volume = 100,
            .playback_gain_db = 6.0f,
            .configure_input_gain = true,
            .input_gain_db = 30.0f,
        },
    };
    ESP_ERROR_CHECK(esp_openclaw_room_node_start(&config));
}
