#include "room_recording.h"

#include <stdlib.h>
#include <string.h>

#include "esp_ae_rate_cvt.h"
#include "esp_audio_enc.h"
#include "esp_g711_enc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "room_diagnostics_data.h"

enum {
    CHUNK_SAMPLES = 512,
    QUEUE_CHUNKS = 8,
    MAX_FRAME_SAMPLES = 2048,
    MAX_INPUT_SAMPLES = 16000 * 30,
    MAX_AUDIO_BYTES = 8000 * 30,
    MAX_OUTPUT_SAMPLES = 1024,
};

typedef struct {
    size_t samples;
    int16_t pcm[CHUNK_SAMPLES];
} recording_chunk_t;

static portMUX_TYPE recording_mux = portMUX_INITIALIZER_UNLOCKED;
static room_recording_snapshot_t snapshot;
static bool worker_active;
static bool canceled;
static unsigned producers_inflight;
static size_t input_samples;
static QueueHandle_t pcm_queue;
static SemaphoreHandle_t owner_gate;
static uint8_t *draft;

static void fail_recording(room_recording_error_t error)
{
    taskENTER_CRITICAL(&recording_mux);
    if (worker_active) {
        if (snapshot.error == ROOM_RECORDING_ERROR_NONE) snapshot.error = error;
        snapshot.state = ROOM_RECORDING_STOPPING;
    }
    taskEXIT_CRITICAL(&recording_mux);
}

static void finish_recording(uint8_t *audio, size_t bytes)
{
    /* Admission is closed and every admitted producer has returned. Only this
     * worker can destroy the queue or hand ownership back to Live Talk. */
    vQueueDelete(pcm_queue);
    pcm_queue = NULL;
    room_diagnostics_audio_set_capture_owner(ROOM_DIAGNOSTICS_CAPTURE_AMBIENT);
    taskENTER_CRITICAL(&recording_mux);
    if (canceled || snapshot.error != ROOM_RECORDING_ERROR_NONE || bytes == 0) {
        if (!canceled && snapshot.error == ROOM_RECORDING_ERROR_NONE) {
            snapshot.error = ROOM_RECORDING_ERROR_EMPTY;
        }
        snapshot.state = canceled ? ROOM_RECORDING_CANCELED : ROOM_RECORDING_FAILED;
        snapshot.audio_bytes = 0;
    } else {
        draft = audio;
        audio = NULL;
        snapshot.audio_bytes = bytes;
        snapshot.state = ROOM_RECORDING_READY;
    }
    /* Publish completion atomically: a concurrent stop/cancel must never
     * leave STOPPING after the last worker has exited. The gate remains held
     * until any discarded allocation has also been released. */
    worker_active = false;
    taskEXIT_CRITICAL(&recording_mux);
    free(audio);
    xSemaphoreGive(owner_gate);
}

static void recording_task(void *arg)
{
    (void)arg;
    uint8_t *audio = heap_caps_malloc(MAX_AUDIO_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *converted = NULL;
    size_t bytes = 0;
    esp_ae_rate_cvt_handle_t converter = NULL;
    esp_audio_enc_handle_t encoder = NULL;
    esp_ae_rate_cvt_cfg_t rate = {
        .src_rate = 16000, .dest_rate = 8000, .channel = 1, .bits_per_sample = 16,
        .complexity = 3, .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_MEMORY,
    };
    esp_g711_enc_config_t g711 = ESP_G711_ENC_CONFIG_DEFAULT();
    esp_audio_enc_config_t config = {
        .type = ESP_AUDIO_TYPE_G711U, .cfg = &g711, .cfg_sz = sizeof(g711),
    };
    uint32_t capacity = 0;
    int in_size = 0, out_size = 0;
    if (audio == NULL) {
        fail_recording(ROOM_RECORDING_ERROR_MEMORY);
    } else if (esp_ae_rate_cvt_open(&rate, &converter) != ESP_AE_ERR_OK ||
               esp_ae_rate_cvt_get_max_out_sample_num(converter, CHUNK_SAMPLES, &capacity) != ESP_AE_ERR_OK ||
               capacity == 0 || capacity > MAX_OUTPUT_SAMPLES ||
               esp_audio_enc_open(&config, &encoder) != ESP_AUDIO_ERR_OK ||
               esp_audio_enc_get_frame_size(encoder, &in_size, &out_size) != ESP_AUDIO_ERR_OK ||
               in_size != sizeof(int16_t) || out_size != 1) {
        fail_recording(ROOM_RECORDING_ERROR_CODEC);
    } else if ((converted = malloc(capacity * sizeof(*converted))) == NULL) {
        fail_recording(ROOM_RECORDING_ERROR_MEMORY);
    }
    int64_t started = esp_timer_get_time();
    int64_t last_frame = started;
    taskENTER_CRITICAL(&recording_mux);
    if (snapshot.state == ROOM_RECORDING_STARTING) snapshot.state = ROOM_RECORDING_ACTIVE;
    taskEXIT_CRITICAL(&recording_mux);
    recording_chunk_t chunk;
    for (;;) {
        bool received = xQueueReceive(pcm_queue, &chunk, pdMS_TO_TICKS(10)) == pdTRUE;
        int64_t now = esp_timer_get_time();
        taskENTER_CRITICAL(&recording_mux);
        if (snapshot.state == ROOM_RECORDING_ACTIVE) {
            if (now - started >= 30000000) snapshot.state = ROOM_RECORDING_STOPPING;
            else if (now - last_frame >= 2000000 && !received) {
                snapshot.error = ROOM_RECORDING_ERROR_SOURCE;
                snapshot.state = ROOM_RECORDING_STOPPING;
            }
        }
        bool discard = canceled || snapshot.error != ROOM_RECORDING_ERROR_NONE;
        bool done = !received && snapshot.state == ROOM_RECORDING_STOPPING && producers_inflight == 0;
        taskEXIT_CRITICAL(&recording_mux);
        /* A producer may have enqueued between the timed receive and the
         * state lock. With admission closed and no producers left, this last
         * nonblocking receive is the stable empty-queue boundary. */
        if (done) {
            received = xQueueReceive(pcm_queue, &chunk, 0) == pdTRUE;
            if (!received) break;
        }
        if (!received) continue;
        last_frame = now;
        if (discard) continue;
        uint32_t samples = capacity;
        if (esp_ae_rate_cvt_process(converter, chunk.pcm, chunk.samples, converted, &samples) != ESP_AE_ERR_OK ||
            samples > capacity) {
            fail_recording(ROOM_RECORDING_ERROR_CODEC);
            continue;
        }
        if (samples > MAX_AUDIO_BYTES - bytes) samples = MAX_AUDIO_BYTES - bytes;
        if (samples != 0) {
            esp_audio_enc_in_frame_t input = {.buffer = (uint8_t *)converted, .len = samples * sizeof(*converted)};
            esp_audio_enc_out_frame_t output = {.buffer = audio + bytes, .len = samples};
            if (esp_audio_enc_process(encoder, &input, &output) != ESP_AUDIO_ERR_OK ||
                output.encoded_bytes != samples) {
                fail_recording(ROOM_RECORDING_ERROR_CODEC);
                continue;
            }
            bytes += output.encoded_bytes;
            taskENTER_CRITICAL(&recording_mux);
            snapshot.audio_bytes = bytes;
            taskEXIT_CRITICAL(&recording_mux);
        }
        if (bytes == MAX_AUDIO_BYTES) room_recording_stop();
    }
    if (encoder != NULL) esp_audio_enc_close(encoder);
    if (converter != NULL) esp_ae_rate_cvt_close(converter);
    free(converted);
    finish_recording(audio, bytes);
    vTaskDeleteWithCaps(NULL);
}

esp_err_t room_recording_start(SemaphoreHandle_t media_gate)
{
    if (media_gate == NULL || xSemaphoreTake(media_gate, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    taskENTER_CRITICAL(&recording_mux);
    if (worker_active || draft != NULL) {
        taskEXIT_CRITICAL(&recording_mux);
        xSemaphoreGive(media_gate);
        return ESP_ERR_INVALID_STATE;
    }
    worker_active = true;
    canceled = false;
    input_samples = 0;
    snapshot = (room_recording_snapshot_t){.state = ROOM_RECORDING_STARTING};
    owner_gate = media_gate;
    taskEXIT_CRITICAL(&recording_mux);
    pcm_queue = xQueueCreate(QUEUE_CHUNKS, sizeof(recording_chunk_t));
    if (pcm_queue == NULL) {
        taskENTER_CRITICAL(&recording_mux);
        snapshot.state = ROOM_RECORDING_FAILED;
        snapshot.error = ROOM_RECORDING_ERROR_MEMORY;
        worker_active = false;
        taskEXIT_CRITICAL(&recording_mux);
        xSemaphoreGive(media_gate);
        return ESP_ERR_NO_MEM;
    }
    room_diagnostics_audio_set_capture_owner(ROOM_DIAGNOSTICS_CAPTURE_RECORDING);
    if (xTaskCreateWithCaps(recording_task, "voice_record", 6144, NULL, 5, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        fail_recording(ROOM_RECORDING_ERROR_MEMORY);
        finish_recording(NULL, 0);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool room_recording_active(void)
{
    taskENTER_CRITICAL(&recording_mux);
    bool active = worker_active;
    taskEXIT_CRITICAL(&recording_mux);
    return active;
}

void room_recording_feed(const void *pcm, size_t bytes)
{
    taskENTER_CRITICAL(&recording_mux);
    if (snapshot.state != ROOM_RECORDING_ACTIVE) {
        taskEXIT_CRITICAL(&recording_mux);
        return;
    }
    ++producers_inflight;
    size_t samples = bytes / sizeof(int16_t);
    bool valid = pcm != NULL && bytes != 0 && bytes % sizeof(int16_t) == 0 && samples <= MAX_FRAME_SAMPLES;
    if (samples > MAX_INPUT_SAMPLES - input_samples) samples = MAX_INPUT_SAMPLES - input_samples;
    input_samples += samples;
    bool limit = input_samples == MAX_INPUT_SAMPLES;
    taskEXIT_CRITICAL(&recording_mux);
    if (!valid) {
        fail_recording(ROOM_RECORDING_ERROR_SOURCE);
    } else {
        recording_chunk_t chunk;
        const uint8_t *source = pcm;
        for (size_t offset = 0; offset < samples; offset += chunk.samples) {
            chunk.samples = samples - offset;
            if (chunk.samples > CHUNK_SAMPLES) chunk.samples = CHUNK_SAMPLES;
            memcpy(chunk.pcm, source + offset * sizeof(int16_t), chunk.samples * sizeof(int16_t));
            if (xQueueSend(pcm_queue, &chunk, 0) != pdTRUE) {
                fail_recording(ROOM_RECORDING_ERROR_OVERFLOW);
                break;
            }
        }
    }
    taskENTER_CRITICAL(&recording_mux);
    if (limit) snapshot.state = ROOM_RECORDING_STOPPING;
    --producers_inflight;
    taskEXIT_CRITICAL(&recording_mux);
}

void room_recording_stop(void)
{
    taskENTER_CRITICAL(&recording_mux);
    if (worker_active) snapshot.state = ROOM_RECORDING_STOPPING;
    taskEXIT_CRITICAL(&recording_mux);
}

void room_recording_cancel(void)
{
    taskENTER_CRITICAL(&recording_mux);
    uint8_t *discard = draft;
    draft = NULL;
    if (worker_active) {
        canceled = true;
        snapshot.state = ROOM_RECORDING_STOPPING;
    } else {
        snapshot = (room_recording_snapshot_t){.state = ROOM_RECORDING_CANCELED};
    }
    taskEXIT_CRITICAL(&recording_mux);
    free(discard);
}

void room_recording_snapshot(room_recording_snapshot_t *result)
{
    if (result == NULL) return;
    taskENTER_CRITICAL(&recording_mux);
    *result = snapshot;
    taskEXIT_CRITICAL(&recording_mux);
}

esp_err_t room_recording_take(uint8_t **audio, size_t *bytes)
{
    if (audio == NULL || bytes == NULL) return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&recording_mux);
    if (worker_active || snapshot.state != ROOM_RECORDING_READY || draft == NULL) {
        taskEXIT_CRITICAL(&recording_mux);
        return ESP_ERR_INVALID_STATE;
    }
    *audio = draft;
    *bytes = snapshot.audio_bytes;
    draft = NULL;
    snapshot = (room_recording_snapshot_t){.state = ROOM_RECORDING_IDLE};
    taskEXIT_CRITICAL(&recording_mux);
    return ESP_OK;
}
