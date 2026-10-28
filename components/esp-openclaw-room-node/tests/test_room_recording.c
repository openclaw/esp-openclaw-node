#include "recording_host.h"
#include "room_recording.h"
#include "room_diagnostics_data.h"
#include "esp_ae_rate_cvt.h"
#include "esp_audio_enc.h"
#include "esp_g711_enc.h"

#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct test_queue {
    pthread_mutex_t lock;
    size_t capacity, size, used, read, write;
    uint8_t *data;
};
struct test_gate { pthread_mutex_t lock; bool available; unsigned gives; };
static struct test_gate gate = {PTHREAD_MUTEX_INITIALIZER, true, 0};
static pthread_t worker;
static void (*worker_fn)(void *);
static void *worker_arg;
static atomic_llong now_us = 1000000;
static atomic_bool fail_memory, fail_task, fail_codec, short_conversion;
static atomic_bool cancel_on_finish;
static atomic_uint codecs_open, codecs_closed, output_frees;
static void *output_allocation;
static pthread_mutex_t schedule_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t schedule_changed = PTHREAD_COND_INITIALIZER;
static bool pause_send, send_waiting, pause_process, process_waiting, pause_empty, empty_waiting;
static room_diagnostics_capture_owner_t capture_owner;

static void tick(void)
{
    struct timespec delay = {.tv_nsec = 1000000};
    nanosleep(&delay, NULL);
}
static room_recording_snapshot_t get_snapshot(void)
{
    room_recording_snapshot_t result;
    room_recording_snapshot(&result);
    return result;
}
static void wait_state(room_recording_state_t state)
{
    for (unsigned i = 0; i < 2000; ++i) {
        if (get_snapshot().state == state) return;
        tick();
    }
    fprintf(stderr, "state %d, expected %d\n", get_snapshot().state, state);
    abort();
}
static void join_worker(void)
{
    assert(pthread_join(worker, NULL) == 0);
    assert(!room_recording_active());
    assert(gate.available);
    assert(capture_owner == ROOM_DIAGNOSTICS_CAPTURE_AMBIENT);
    assert(atomic_load(&codecs_open) == atomic_load(&codecs_closed));
}
static void start(void)
{
    assert(room_recording_start(&gate) == ESP_OK);
    wait_state(ROOM_RECORDING_ACTIVE);
    assert(capture_owner == ROOM_DIAGNOSTICS_CAPTURE_RECORDING);
}
static void discard(void)
{
    room_recording_cancel();
    if (room_recording_active()) join_worker();
    assert(get_snapshot().state == ROOM_RECORDING_CANCELED);
}
static void wait_schedule(bool *flag)
{
    assert(pthread_mutex_lock(&schedule_lock) == 0);
    while (!*flag) pthread_cond_wait(&schedule_changed, &schedule_lock);
    assert(pthread_mutex_unlock(&schedule_lock) == 0);
}
static void resume(bool *pause)
{
    assert(pthread_mutex_lock(&schedule_lock) == 0);
    *pause = false;
    pthread_cond_broadcast(&schedule_changed);
    assert(pthread_mutex_unlock(&schedule_lock) == 0);
}
static void schedule_point(bool *pause, bool *waiting)
{
    assert(pthread_mutex_lock(&schedule_lock) == 0);
    if (*pause) {
        *waiting = true;
        pthread_cond_broadcast(&schedule_changed);
        while (*pause) pthread_cond_wait(&schedule_changed, &schedule_lock);
        *waiting = false;
    }
    assert(pthread_mutex_unlock(&schedule_lock) == 0);
}

QueueHandle_t xQueueCreate(UBaseType_t count, UBaseType_t size)
{
    QueueHandle_t q = calloc(1, sizeof(*q));
    assert(q != NULL && pthread_mutex_init(&q->lock, NULL) == 0);
    q->capacity = count; q->size = size;
    q->data = calloc(count, size);
    assert(q->data != NULL);
    return q;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    assert(wait == 0);
    schedule_point(&pause_send, &send_waiting);
    pthread_mutex_lock(&q->lock);
    bool room = q->used < q->capacity;
    if (room) {
        memcpy(q->data + q->write * q->size, item, q->size);
        q->write = (q->write + 1) % q->capacity;
        ++q->used;
    }
    pthread_mutex_unlock(&q->lock);
    return room;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    pthread_mutex_lock(&q->lock);
    bool present = q->used != 0;
    if (present) {
        memcpy(item, q->data + q->read * q->size, q->size);
        q->read = (q->read + 1) % q->capacity;
        --q->used;
    }
    pthread_mutex_unlock(&q->lock);
    if (!present && wait != 0) {
        schedule_point(&pause_empty, &empty_waiting);
        tick();
    }
    return present;
}
void vQueueDelete(QueueHandle_t q)
{
    assert(q->used == 0);
    pthread_mutex_destroy(&q->lock);
    free(q->data); free(q);
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t wait)
{
    assert(wait == 0);
    pthread_mutex_lock(&sem->lock);
    bool available = sem->available;
    sem->available = false;
    pthread_mutex_unlock(&sem->lock);
    return available;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    pthread_mutex_lock(&sem->lock);
    assert(!sem->available);
    sem->available = true;
    ++sem->gives;
    pthread_mutex_unlock(&sem->lock);
    return pdTRUE;
}
static void *run_worker(void *arg)
{
    (void)arg;
    worker_fn(worker_arg);
    return NULL;
}
BaseType_t xTaskCreateWithCaps(void (*fn)(void *), const char *name, uint32_t stack,
    void *arg, UBaseType_t priority, TaskHandle_t *task, UBaseType_t caps)
{
    (void)name; (void)stack; (void)priority; (void)task; (void)caps;
    if (atomic_load(&fail_task)) return pdFALSE;
    worker_fn = fn; worker_arg = arg;
    assert(pthread_create(&worker, NULL, run_worker, NULL) == 0);
    return pdPASS;
}
void vTaskDeleteWithCaps(TaskHandle_t task) { (void)task; }
void vTaskDelay(TickType_t ticks) { (void)ticks; tick(); }
int64_t esp_timer_get_time(void) { return atomic_load(&now_us); }
void *heap_caps_malloc(size_t bytes, unsigned caps)
{
    assert(bytes == 240000 && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (atomic_load(&fail_memory)) return NULL;
    output_allocation = malloc(bytes);
    return output_allocation;
}
void recording_host_free(void *pointer)
{
    if (pointer != NULL && pointer == output_allocation) {
        ++output_frees;
        output_allocation = NULL;
    }
    free(pointer);
}
void room_diagnostics_audio_set_capture_owner(room_diagnostics_capture_owner_t owner)
{
    capture_owner = owner;
    if (owner == ROOM_DIAGNOSTICS_CAPTURE_AMBIENT && atomic_exchange(&cancel_on_finish, false)) {
        room_recording_cancel();
    }
}

esp_ae_err_t esp_ae_rate_cvt_open(esp_ae_rate_cvt_cfg_t *cfg, esp_ae_rate_cvt_handle_t *handle)
{
    assert(cfg->src_rate == 16000 && cfg->dest_rate == 8000 && cfg->channel == 1 && cfg->bits_per_sample == 16);
    if (atomic_load(&fail_codec)) return ESP_AE_ERR_FAIL;
    *handle = malloc(1); assert(*handle != NULL); ++codecs_open;
    return ESP_AE_ERR_OK;
}
esp_ae_err_t esp_ae_rate_cvt_get_max_out_sample_num(esp_ae_rate_cvt_handle_t handle, uint32_t in, uint32_t *out)
{
    assert(handle != NULL); *out = in / 2 + 8; return ESP_AE_ERR_OK;
}
esp_ae_err_t esp_ae_rate_cvt_process(esp_ae_rate_cvt_handle_t handle, esp_ae_sample_t input,
    uint32_t in, esp_ae_sample_t output, uint32_t *out)
{
    assert(handle != NULL && *out >= in / 2);
    schedule_point(&pause_process, &process_waiting);
    *out = atomic_load(&short_conversion) ? in / 4 : in / 2;
    for (uint32_t i = 0; i < *out; ++i) ((int16_t *)output)[i] = ((int16_t *)input)[i * 2];
    return ESP_AE_ERR_OK;
}
void esp_ae_rate_cvt_close(esp_ae_rate_cvt_handle_t handle) { free(handle); ++codecs_closed; }
esp_audio_err_t esp_audio_enc_open(esp_audio_enc_config_t *cfg, esp_audio_enc_handle_t *handle)
{
    esp_g711_enc_config_t *g711 = cfg->cfg;
    assert(cfg->type == ESP_AUDIO_TYPE_G711U && g711->sample_rate == 8000 && g711->channel == 1);
    *handle = malloc(1); assert(*handle != NULL); ++codecs_open;
    return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_audio_enc_get_frame_size(esp_audio_enc_handle_t handle, int *in, int *out)
{
    assert(handle != NULL); *in = 2; *out = 1; return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_audio_enc_process(esp_audio_enc_handle_t handle,
    esp_audio_enc_in_frame_t *in, esp_audio_enc_out_frame_t *out)
{
    assert(handle != NULL && in->len / 2 == out->len);
    out->encoded_bytes = in->len / 2;
    for (uint32_t i = 0; i < out->encoded_bytes; ++i) out->buffer[i] = (uint8_t)((int16_t *)in->buffer)[i];
    return ESP_AUDIO_ERR_OK;
}
void esp_audio_enc_close(esp_audio_enc_handle_t handle) { free(handle); ++codecs_closed; }

/* Minimal capture SDK boundary. Frame storage is poisoned on release. */
typedef struct { int stream_type; uint32_t pts; uint8_t *data; int size; } esp_capture_stream_frame_t;
#define ESP_CAPTURE_STREAM_TYPE_AUDIO 1
#define ESP_CAPTURE_ERR_OK 0
typedef bool (*room_media_talk_busy_cb_t)(void *);
static void *wake_sink;
static SemaphoreHandle_t media_owner_gate = &gate;
static bool media_initialized = true;
static void (*wake_callback)(const char *, void *);
static void *wake_callback_ctx;
static jmp_buf drain_finished;
static int16_t capture_pcm[320];
static unsigned acquired, released, wakes;
static int esp_capture_sink_acquire_frame(void *sink, esp_capture_stream_frame_t *frame, bool no_wait)
{
    (void)sink; (void)no_wait;
    if (acquired++) longjmp(drain_finished, 1);
    frame->data = (uint8_t *)capture_pcm;
    frame->size = sizeof(capture_pcm);
    return ESP_CAPTURE_ERR_OK;
}
static void esp_capture_sink_release_frame(void *sink, esp_capture_stream_frame_t *frame)
{
    (void)sink;
    memset(frame->data, 0, (size_t)frame->size);
    ++released;
}
#include "media_boundary.h"

static void woke(const char *word, void *ctx) { (void)word; (void)ctx; ++wakes; }
static void *feed_thread(void *arg) { room_recording_feed(arg, 640); return NULL; }

int main(void)
{
    int16_t pcm[2048];
    for (size_t i = 0; i < 2048; ++i) pcm[i] = 37;
    uint8_t *audio = NULL;
    size_t bytes = 0;
    wake_callback = woke;

    start();
    assert(room_media_begin_talk() == ESP_ERR_INVALID_STATE);
    assert(room_recording_start(&gate) == ESP_ERR_INVALID_STATE);
    room_afe_wake(NULL); assert(wakes == 0);
    for (size_t i = 0; i < 320; ++i) capture_pcm[i] = 37;
    if (setjmp(drain_finished) == 0) wake_drain_task(NULL);
    assert(released == 1 && capture_pcm[0] == 0);
    room_recording_stop(); join_worker();
    assert(room_recording_take(&audio, &bytes) == ESP_OK && bytes == 160);
    for (size_t i = 0; i < bytes; ++i) assert(audio[i] == 37);
    recording_host_free(audio);
    room_afe_wake(NULL); assert(wakes == 1);
    assert(room_media_begin_talk() == ESP_OK);
    assert(room_media_request_recording(NULL, NULL) == ESP_ERR_INVALID_STATE);
    room_media_end_talk(true);
    puts("PASS copied capture frames, wake suppression, and exclusive media admission");

    start();
    pthread_mutex_lock(&schedule_lock);
    pause_send = true;
    pause_empty = true;
    pthread_mutex_unlock(&schedule_lock);
    pthread_t producer;
    assert(pthread_create(&producer, NULL, feed_thread, pcm) == 0);
    wait_schedule(&send_waiting);
    room_recording_stop();
    wait_schedule(&empty_waiting);
    assert(room_recording_active() && !gate.available);
    resume(&pause_send);
    assert(pthread_join(producer, NULL) == 0);
    resume(&pause_empty);
    join_worker();
    assert(room_recording_take(&audio, &bytes) == ESP_OK && bytes == 160);
    recording_host_free(audio);
    puts("PASS stop waits for admitted producer before freeing queue");

    start();
    pthread_mutex_lock(&schedule_lock);
    pause_process = true;
    pthread_mutex_unlock(&schedule_lock);
    room_recording_feed(pcm, 1024);
    wait_schedule(&process_waiting);
    for (unsigned i = 0; i < 3; ++i) room_recording_feed(pcm, sizeof(pcm));
    assert(get_snapshot().error == ROOM_RECORDING_ERROR_OVERFLOW);
    resume(&pause_process); join_worker();
    assert(get_snapshot().state == ROOM_RECORDING_FAILED && get_snapshot().audio_bytes == 0);
    assert(room_recording_take(&audio, &bytes) == ESP_ERR_INVALID_STATE);
    puts("PASS full queue fails and discards rather than silently dropping audio");

    start(); room_recording_feed(pcm, 640); discard();
    assert(room_recording_take(&audio, &bytes) == ESP_ERR_INVALID_STATE);
    start(); room_recording_feed(pcm, 640);
    atomic_store(&cancel_on_finish, true);
    room_recording_stop(); join_worker();
    assert(get_snapshot().state == ROOM_RECORDING_CANCELED && get_snapshot().audio_bytes == 0);
    start(); room_recording_feed(pcm, 641); join_worker();
    assert(get_snapshot().error == ROOM_RECORDING_ERROR_SOURCE);
    start(); room_recording_stop(); join_worker();
    assert(get_snapshot().error == ROOM_RECORDING_ERROR_EMPTY);
    start(); atomic_fetch_add(&now_us, 2000001); join_worker();
    assert(get_snapshot().error == ROOM_RECORDING_ERROR_SOURCE);
    puts("PASS cancellation, malformed/empty input, and stalled source cleanup");

    start(); room_recording_feed(pcm, 640);
    while (get_snapshot().audio_bytes == 0) tick();
    atomic_fetch_add(&now_us, 30000001); join_worker();
    assert(room_recording_take(&audio, &bytes) == ESP_OK && bytes == 160);
    recording_host_free(audio);
    atomic_store(&short_conversion, true);
    start(); room_recording_feed(pcm, 644); room_recording_stop(); join_worker();
    assert(room_recording_take(&audio, &bytes) == ESP_OK && bytes == 80);
    recording_host_free(audio); atomic_store(&short_conversion, false);
    puts("PASS wall-clock cap and actual converter output length");

    start();
    size_t fed = 0;
    while (fed < 480000) {
        size_t count = 480000 - fed;
        if (count > 512) count = 512;
        room_recording_feed(pcm, count * 2);
        fed += count;
        while (get_snapshot().audio_bytes < fed / 2 && room_recording_active()) tick();
    }
    join_worker();
    assert(room_recording_take(&audio, &bytes) == ESP_OK && bytes == 240000);
    recording_host_free(audio);
    puts("PASS input and encoded output cannot exceed 30 seconds");

    atomic_store(&fail_memory, true);
    assert(room_recording_start(&gate) == ESP_OK); join_worker();
    assert(get_snapshot().error == ROOM_RECORDING_ERROR_MEMORY);
    atomic_store(&fail_memory, false); atomic_store(&fail_codec, true);
    assert(room_recording_start(&gate) == ESP_OK); join_worker();
    assert(get_snapshot().error == ROOM_RECORDING_ERROR_CODEC);
    atomic_store(&fail_codec, false); atomic_store(&fail_task, true);
    assert(room_recording_start(&gate) == ESP_ERR_NO_MEM && gate.available);
    atomic_store(&fail_task, false);
    start(); room_recording_feed(pcm, 640); room_recording_stop(); join_worker();
    unsigned freed = output_frees;
    assert(room_recording_start(&gate) == ESP_ERR_INVALID_STATE);
    discard(); assert(output_frees == freed + 1);
    puts("PASS allocation/codec/task failures release ownership and unconsumed drafts block replacement");
    return 0;
}
