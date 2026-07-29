#include "room_media.h"

#include <stdlib.h>
#include <string.h>

#include "av_render.h"
#include "av_render_default.h"
#include "bsp/esp-bsp.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_enc_default.h"
#include "esp_capture.h"
#include "esp_capture_defaults.h"
#include "esp_capture_sink.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "media_lib_err.h"
#include "model_path.h"

#define TAG "room_media"

static esp_capture_handle_t capture;
static esp_capture_sink_handle_t talk_sink;
static esp_capture_sink_handle_t wake_sink;
static av_render_handle_t player;
static esp_wn_iface_t *wakenet;
static model_iface_data_t *wakenet_data;
static srmodel_list_t *models;
static room_wake_callback_t wake_callback;
static void *wake_callback_ctx;
static size_t wake_chunk_samples;
static i2s_chan_handle_t audio_tx;
static i2s_chan_handle_t audio_rx;
static const audio_codec_data_if_t *audio_data;

static esp_err_t room_audio_codecs_init(
    esp_codec_dev_handle_t *record,
    esp_codec_dev_handle_t *playback)
{
    i2s_chan_config_t channel_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    channel_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(
        i2s_new_channel(&channel_cfg, &audio_tx, &audio_rx),
        TAG,
        "I2S channel create");

    const i2s_std_config_t i2s_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(24000),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
            .invert_flags = {0},
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(audio_tx, &i2s_cfg), TAG, "I2S TX init");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(audio_rx, &i2s_cfg), TAG, "I2S RX init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(audio_tx), TAG, "I2S TX enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(audio_rx), TAG, "I2S RX enable");

    audio_codec_i2s_cfg_t data_cfg = {
        .port = CONFIG_BSP_I2S_NUM,
        .rx_handle = audio_rx,
        .tx_handle = audio_tx,
    };
    audio_data = audio_codec_new_i2s_data(&data_cfg);
    if (audio_data == NULL) {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_bus_handle_t i2c = bsp_i2c_get_handle();
    if (i2c == NULL) {
        return ESP_FAIL;
    }
    audio_codec_i2c_cfg_t speaker_ctrl_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c,
    };
    const audio_codec_ctrl_if_t *speaker_ctrl = audio_codec_new_i2c_ctrl(&speaker_ctrl_cfg);
    const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();
    if (speaker_ctrl == NULL || gpio == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };
    es8311_codec_cfg_t speaker_cfg = {
        .ctrl_if = speaker_ctrl,
        .gpio_if = gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_POWER_AMP_IO,
        .use_mclk = true,
        .hw_gain = gain,
    };
    const audio_codec_if_t *speaker_codec = es8311_codec_new(&speaker_cfg);
    if (speaker_codec == NULL) {
        return ESP_FAIL;
    }
    esp_codec_dev_cfg_t speaker_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = speaker_codec,
        .data_if = audio_data,
    };
    *playback = esp_codec_dev_new(&speaker_dev_cfg);

    audio_codec_i2c_cfg_t microphone_ctrl_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c,
    };
    const audio_codec_ctrl_if_t *microphone_ctrl =
        audio_codec_new_i2c_ctrl(&microphone_ctrl_cfg);
    if (microphone_ctrl == NULL) {
        return ESP_ERR_NO_MEM;
    }
    es7210_codec_cfg_t microphone_cfg = {
        .ctrl_if = microphone_ctrl,
        // The board routes the speaker loopback to MIC3; AFE layout MR requires it second.
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC3,
    };
    const audio_codec_if_t *microphone_codec = es7210_codec_new(&microphone_cfg);
    if (microphone_codec == NULL) {
        return ESP_FAIL;
    }
    esp_codec_dev_cfg_t microphone_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = microphone_codec,
        .data_if = audio_data,
    };
    *record = esp_codec_dev_new(&microphone_dev_cfg);
    return *record != NULL && *playback != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static void wake_task(void *arg)
{
    (void)arg;
    int16_t *chunk = malloc(wake_chunk_samples * sizeof(*chunk));
    if (chunk == NULL) {
        ESP_LOGE(TAG, "failed to allocate WakeNet input chunk");
        vTaskDelete(NULL);
        return;
    }
    size_t filled = 0;
    for (;;) {
        esp_capture_stream_frame_t frame = {.stream_type = ESP_CAPTURE_STREAM_TYPE_AUDIO};
        if (esp_capture_sink_acquire_frame(wake_sink, &frame, false) != ESP_CAPTURE_ERR_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        const int16_t *input = (const int16_t *)frame.data;
        size_t available = frame.size / sizeof(*input);
        while (available > 0) {
            size_t needed = wake_chunk_samples - filled;
            size_t copy = available < needed ? available : needed;
            memcpy(chunk + filled, input, copy * sizeof(*input));
            filled += copy;
            input += copy;
            available -= copy;
            if (filled != wake_chunk_samples) {
                continue;
            }
            int detected = wakenet->detect(wakenet_data, chunk);
            filled = 0;
            if (detected > 0 && wake_callback != NULL) {
                wake_callback(wakenet->get_word_name(wakenet_data, detected), wake_callback_ctx);
            }
        }
        esp_capture_sink_release_frame(wake_sink, &frame);
    }
}

esp_err_t room_media_init(room_wake_callback_t callback, void *ctx)
{
    wake_callback = callback;
    wake_callback_ctx = ctx;

    esp_codec_dev_handle_t record = NULL;
    esp_codec_dev_handle_t playback = NULL;
    ESP_RETURN_ON_ERROR(
        room_audio_codecs_init(&record, &playback),
        TAG,
        "audio codec init");
    if (esp_codec_dev_set_out_vol(playback, 65) != 0) {
        return ESP_FAIL;
    }

    esp_capture_audio_aec_src_cfg_t source_cfg = {
        .mic_layout = "MR",
        .record_handle = record,
        .channel = 2,
        // ES7210 packs the two enabled analog inputs into the two standard-I2S slots.
        .channel_mask = 0x3,
    };
    esp_capture_audio_src_if_t *audio_source = esp_capture_new_audio_aec_src(&source_cfg);
    if (audio_source == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_capture_cfg_t capture_cfg = {
        .sync_mode = ESP_CAPTURE_SYNC_MODE_AUDIO,
        .audio_src = audio_source,
    };
    if (esp_capture_open(&capture_cfg, &capture) != ESP_CAPTURE_ERR_OK) {
        return ESP_FAIL;
    }

    esp_audio_enc_register_default();
    esp_audio_dec_register_default();
    esp_capture_sink_cfg_t talk_cfg = {
        .audio_info = {
            .format_id = ESP_CAPTURE_FMT_ID_OPUS,
            .sample_rate = 16000,
            .channel = 1,
            .bits_per_sample = 16,
        },
    };
    esp_capture_sink_cfg_t wake_cfg = {
        .audio_info = {
            .format_id = ESP_CAPTURE_FMT_ID_PCM,
            .sample_rate = 16000,
            .channel = 1,
            .bits_per_sample = 16,
        },
    };
    // esp_webrtc owns sink 0 and retrieves this exact pre-created path after capture starts.
    if (esp_capture_sink_setup(capture, 0, &talk_cfg, &talk_sink) != ESP_CAPTURE_ERR_OK ||
        esp_capture_sink_setup(capture, 1, &wake_cfg, &wake_sink) != ESP_CAPTURE_ERR_OK ||
        esp_capture_sink_enable(wake_sink, ESP_CAPTURE_RUN_MODE_ALWAYS) != ESP_CAPTURE_ERR_OK ||
        esp_capture_start(capture) != ESP_CAPTURE_ERR_OK) {
        return ESP_FAIL;
    }

    i2s_render_cfg_t render_cfg = {.play_handle = playback};
    audio_render_handle_t renderer = av_render_alloc_i2s_render(&render_cfg);
    if (renderer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    av_render_cfg_t player_cfg = {
        .audio_render = renderer,
        .audio_raw_fifo_size = 8 * 4096,
        .audio_render_fifo_size = 100 * 1024,
        .allow_drop_data = false,
    };
    player = av_render_open(&player_cfg);
    if (player == NULL) {
        return ESP_ERR_NO_MEM;
    }
    av_render_audio_frame_info_t frame_info = {
        .sample_rate = 16000,
        .channel = 2,
        .bits_per_sample = 16,
    };
    // Pinned av_render copies this pre-stream setting despite returning WRONG_STATE.
    int fixed_info_result = av_render_set_fixed_frame_info(player, &frame_info);
    if (fixed_info_result != ESP_MEDIA_ERR_OK && fixed_info_result != ESP_MEDIA_ERR_WRONG_STATE) {
        return ESP_FAIL;
    }

    models = esp_srmodel_init("model");
    char *model_name = models != NULL ? esp_srmodel_filter(models, ESP_WN_PREFIX, NULL) : NULL;
    wakenet = model_name != NULL ? (esp_wn_iface_t *)esp_wn_handle_from_name(model_name) : NULL;
    wakenet_data = wakenet != NULL ? wakenet->create(model_name, DET_MODE_95) : NULL;
    if (wakenet_data == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    wake_chunk_samples = (size_t)wakenet->get_samp_chunksize(wakenet_data);
    if (wake_chunk_samples == 0) {
        return ESP_FAIL;
    }
    if (xTaskCreate(wake_task, "ambient_wake", 8192, NULL, 8, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "24 kHz dual-channel capture, device AEC, and WakeNet are active");
    return ESP_OK;
}

esp_err_t room_media_get_webrtc_provider(esp_webrtc_media_provider_t *provider)
{
    if (provider == NULL || capture == NULL || player == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    provider->capture = capture;
    provider->player = player;
    return ESP_OK;
}
