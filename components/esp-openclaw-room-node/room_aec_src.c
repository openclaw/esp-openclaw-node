/**
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE.ESPRESSIF-MODIFIED-MIT for details.
 */

/*
 * Copied from espressif/esp_capture v1.0.2 because upstream does not surface
 * AFE wake events. The wake callback should be upstreamed and this copy
 * dropped once esp_capture exposes those detections.
 */

#include <sdkconfig.h>
#include <limits.h>
#include <string.h>
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31
#include "esp_capture_types.h"
#include "esp_capture_audio_src_if.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_aec.h"
#include "esp_gmf_data_queue.h"
#include "capture_utils.h"
#include "esp_afe_sr_iface.h"
#include "esp_vadn_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_vad.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "msg_q.h"
#include "room_aec_src.h"
#include "room_diagnostics_data.h"

#define TAG  "AUD_AEC_SRC"

#define VAD_CACHE_BLOCK   (3)
#define VAD_SILENT_BLOCK  (20)
#define AFE_RUN_STACK     (8192)
#define WAKE_DEBOUNCE_US  (2 * 1000 * 1000)
#define VALID_ON_VAD      // Turn on to not send data if VAD not active or else send silent data
#define WAIT_STATE_TIMEOUT(state) do {                    \
    int _wait_time_out = 1000;                            \
    while (state) {                                       \
        capture_sleep(10);                                \
        _wait_time_out -= 10;                             \
        if (_wait_time_out == 0) {                        \
            ESP_LOGE(TAG, "Wait for" #state "timeout");   \
            break;                                        \
        }                                                 \
    }                                                     \
} while (0);

typedef enum {
    VAD_CHECKING_DETECTING,
    VAD_CHECKING_STARTED,
    VAD_CHECKING_ENDED,
} vad_checking_state_t;

typedef struct {
    esp_vadn_iface_t     *vadnet;
    model_iface_data_t   *vad_model;
    uint8_t              *vad_working_buf;
    uint8_t               vad_channel;
    uint8_t               vad_filled_block;
    uint8_t               silent_block;
    esp_gmf_data_queue_t *in_q;
    msg_q_handle_t        vad_q;
    uint8_t               vad_duration;
    vad_checking_state_t  vad_state;
    bool                  dev_src_running;
} audio_aec_vad_res_t;

typedef struct {
    esp_capture_audio_src_if_t  base;
    const char                 *mic_layout;
    uint8_t                     channel;
    uint8_t                     channel_mask;
    bool                        data_on_vad;
    esp_codec_dev_handle_t      handle;
    esp_capture_audio_info_t    info;
    uint64_t                    samples;
    uint8_t                    *cached_frame;
    int                         cached_read_pos;
    int                         feed_size;
    int                         fetch_size;
    int                         cache_fill;
    uint8_t                     start        : 1;
    uint8_t                     open         : 1;
    uint8_t                     in_quit      : 1;
    uint8_t                     in_error     : 1;
    uint8_t                     stopping     : 1;
    bool                        wait_feeding : 1;
    const esp_afe_sr_iface_t   *afe_handle;
    esp_afe_sr_data_t          *afe_data;
    srmodel_list_t             *models;
    audio_aec_vad_res_t        *vad_res;
    void                      (*wake_cb)(void *ctx);
    void                       *wake_ctx;
    bool                        wakenet_enabled;
} audio_aec_src_t;

static int64_t last_wake_callback_us;
static uint8_t get_src_channel(audio_aec_src_t *src);

static size_t layout_channel_index(const audio_aec_src_t *src, char channel, size_t fallback)
{
    const char *found = src->mic_layout != NULL ? strchr(src->mic_layout, channel) : NULL;
    if (found == NULL) return fallback;
    size_t index = (size_t)(found - src->mic_layout);
    return index < get_src_channel((audio_aec_src_t *)src) ? index : fallback;
}

static bool audio_aec_chunk_samples_to_bytes(int samples, int *bytes)
{
    if (samples <= 0 || samples > INT_MAX / (int)sizeof(int16_t)) {
        return false;
    }
    *bytes = samples * (int)sizeof(int16_t);
    return true;
}

static int open_afe_in_ram(void *arg)
{
    audio_aec_src_t *src = (audio_aec_src_t *)arg;
    esp_capture_err_t ret = ESP_CAPTURE_ERR_OK;
    do {
        src->models = esp_srmodel_init("model");
        if (src->models == NULL) {
            ESP_LOGW(TAG, "No model to load");
        }
        /* Ambient wake uses speech-recognition AFE; active Talk uses the
         * 16 kHz voice-communication pipeline with AEC but no WakeNet. */
        afe_type_t afe_type = src->wakenet_enabled ? AFE_TYPE_SR : AFE_TYPE_VC;
        afe_config_t *afe_config = afe_config_init(src->mic_layout, src->models, afe_type, AFE_MODE_LOW_COST);
        if (afe_config == NULL) {
            ESP_LOGE(TAG, "Failed to create AFE config");
            ret = ESP_CAPTURE_ERR_NO_MEM;
            break;
        }
        afe_config->wakenet_init = src->wakenet_enabled;
        if (!src->wakenet_enabled) {
            /* Talk streams continuously and the remote session owns turn
             * detection. Disable WakeNet/NS/VAD while preserving AEC+NLP. */
            afe_config->ns_init = false;
            afe_config->vad_init = false;
        } else if (src->data_on_vad) {
            // When data_on_vad turn on VAD process before AFE disable it
            afe_config->vad_init = false;
        }
        src->afe_handle = esp_afe_handle_from_config(afe_config);
        if (src->afe_handle == NULL) {
            ESP_LOGE(TAG, "Failed to create AFE handle");
            ret = ESP_CAPTURE_ERR_NOT_SUPPORTED;
            afe_config_free(afe_config);
            break;
        }
        src->afe_data = src->afe_handle->create_from_config(afe_config);
        afe_config_free(afe_config);
        if (src->afe_data == NULL) {
            ESP_LOGE(TAG, "Failed to create AFE data");
            ret = ESP_CAPTURE_ERR_NOT_SUPPORTED;
            break;
        }
    } while (0);
    return ret;
}

static esp_capture_err_t open_afe(audio_aec_src_t *src)
{
    esp_capture_err_t ret = ESP_CAPTURE_ERR_OK;
    CAPTURE_RUN_SYNC_IN_RAM("afe_open", open_afe_in_ram, src, ret, AFE_RUN_STACK);
    return ret;
}

static esp_capture_err_t audio_aec_src_open(esp_capture_audio_src_if_t *h)
{
    audio_aec_src_t *src = (audio_aec_src_t *)h;
    if (src->handle == NULL) {
        return ESP_CAPTURE_ERR_NOT_SUPPORTED;
    }
    src->samples = 0;
    src->open = true;
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t audio_aec_src_get_support_codecs(esp_capture_audio_src_if_t *src, const esp_capture_format_id_t **codecs, uint8_t *num)
{
    static esp_capture_format_id_t support_codecs[] = {ESP_CAPTURE_FMT_ID_PCM};
    *codecs = support_codecs;
    *num = 1;
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t audio_aec_src_negotiate_caps(esp_capture_audio_src_if_t *h, esp_capture_audio_info_t *in_cap, esp_capture_audio_info_t *out_caps)
{
    audio_aec_src_t *src = (audio_aec_src_t *)h;
    // Only support 1 channel 16bits PCM
    if (in_cap->format_id != ESP_CAPTURE_FMT_ID_PCM) {
        return ESP_CAPTURE_ERR_NOT_SUPPORTED;
    }
    if (in_cap->sample_rate == 8000) {
        out_caps->sample_rate = 8000;
    } else {
        out_caps->sample_rate = 16000;
    }
    out_caps->channel = 1;
    out_caps->bits_per_sample = 16;
    out_caps->format_id = ESP_CAPTURE_FMT_ID_PCM;
    src->info = *out_caps;
    return ESP_CAPTURE_ERR_OK;
}

static uint8_t get_src_channel(audio_aec_src_t *src)
{
    uint8_t ch = src->channel;
    if (src->channel_mask) {
        ch = __builtin_popcount(src->channel_mask);
    }
    return ch;
}

static inline void audio_aec_fill_vad_working_buf(audio_aec_src_t *src, uint8_t *feed_data, int feed_size)
{
    audio_aec_vad_res_t *vad_res = src->vad_res;
    uint8_t src_channel = get_src_channel(src);
    int16_t *src_pcm = (int16_t *)feed_data;
    int16_t *dst_pcm = (int16_t *)vad_res->vad_working_buf;
    int16_t *end = (int16_t *)(feed_data + feed_size);
    src_pcm += vad_res->vad_channel;
    while (src_pcm < end) {
        *(dst_pcm++) = *src_pcm;
        src_pcm += src_channel;
    }
}

static int audio_aec_feed_data(audio_aec_src_t *src, uint8_t *feed_data, int feed_size)
{
    int ret = src->afe_handle->feed(src->afe_data, (int16_t *)feed_data);
    room_diagnostics_audio_record_feed(ret >= 0);
    return ret;
}

static int audio_aec_read_by_vad(audio_aec_src_t *src, int read_size)
{
    audio_aec_vad_res_t *vad_res = src->vad_res;
    int ret = 0;
    uint8_t *feed_data = NULL;
    if (vad_res->vad_state == VAD_CHECKING_STARTED) {
        if (vad_res->vad_filled_block > 0) {
            // Send vad detection cache firstly
            esp_gmf_data_queue_acquire_read(vad_res->in_q, (void **)&feed_data, &read_size, ESP_GMF_DATA_QUEUE_WAIT_FOREVER);
            if (feed_data == NULL) {
                ESP_LOGE(TAG, "Fail to get from dev src queue on %d", __LINE__);
                return -1;
            }
            ret = audio_aec_feed_data(src, feed_data, read_size);
            if (ret < 0) {
                ESP_LOGE(TAG, "Fail to feed data %d on %d", ret, __LINE__);
            }
            vad_res->vad_filled_block--;
            esp_gmf_data_queue_release_read(vad_res->in_q);
            return 0;
        }
    }
    esp_gmf_data_queue_acquire_read(vad_res->in_q, (void **)&feed_data, &read_size, ESP_GMF_DATA_QUEUE_WAIT_FOREVER);
    if (feed_data == NULL) {
        ESP_LOGE(TAG, "Fail to get from dev src queue on %d", __LINE__);
        return -1;
    }
    // Fill working buffer and do detection
    audio_aec_fill_vad_working_buf(src, feed_data, read_size);
    vad_state_t vad_state = vad_res->vadnet->detect(vad_res->vad_model, (int16_t *)vad_res->vad_working_buf);
    switch (vad_res->vad_state) {
        case VAD_CHECKING_STARTED:
            ret = audio_aec_feed_data(src, feed_data, read_size);
            esp_gmf_data_queue_release_read(vad_res->in_q);
            if (ret < 0) {
                ESP_LOGE(TAG, "Fail to feed data %d on %d", ret, __LINE__);
                break;
            }
            if (vad_state == VAD_SILENCE) {
                if (vad_res->silent_block == 0) {
                    ESP_LOGI(TAG, "VAD ended");
                }
                vad_res->silent_block++;
                if (vad_res->silent_block >= VAD_SILENT_BLOCK) {
                    vad_res->vad_state = VAD_CHECKING_ENDED;
                }
            }
            break;
        case VAD_CHECKING_ENDED:
            if (src->wait_feeding) {
                ret = audio_aec_feed_data(src, feed_data, read_size);
                esp_gmf_data_queue_release_read(vad_res->in_q);
                if (ret < 0) {
                    ESP_LOGE(TAG, "Fail to feed data %d on %d", ret, __LINE__);
                }
                break;
            }
            int v = 0;
            while (msg_q_recv(vad_res->vad_q, &v, sizeof(v), true) == 0);
            vad_res->vad_state = VAD_CHECKING_DETECTING;
            vad_res->vad_filled_block = 0;
            ESP_LOGI(TAG, "VAD Detecting");
            // fallthrough
        case VAD_CHECKING_DETECTING:
            if (vad_res->vad_filled_block < VAD_CACHE_BLOCK) {
                vad_res->vad_filled_block++;
            }
            esp_gmf_data_queue_release_read(vad_res->in_q);
            if (vad_state == VAD_SPEECH) {
                ESP_LOGI(TAG, "VAD started");
                vad_res->vad_state = VAD_CHECKING_STARTED;
                vad_res->silent_block = 0;
                // Rewind and resend again
                esp_gmf_data_queue_rewind(vad_res->in_q, VAD_CACHE_BLOCK);
            }
            v = msg_q_number(vad_res->vad_q);
            if (v < VAD_CACHE_BLOCK) {
                msg_q_send(vad_res->vad_q, &v, sizeof(v));
            }
            ret = 0;
            break;
        default:
            break;
    }
    return ret > 0 ? 0 : ret;
}

static void codec_dev_read_thread(void *arg)
{
    audio_aec_src_t *src = (audio_aec_src_t *)arg;
    audio_aec_vad_res_t *vad_res = src->vad_res;
    int read_size = src->feed_size * get_src_channel(src);
    bool err = false;
    while (!src->stopping) {
        void *data = NULL;
        if (esp_gmf_data_queue_acquire_write(vad_res->in_q, &data, read_size, ESP_GMF_DATA_QUEUE_WAIT_FOREVER) != 0 ||
            data == NULL) {
            break;
        }
        int ret = esp_codec_dev_read(src->handle, (uint8_t *)data, read_size);
        size_t channels = get_src_channel(src);
        room_diagnostics_audio_record_capture_read(
            data,
            (size_t)read_size / sizeof(int16_t),
            channels,
            layout_channel_index(src, 'M', 0),
            layout_channel_index(src, 'R', channels),
            ret == 0);
        if (ret != 0) {
            ESP_LOGE(TAG, "Fail to read data %d", ret);
            esp_gmf_data_queue_release_write(vad_res->in_q, 0);
            err = true;
            break;
        }
        esp_gmf_data_queue_release_write(vad_res->in_q, read_size);
    }
    if (err) {
        esp_gmf_data_queue_wakeup(vad_res->in_q);
    }
    vad_res->dev_src_running = false;
    ESP_LOGI(TAG, "Codec src in exited");
    capture_thread_destroy(NULL);
}

static inline int audio_aec_src_read_from_vad(audio_aec_src_t *src)
{
    int ret = 0;
    int read_size = src->feed_size * get_src_channel(src);
    audio_aec_vad_res_t *vad_res = src->vad_res;
    do {
        vad_res->dev_src_running = true;
        capture_thread_handle_t thread = NULL;
        capture_thread_create_from_scheduler(&thread, "codec_dev_src", codec_dev_read_thread, src);
        if (thread == NULL) {
            ret = -1;
            vad_res->dev_src_running = false;
            break;
        }
        while (!src->stopping) {
            // Handle read by vad
            ret = audio_aec_read_by_vad(src, read_size);
            if (ret != 0) {
                ESP_LOGE(TAG, "Fail to read data ddd %d", ret);
                break;
            }
        }
    } while (0);
    if (vad_res->in_q) {
        esp_gmf_data_queue_wakeup(vad_res->in_q);
    }
    if (ret) {
        src->in_error = true;
        int v = msg_q_number(vad_res->vad_q);
        if (v < VAD_CACHE_BLOCK) {
            msg_q_send(vad_res->vad_q, &v, sizeof(v));
        }
    }
    // Wait for codec source exited
    WAIT_STATE_TIMEOUT(vad_res->dev_src_running);
    return ret;
}

static inline int audio_aec_src_read_directly(audio_aec_src_t *src)
{
    int read_size = src->feed_size * get_src_channel(src);
    int ret = 0;
    uint32_t *feed_data = NULL;
    do {
        feed_data = malloc(read_size);
        if (feed_data == NULL) {
            ret = -1;
            break;
        }
        while (!src->stopping) {
            ret = esp_codec_dev_read(src->handle, feed_data, read_size);
            size_t channels = get_src_channel(src);
            room_diagnostics_audio_record_capture_read(
                (const int16_t *)feed_data,
                (size_t)read_size / sizeof(int16_t),
                channels,
                layout_channel_index(src, 'M', 0),
                layout_channel_index(src, 'R', channels),
                ret == 0);
            if (ret != 0) {
                ESP_LOGE(TAG, "Fail to read data %d", ret);
                break;
            }
            ret = audio_aec_feed_data(src, (uint8_t *)feed_data, read_size);
            if (ret < 0) {
                ESP_LOGE(TAG, "Fail to feed data %d", ret);
                break;
            }
#if CONFIG_IDF_TARGET_ESP32P4 && CONFIG_ESP32P4_SELECTS_REV_LESS_V3
            /* VOIP AEC can continuously drain queued TDM frames on early P4;
             * yield two 1 kHz ticks so IDLE0 retains watchdog service time. */
            vTaskDelay(2);
#endif
        }
    } while (0);
    if (ret < 0) {
        src->in_error = true;
    }
    if (feed_data) {
        capture_free(feed_data);
    }
    return ret;
}

static void audio_aec_src_buffer_in_thread(void *arg)
{
    audio_aec_src_t *src = (audio_aec_src_t *)arg;
    if (src->vad_res) {
        audio_aec_src_read_from_vad(src);
    } else {
        audio_aec_src_read_directly(src);
    }
    src->in_quit = true;
    ESP_LOGI(TAG, "Buffer in exited");
    capture_thread_destroy(NULL);
}

static void release_vad(audio_aec_src_t *src)
{
    if (src->vad_res == NULL) {
        return;
    }
    audio_aec_vad_res_t *vad_res = src->vad_res;
    if (vad_res->vadnet) {
        vad_res->vadnet->destroy(vad_res->vad_model);
        vad_res->vadnet = NULL;
    }
    if (vad_res->in_q) {
        esp_gmf_data_queue_destroy(vad_res->in_q);
        vad_res->in_q = NULL;
    }
    if (vad_res->vad_working_buf) {
        capture_free(vad_res->vad_working_buf);
        vad_res->vad_working_buf = NULL;
    }
    if (vad_res->vad_q) {
        msg_q_destroy(vad_res->vad_q);
        vad_res->vad_q = NULL;
    }
    vad_res->vad_filled_block = 0;
    capture_free(src->vad_res);
    src->vad_res = NULL;
}

static esp_capture_err_t prepare_vad(audio_aec_src_t *src)
{
    if (src->models == NULL || src->data_on_vad == false) {
        return ESP_CAPTURE_ERR_OK;
    }
    char *model_name = esp_srmodel_filter(src->models, ESP_VADN_PREFIX, NULL);
    esp_vadn_iface_t *vadnet = (esp_vadn_iface_t *)esp_vadn_handle_from_name(model_name);
    if (vadnet == NULL) {
        ESP_LOGW(TAG, "VAD model not found");
        return ESP_CAPTURE_ERR_NOT_FOUND;
    }
    src->vad_res = capture_calloc(1, sizeof(audio_aec_vad_res_t));
    if (src->vad_res == NULL) {
        ESP_LOGE(TAG, "Failed to allocate vad res");
        return ESP_CAPTURE_ERR_NO_MEM;
    }
    audio_aec_vad_res_t *vad_res = src->vad_res;
    do {
        vad_res->vad_channel = (uint8_t)(strchr(src->mic_layout, 'M') - src->mic_layout);
        vad_res->vadnet = vadnet;
        vad_res->vad_model = vadnet->create(model_name, VAD_MODE_0, 1, 32, 64);
        if (vad_res->vad_model == NULL) {
            ESP_LOGE(TAG, "Failed to create vad model");
            break;
        }
        int cache_size = (VAD_CACHE_BLOCK * 3) * (src->feed_size * get_src_channel(src) + 16);
        vad_res->in_q = esp_gmf_data_queue_create(cache_size);
        if (vad_res->in_q == NULL) {
            ESP_LOGE(TAG, "Failed to create vad cache");
            break;
        }
        // Only one channel data for vad
        vad_res->vad_working_buf = capture_calloc(1, src->feed_size);
        if (vad_res->vad_working_buf == NULL) {
            ESP_LOGE(TAG, "Failed to allocate vad cache");
            break;
        }
        vad_res->vad_q = msg_q_create(VAD_CACHE_BLOCK, sizeof(int));
        if (vad_res->vad_q == NULL) {
            ESP_LOGE(TAG, "Failed to create vad queue");
            break;
        }
        vad_res->vad_state = VAD_CHECKING_DETECTING;
        return ESP_CAPTURE_ERR_OK;
    } while (0);
    release_vad(src);
    return ESP_CAPTURE_ERR_NO_MEM;
}

static esp_capture_err_t audio_aec_src_stop(esp_capture_audio_src_if_t *h);

static bool audio_aec_record_fetch_result(
    audio_aec_src_t *src,
    afe_fetch_result_t *res)
{
    bool fetch_success = res != NULL && res->ret_value == ESP_OK;
    bool valid_size = res != NULL && res->data_size >= 0 &&
        res->data_size <= src->fetch_size &&
        (res->data_size == 0 || res->data != NULL);
    uint8_t ringbuffer_free = 0;
    if (res != NULL && res->ringbuff_free_pct > 0) {
        float percent = res->ringbuff_free_pct * 100.0f;
        ringbuffer_free = percent >= 100.0f ? 100 : (uint8_t)percent;
    }
    room_diagnostics_audio_record_fetch(
        valid_size ? res->data : NULL,
        valid_size ? (size_t)res->data_size / sizeof(int16_t) : 0,
        fetch_success,
        valid_size,
        ringbuffer_free);
    if (!fetch_success) {
        ESP_LOGE(TAG, "Fail to read from AEC ret %d",
            res != NULL ? res->ret_value : ESP_FAIL);
    }
    if (!valid_size) {
        ESP_LOGE(TAG, "AFE fetch size out of bounds: got %d bytes, capacity %d bytes",
            res != NULL ? res->data_size : -1, src->fetch_size);
    }
    if (res != NULL && res->wakeup_state == WAKENET_DETECTED) {
        room_diagnostics_audio_record_wakenet();
    }
    return valid_size;
}

static esp_capture_err_t audio_aec_src_start(esp_capture_audio_src_if_t *h)
{
    audio_aec_src_t *src = (audio_aec_src_t *)h;
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = src->info.sample_rate,
        .bits_per_sample = 16,
        .channel = src->channel,
        .channel_mask = src->channel_mask,
    };
    src->in_quit = true;
    int ret = esp_codec_dev_open(src->handle, &fs);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to open codec device, ret=%d", ret);
        return ESP_CAPTURE_ERR_NOT_SUPPORTED;
    }
    ret = open_afe(src);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to open AFE");
        return ESP_CAPTURE_ERR_NOT_SUPPORTED;
    }
    int feed_chunksize = src->afe_handle->get_feed_chunksize(src->afe_data);
    int fetch_chunksize = src->afe_handle->get_fetch_chunksize(src->afe_data);
    int feed_size = 0;
    int fetch_size = 0;
    if (!audio_aec_chunk_samples_to_bytes(feed_chunksize, &feed_size) ||
        !audio_aec_chunk_samples_to_bytes(fetch_chunksize, &fetch_size)) {
        ESP_LOGE(TAG, "Invalid AFE frame sizes: feed=%d samples, fetch=%d samples",
                 feed_chunksize, fetch_chunksize);
        audio_aec_src_stop(h);
        return ESP_CAPTURE_ERR_INTERNAL;
    }
    src->feed_size = feed_size;
    src->fetch_size = fetch_size;
    room_diagnostics_audio_set_capture_mode(
        src->wakenet_enabled
            ? ROOM_DIAGNOSTICS_AFE_AMBIENT_SR
            : ROOM_DIAGNOSTICS_AFE_TALK_VC,
        src->wakenet_enabled,
        (uint32_t)feed_size,
        (uint32_t)fetch_size);
    if (src->data_on_vad) {
        ret = prepare_vad(src);
        if (ret != ESP_CAPTURE_ERR_OK) {
            return ret;
        }
    }
    src->cached_frame = capture_calloc(1, src->fetch_size);
    if (src->cached_frame == NULL) {
        ESP_LOGE(TAG, "Failed to allocate cache frame");
        return ESP_CAPTURE_ERR_NOT_SUPPORTED;
    }
    src->samples = 0;
    src->cached_read_pos = src->cache_fill = 0;
    src->stopping = false;

    capture_thread_handle_t thread = NULL;
    int thread_ret = capture_thread_create_from_scheduler(
        &thread,
        "buffer_in",
        audio_aec_src_buffer_in_thread,
        src);
    if (thread_ret != 0 || thread == NULL) {
        ESP_LOGE(TAG, "Failed to create buffer input thread, ret=%d", thread_ret);
        audio_aec_src_stop(h);
        return ESP_CAPTURE_ERR_NO_RESOURCES;
    }
    src->start = true;
    src->in_quit = false;
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t audio_aec_src_read_frame(esp_capture_audio_src_if_t *h, esp_capture_stream_frame_t *frame)
{
    audio_aec_src_t *src = (audio_aec_src_t *)h;
    if (src->start == false) {
        return ESP_CAPTURE_ERR_NOT_SUPPORTED;
    }
    frame->pts = (uint32_t)(src->samples * 1000 / src->info.sample_rate);

    int need_size = frame->size;
    uint8_t *frame_data = frame->data;
    while (need_size > 0) {
        if (src->cached_read_pos < src->cache_fill) {
            int left = src->cache_fill - src->cached_read_pos;
            if (left > need_size) {
                left = need_size;
            }
            memcpy(frame_data, src->cached_frame + src->cached_read_pos, left);
            src->cached_read_pos += left;
            need_size -= left;
            frame_data += left;
            continue;
        }
        if (src->in_quit || src->in_error) {
            return ESP_CAPTURE_ERR_INTERNAL;
        }
        src->cache_fill = 0;
        src->cached_read_pos = 0;
        bool use_silent = false;
        audio_aec_vad_res_t *vad_res = src->vad_res;
        if (vad_res && vad_res->vad_state != VAD_CHECKING_STARTED) {
            // Receive from queue
            int v = 0;
            msg_q_recv(vad_res->vad_q, &v, sizeof(v), false);
#ifdef VALID_ON_VAD
            frame->size = 0;
            return ESP_CAPTURE_ERR_OK;
#endif
            memset(src->cached_frame, 0, src->fetch_size);
            src->cache_fill = src->fetch_size;
            use_silent = true;
        }
        if (use_silent == false) {
            src->wait_feeding = true;
            afe_fetch_result_t *res = src->afe_handle->fetch(src->afe_data);
            src->wait_feeding = false;
            bool valid_size = audio_aec_record_fetch_result(src, res);
            if (res != NULL && res->wakeup_state == WAKENET_DETECTED) {
                int64_t now_us = esp_timer_get_time();
                if (src->wake_cb != NULL && (last_wake_callback_us == 0 ||
                    now_us - last_wake_callback_us >= WAKE_DEBOUNCE_US)) {
                    last_wake_callback_us = now_us;
                    src->wake_cb(src->wake_ctx);
                }
            }
            if (valid_size) {
                if (res->data_size > 0) {
                    memcpy(src->cached_frame, res->data, res->data_size);
                }
                src->cache_fill = res->data_size;
            }
        }
    }
    src->samples += frame->size / 2;
    return ESP_CAPTURE_ERR_OK;
}

static int close_afe_in_ram(void *arg)
{
    audio_aec_src_t *src = (audio_aec_src_t *)arg;
    if (src->models) {
        esp_srmodel_deinit(src->models);
        src->models = NULL;
    }
    if (src->afe_data) {
        src->afe_handle->destroy(src->afe_data);
        src->afe_data = NULL;
    }
    return 0;
}

static esp_capture_err_t audio_aec_src_stop(esp_capture_audio_src_if_t *h)
{
    audio_aec_src_t *src = (audio_aec_src_t *)h;
    esp_capture_err_t ret = ESP_CAPTURE_ERR_OK;
    if (src->in_quit == false) {
        // fetch once
        if (src->vad_res && src->vad_res->vad_state != VAD_CHECKING_STARTED) {
        } else {
            afe_fetch_result_t *res = src->afe_handle->fetch(src->afe_data);
            (void)audio_aec_record_fetch_result(src, res);
        }
        src->stopping = true;
        WAIT_STATE_TIMEOUT(src->in_quit == false);
    }
    release_vad(src);

    CAPTURE_RUN_SYNC_IN_RAM("afe_close", close_afe_in_ram, src, ret, AFE_RUN_STACK);

    if (src->cached_frame) {
        capture_free(src->cached_frame);
        src->cached_frame = NULL;
    }
    if (src->handle) {
        esp_codec_dev_close(src->handle);
    }
    src->in_error = false;
    src->start = false;
    return ret;
}

static esp_capture_err_t audio_aec_src_close(esp_capture_audio_src_if_t *h)
{
    return ESP_CAPTURE_ERR_OK;
}

esp_capture_audio_src_if_t *room_capture_new_audio_aec_src(room_capture_audio_aec_src_cfg_t *cfg)
{
    if (cfg == NULL || cfg->record_handle == NULL) {
        return NULL;
    }
    audio_aec_src_t *src = capture_calloc(1, sizeof(audio_aec_src_t));
    if (src == NULL) {
        return NULL;
    }
    src->base.open = audio_aec_src_open;
    src->base.get_support_codecs = audio_aec_src_get_support_codecs;
    src->base.negotiate_caps = audio_aec_src_negotiate_caps;
    src->base.start = audio_aec_src_start;
    src->base.read_frame = audio_aec_src_read_frame;
    src->base.stop = audio_aec_src_stop;
    src->base.close = audio_aec_src_close;
    src->handle = cfg->record_handle;
    src->channel = cfg->channel ? cfg->channel : 2;
    src->channel_mask = cfg->channel_mask;
    src->data_on_vad = cfg->data_on_vad;
    src->wake_cb = cfg->wake_cb;
    src->wake_ctx = cfg->wake_ctx;
    src->wakenet_enabled = cfg->wake_cb != NULL;
    if (cfg->mic_layout == NULL) {
        src->mic_layout = "MR";
    } else {
        src->mic_layout = cfg->mic_layout;
    }
    return &src->base;
}

esp_capture_err_t room_capture_audio_aec_src_set_wakenet_enabled(
    esp_capture_audio_src_if_t *source,
    bool enabled)
{
    if (source == NULL) {
        return ESP_CAPTURE_ERR_INVALID_ARG;
    }
    audio_aec_src_t *src = (audio_aec_src_t *)source;
    src->wakenet_enabled = enabled;
    return ESP_CAPTURE_ERR_OK;
}

#endif  /* CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 */
