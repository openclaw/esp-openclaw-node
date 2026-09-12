#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_SIZE 0x104
#define BSP_FAILURE 0x105
#define TAG "camera-fixture"
#define CAMERA_SENSOR_WIDTH 1280U
#define CAMERA_SENSOR_HEIGHT 720U
#define BSP_CAMERA_DEVICE "synthetic-camera-path-canary"
#define O_RDONLY 0
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
/* ESP-IDF's VFS mmap failure sentinel is NULL, not the POSIX host sentinel. */
#define MAP_FAILED NULL
#define V4L2_BUF_TYPE_VIDEO_CAPTURE 1
#define V4L2_MEMORY_MMAP 1
#define V4L2_PIX_FMT_RGB565 0x50424752U
#define V4L2_PIX_FMT_RGB565X 0x52424752U
#define FRAME_SIZE (CAMERA_SENSOR_WIDTH * CAMERA_SENSOR_HEIGHT * 2U)
enum { VIDIOC_G_FMT, VIDIOC_S_FMT, VIDIOC_REQBUFS, VIDIOC_QUERYBUF,
       VIDIOC_QBUF, VIDIOC_STREAMON, VIDIOC_DQBUF, VIDIOC_STREAMOFF,
       VIDIOC_S_DQBUF_TIMEOUT };
typedef int esp_err_t;
struct v4l2_format {
    uint32_t type;
    union { struct { uint32_t width, height, pixelformat, bytesperline; } pix; } fmt;
};
struct v4l2_requestbuffers { uint32_t count, type, memory; };
struct v4l2_buffer {
    uint32_t type, memory, index, length, bytesused;
    union { uint32_t offset; } m;
};
typedef struct { char stage[40], domain[16]; int error; } record_t;
static record_t records[2];
typedef struct { char stage[40]; uint64_t elapsed_ms; } pipeline_record_t;
static pipeline_record_t pipeline_records[10];
static unsigned pipeline_count, timeout_sets;
static uint64_t dequeue_timeout_ms;
static unsigned record_count, log_count, bsp_calls, gfmt_calls, maps, unmaps;
static unsigned closes, streamoffs, dequeues, initial_queues, requeues;
static int64_t clock_us;
static uint8_t mapped[2][8];
static bool live_mapping[2];
static const char *scenario;
static char validation_detail[512];

static bool is(const char *name) { return strcmp(scenario, name) == 0; }

static void capture_log(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    assert(length >= 0 && (size_t)length < sizeof(line));
    assert(strstr(line, BSP_CAMERA_DEVICE) == NULL);
    ++log_count;
    if (strncmp(line, "camera_capture_diag ", 20) == 0) {
        assert(record_count < 2);
        record_t *record = &records[record_count++];
        int consumed = 0;
        assert(sscanf(line, "camera_capture_diag stage=%39s domain=%15s error=%d%n",
                      record->stage, record->domain, &record->error, &consumed) == 3);
        assert(line[consumed] == '\0');
    } else if (strncmp(line, "camera_pipeline_diag ", 21) == 0) {
        assert(pipeline_count < 10);
        pipeline_record_t *record = &pipeline_records[pipeline_count++];
        int consumed = 0;
        assert(sscanf(line, "camera_pipeline_diag stage=%39s elapsed_ms=%" SCNu64 "%n",
                      record->stage, &record->elapsed_ms, &consumed) == 2);
        assert(line[consumed] == '\0');
        if (strstr(record->stage, "_begin") != NULL) assert(record->elapsed_ms == 0);
        for (unsigned i = 0; i + 1 < pipeline_count; ++i) {
            assert(strcmp(record->stage, pipeline_records[i].stage) != 0);
        }
    } else {
        memcpy(validation_detail, line, (size_t)length + 1);
    }
    errno = EBUSY;
}
#define ESP_LOGE(tag, ...) do { (void)(tag); capture_log(__VA_ARGS__); } while (0)
#define ESP_LOGI(tag, ...) do { (void)(tag); capture_log(__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, ...) do { (void)(tag); capture_log(__VA_ARGS__); } while (0)
#define ESP_RETURN_ON_ERROR(call, tag, ...) do { \
    esp_err_t error = (call); \
    if (error != ESP_OK) { ESP_LOGE(tag, __VA_ARGS__); return error; } \
} while (0)

static esp_err_t bsp_camera_start(const void *config)
{
    assert(config == NULL);
    ++bsp_calls;
    errno = ESTALE;
    return is("bsp") || is("bsp-retry") ? BSP_FAILURE : ESP_OK;
}
static int camera_open(const char *path, int flags)
{
    assert(strcmp(path, BSP_CAMERA_DEVICE) == 0 && flags == O_RDONLY);
    dequeue_timeout_ms = UINT64_MAX;
    errno = EIO;
    return is("open") || is("open-cache") ? -1 : 7;
}
static int camera_close(int fd)
{
    assert(fd == 7);
    ++closes;
    clock_us += 3000;
    errno = EBADF;
    return -1;
}
static void *camera_mmap(void *address, size_t length, int protection,
                         int flags, int fd, uint32_t offset)
{
    assert(address == NULL && length == FRAME_SIZE && fd == 7 && offset < 2);
    assert(protection == (PROT_READ | PROT_WRITE) && flags == MAP_SHARED);
    if ((offset == 0 && is("mmap0")) || (offset == 1 && is("mmap1"))) {
        errno = EIO;
        return MAP_FAILED;
    }
    assert(!live_mapping[offset]);
    live_mapping[offset] = true;
    ++maps;
    return mapped[offset];
}
static int camera_munmap(void *address, size_t length)
{
    assert(length == FRAME_SIZE);
    unsigned index = address == mapped[0] ? 0 : 1;
    assert(address == mapped[index] && live_mapping[index]);
    live_mapping[index] = false;
    ++unmaps;
    clock_us += 2000;
    errno = EBADF;
    return -1;
}
static int camera_ioctl(int fd, int request, ...)
{
    va_list args;
    va_start(args, request);
    void *argument = va_arg(args, void *);
    va_end(args);
    assert(fd == 7);
    errno = ESTALE;
    if (request == VIDIOC_STREAMOFF) {
        assert(*(int *)argument == V4L2_BUF_TYPE_VIDEO_CAPTURE);
        ++streamoffs;
        clock_us += 1000;
        errno = EBADF;
        return -1;
    }
    if (request == VIDIOC_S_DQBUF_TIMEOUT) {
        const struct timeval *timeout = argument;
        assert(gfmt_calls == 0 && maps == 0 && initial_queues == 0);
        ++timeout_sets;
        if (is("timeout-setup")) {
            errno = ENOTTY;
            return -1;
        }
        assert(timeout->tv_sec == 2 && timeout->tv_usec == 0);
        dequeue_timeout_ms = (uint64_t)timeout->tv_sec * 1000 + timeout->tv_usec / 1000;
    } else if (request == VIDIOC_G_FMT) {
        struct v4l2_format *format = argument;
        ++gfmt_calls;
        if ((gfmt_calls == 1 && is("gfmt")) || (gfmt_calls == 2 && is("gfmt-after"))) goto failure;
        format->fmt.pix.width = CAMERA_SENSOR_WIDTH;
        format->fmt.pix.height = CAMERA_SENSOR_HEIGHT;
        format->fmt.pix.pixelformat = is("rgb565x") ? V4L2_PIX_FMT_RGB565X : V4L2_PIX_FMT_RGB565;
        format->fmt.pix.bytesperline = is("explicit-stride") ? CAMERA_SENSOR_WIDTH * 2 : 0;
        if (gfmt_calls == 1 && is("default-size")) format->fmt.pix.width = 640;
        if (gfmt_calls == 2) {
            if (is("negotiated-size")) format->fmt.pix.height = 480;
            if (is("negotiated-format")) format->fmt.pix.pixelformat = 0;
            if (is("negotiated-stride")) format->fmt.pix.bytesperline = 1;
        }
    } else if (request == VIDIOC_S_FMT) {
        const struct v4l2_format *format = argument;
        assert(format->fmt.pix.width == CAMERA_SENSOR_WIDTH);
        assert(format->fmt.pix.height == CAMERA_SENSOR_HEIGHT);
        assert(format->fmt.pix.pixelformat == V4L2_PIX_FMT_RGB565);
        if (is("sfmt")) goto failure;
    } else if (request == VIDIOC_REQBUFS) {
        struct v4l2_requestbuffers *buffers = argument;
        assert(buffers->count == 2 && buffers->memory == V4L2_MEMORY_MMAP);
        if (is("reqbufs")) goto failure;
        if (is("buffer-count")) buffers->count = 1;
    } else if (request == VIDIOC_QUERYBUF) {
        struct v4l2_buffer *buffer = argument;
        assert(buffer->index < 2);
        if ((buffer->index == 0 && is("query0")) || (buffer->index == 1 && is("query1"))) goto failure;
        buffer->length = buffer->index == 1 && is("buffer-length") ? 1 : FRAME_SIZE;
        buffer->m.offset = buffer->index;
    } else if (request == VIDIOC_QBUF) {
        const struct v4l2_buffer *buffer = argument;
        assert(buffer->index < 2);
        if (dequeues == 0) {
            ++initial_queues;
            if ((buffer->index == 0 && is("qbuf0")) || (buffer->index == 1 && is("qbuf1"))) goto failure;
        } else {
            ++requeues;
            if (is("requeue")) goto failure;
        }
    } else if (request == VIDIOC_STREAMON) {
        assert(*(int *)argument == V4L2_BUF_TYPE_VIDEO_CAPTURE);
        assert(initial_queues == 2);
        if (is("streamon")) goto failure;
    } else if (request == VIDIOC_DQBUF) {
        struct v4l2_buffer *buffer = argument;
        ++dequeues;
        if (is("missing-frame") || (is("warmup-starved") && dequeues > 1)) {
            /* Model the SDK's default infinite ready-semaphore wait without hanging the test. */
            assert(dequeue_timeout_ms > 0 && dequeue_timeout_ms != UINT64_MAX);
            clock_us += (int64_t)dequeue_timeout_ms * 1000;
            errno = ETIMEDOUT;
            return -1;
        }
        clock_us += is("warmup-max") ? 250000 : 1000;
        if (is("dqbuf")) goto failure;
        buffer->index = is("frame-index") ? 2 : (dequeues - 1) % 2;
        buffer->bytesused = is("frame-length") ? 1 : is("zero-bytesused") ? 0 : FRAME_SIZE;
    } else {
        assert(false);
    }
    return 0;
failure:
    errno = EIO;
    return -1;
}
static int64_t esp_timer_get_time(void)
{
    return clock_us;
}
#define open camera_open
#define close camera_close
#define mmap camera_mmap
#define munmap camera_munmap
#define ioctl camera_ioctl
#include "camera_capture_under_test.inc"

/* Run the actual command owner with valid parameters and observable dependency boundaries. */
typedef struct cJSON {
    int kind, valueint;
    double valuedouble;
    const char *valuestring, *string;
    struct cJSON *child, *next;
} cJSON;
static cJSON parameters, delay_parameter;
static unsigned parameter_deletes, indicator_ends, releases, transforms, encodes;
static bool lease_held, indicator_active;
typedef void *esp_openclaw_node_handle_t;
typedef struct { const char *code, *message; } esp_openclaw_node_error_t;
static cJSON *cJSON_ParseWithLength(const char *text, size_t length)
{
    assert(length == 2 && strcmp(text, "{}") == 0);
    parameters = (cJSON){.kind = 1};
    if (is("warmup-max") || is("warmup-starved")) {
        delay_parameter = (cJSON){.kind = 2, .valueint = 10000, .valuedouble = 10000,
                                 .string = "delayMs"};
        parameters.child = &delay_parameter;
    }
    return &parameters;
}
static cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *name)
{
    assert(object == &parameters);
    return strcmp(name, "delayMs") == 0 ? parameters.child : NULL;
}
static bool cJSON_IsObject(const cJSON *item) { return item != NULL && item->kind == 1; }
static bool cJSON_IsNumber(const cJSON *item) { return item != NULL && item->kind == 2; }
static bool cJSON_IsString(const cJSON *item) { return item != NULL && item->kind == 3; }
static void cJSON_Delete(cJSON *item) { assert(item == &parameters); ++parameter_deletes; }
static bool esp_openclaw_room_node_try_acquire_camera(void)
{
    assert(!lease_held);
    lease_held = true;
    return true;
}
static esp_err_t esp_openclaw_room_node_camera_indicator_begin(void)
{
    assert(lease_held && !indicator_active);
    indicator_active = true;
    return ESP_OK;
}
static void esp_openclaw_room_node_camera_indicator_end(void)
{
    assert(indicator_active && lease_held && closes == 1);
    indicator_active = false;
    ++indicator_ends;
    clock_us += 2000;
}
static void esp_openclaw_room_node_release_camera(void)
{
    assert(lease_held && !indicator_active);
    lease_held = false;
    ++releases;
    clock_us += 3000;
}
typedef struct {
    uint8_t *data;
    size_t data_size;
    uint32_t width, height;
} camera_transformed_frame_t;
static esp_err_t transform_camera_frame(const camera_frame_t *camera, int width,
                                        camera_transformed_frame_t *output)
{
    assert(camera->fd == 7 && camera->frame.index < 2 && width == 1024);
    assert(lease_held && indicator_active && unmaps == 0);
    ++transforms;
    clock_us += 7000;
    if (is("transform-failure")) return ESP_FAIL;
    *output = (camera_transformed_frame_t){.width = 800, .height = 800, .data_size = 800 * 800 * 3};
    output->data = malloc(output->data_size);
    assert(output->data != NULL);
    return ESP_OK;
}
static void heap_caps_free(void *memory) { free(memory); }
typedef struct {
    int width, height, src_type, subsampling;
    uint8_t quality;
    bool task_enable;
} jpeg_enc_config_t;
typedef void *jpeg_enc_handle_t;
typedef int jpeg_error_t;
#define DEFAULT_JPEG_ENC_CONFIG() ((jpeg_enc_config_t){0})
#define JPEG_PIXEL_FORMAT_RGB888 1
#define JPEG_SUBSAMPLE_420 2
#define JPEG_ERR_OK 0
static int encoder_token;
static jpeg_error_t jpeg_enc_open(const jpeg_enc_config_t *config, jpeg_enc_handle_t *encoder)
{
    assert(config->width == 800 && config->height == 800 && !config->task_enable);
    assert(config->src_type == JPEG_PIXEL_FORMAT_RGB888 && config->subsampling == JPEG_SUBSAMPLE_420);
    *encoder = &encoder_token;
    return JPEG_ERR_OK;
}
static jpeg_error_t jpeg_enc_process(jpeg_enc_handle_t encoder, const uint8_t *input,
                                    int input_size, uint8_t *output, int capacity, int *size)
{
    assert(encoder == &encoder_token && input != NULL && output != NULL);
    assert(input_size == 800 * 800 * 3 && capacity == input_size + 1024);
    assert(lease_held && indicator_active && unmaps == 0);
    ++encodes;
    clock_us += 2000;
    if (is("encode-failure")) return -1;
    *size = is("quality-retry") && encodes < 3 ? 740 * 1024 + 1 : 32;
    memset(output, 0, (size_t)*size);
    return JPEG_ERR_OK;
}
static void jpeg_enc_close(jpeg_enc_handle_t encoder) { assert(encoder == &encoder_token); }
static int mbedtls_base64_encode(unsigned char *output, size_t capacity, size_t *written,
                                const unsigned char *input, size_t length)
{
    assert(!lease_held && !indicator_active && input != NULL && length == 32 && capacity >= 4);
    memcpy(output, "AAAA", 4);
    *written = 4;
    return 0;
}
#include "camera_handler_under_test.inc"

static const pipeline_record_t *pipeline(const char *stage)
{
    for (unsigned i = 0; i < pipeline_count; ++i) {
        if (strcmp(pipeline_records[i].stage, stage) == 0) return &pipeline_records[i];
    }
    return NULL;
}
static void run_handler(void)
{
    char *output = NULL;
    esp_openclaw_node_error_t error = {0};
    esp_err_t result = camera_snap(NULL, NULL, "{}", 2, &output, &error);
    bool capture_failed = is("missing-frame") || is("warmup-starved") || is("timeout-setup");
    bool failed = capture_failed || is("transform-failure") || is("encode-failure");
    assert(result == (failed ? ESP_FAIL : ESP_OK));
    assert(!lease_held && !indicator_active && releases == 1 && indicator_ends == 1);
    assert(parameter_deletes == 1 && closes == 1 && streamoffs == 1 && maps == unmaps);
    assert(timeout_sets == 1);
    assert(pipeline("cleanup_begin") != NULL && pipeline("cleanup_end") != NULL);
    assert(pipeline("cleanup_end")->elapsed_ms == (is("timeout-setup") ? 4U : 8U));
    assert(pipeline("release_begin") != NULL && pipeline("release_end")->elapsed_ms == 5);
    if (capture_failed) {
        assert(transforms == 0 && encodes == 0 && output == NULL);
        assert(strcmp(error.code, "UNAVAILABLE") == 0);
        const record_t *failure = &records[record_count - 1];
        assert(strcmp(failure->domain, "errno") == 0);
        assert(failure->error == (is("timeout-setup") ? ENOTTY : ETIMEDOUT));
        assert(pipeline("capture_complete") == NULL && pipeline("transform_begin") == NULL);
        if (is("timeout-setup")) {
            assert(dequeues == 0 && gfmt_calls == 0 && maps == 0 && initial_queues == 0);
            assert(strcmp(failure->stage, "dqbuf_timeout_setup") == 0);
            assert(pipeline("dqbuf_first_return") == NULL);
        } else {
            assert(strcmp(failure->stage, "dqbuf") == 0);
            assert(dequeues == (is("warmup-starved") ? 2U : 1U));
            assert(requeues == (is("warmup-starved") ? 1U : 0U));
            assert(pipeline("dqbuf_first_return")->elapsed_ms == (is("warmup-starved") ? 1U : 2000U));
        }
    } else {
        assert(transforms == 1);
        assert(pipeline("capture_complete")->elapsed_ms == (is("warmup-max") ? 10000U : 1U));
        assert(dequeues == (is("warmup-max") ? 40U : 1U) && requeues + 1 == dequeues);
        assert(pipeline("transform_end")->elapsed_ms == 7);
        if (is("transform-failure")) {
            assert(encodes == 0 && pipeline("encode_begin") == NULL);
            assert(strcmp(error.code, "UNAVAILABLE") == 0 && output == NULL);
        } else {
            assert(encodes == (is("quality-retry") ? 3U : 1U));
            assert(pipeline("encode_end")->elapsed_ms == 2U * encodes);
            assert(pipeline_count == 10);
            if (is("encode-failure")) {
                assert(strcmp(error.code, "INTERNAL") == 0 && output == NULL);
            } else {
                assert(output != NULL && strstr(output, "\"width\":800,\"height\":800") != NULL);
            }
        }
    }
    free(output);
}

typedef struct {
    const char *scenario, *stage, *domain;
    unsigned mapped_count, marker_count;
    int result;
} failure_case_t;
static const failure_case_t failures[] = {
    {"bsp", "bsp_start", "esp", 0, 0, BSP_FAILURE},
    {"open", "open", "errno", 0, 0, ESP_FAIL},
    {"gfmt", "g_fmt_initial", "errno", 0, 0, ESP_FAIL},
    {"default-size", "validate_default", "validation", 0, 0, ESP_FAIL},
    {"sfmt", "s_fmt_rgb565", "errno", 0, 0, ESP_FAIL},
    {"gfmt-after", "g_fmt_rgb565", "errno", 0, 0, ESP_FAIL},
    {"negotiated-size", "validate_format", "validation", 0, 0, ESP_FAIL},
    {"negotiated-format", "validate_format", "validation", 0, 0, ESP_FAIL},
    {"negotiated-stride", "validate_format", "validation", 0, 0, ESP_FAIL},
    {"reqbufs", "reqbufs", "errno", 0, 0, ESP_FAIL},
    {"buffer-count", "validate_count", "validation", 0, 0, ESP_FAIL},
    {"query0", "querybuf", "errno", 0, 0, ESP_FAIL},
    {"query1", "querybuf", "errno", 1, 0, ESP_FAIL},
    {"buffer-length", "validate_buffer_length", "validation", 1, 0, ESP_FAIL},
    {"mmap0", "mmap", "errno", 0, 0, ESP_FAIL},
    {"mmap1", "mmap", "errno", 1, 0, ESP_FAIL},
    {"qbuf0", "qbuf", "errno", 1, 0, ESP_FAIL},
    {"qbuf1", "qbuf", "errno", 2, 0, ESP_FAIL},
    {"streamon", "streamon", "errno", 2, 0, ESP_FAIL},
    {"dqbuf", "dqbuf", "errno", 2, 1, ESP_FAIL},
    {"frame-index", "validate_index", "validation", 2, 1, ESP_FAIL},
    {"requeue", "requeue", "errno", 2, 1, ESP_FAIL},
    {"frame-length", "validate_frame_length", "validation", 2, 1, ESP_FAIL},
};

static void assert_marker(void)
{
    assert(strcmp(records[0].stage, "dqbuf_begin") == 0);
    assert(strcmp(records[0].domain, "none") == 0 && records[0].error == 0);
}

int main(int argc, char **argv)
{
    const struct rlimit no_core = {0, 0};
    assert(setrlimit(RLIMIT_CORE, &no_core) == 0);
    assert(argc == 2);
    scenario = argv[1];
    if (strncmp(scenario, "handler-", 8) == 0) {
        scenario += 8;
        run_handler();
        return 0;
    }
    errno = EDOM;
    camera_frame_t camera = {.fd = -1};
    int delay = is("delayed") ? 20 : is("requeue") ? 10 : 0;
    esp_err_t result = capture_rgb565_frame(delay, &camera);
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        const failure_case_t *test = &failures[i];
        if (!is(test->scenario)) continue;
        assert(result == test->result);
        bool detail = is("default-size") || is("negotiated-size") ||
            is("negotiated-format") || is("negotiated-stride") ||
            is("buffer-length") || is("frame-length");
        assert(record_count == test->marker_count + 1);
        assert(log_count == record_count + pipeline_count + (detail ? 1U : 0U));
        if (is("default-size")) assert(strstr(validation_detail, "640x720") != NULL);
        if (is("negotiated-size")) assert(strstr(validation_detail, "1280x480") != NULL);
        if (is("negotiated-format")) assert(strstr(validation_detail, "fourcc=0x00000000") != NULL);
        if (is("negotiated-stride")) assert(strstr(validation_detail, "stride=1 ") != NULL);
        if (is("buffer-length")) assert(strstr(validation_detail, "short: 1") != NULL);
        if (is("frame-length")) assert(strstr(validation_detail, "1 bytes") != NULL);
        if (test->marker_count) assert_marker();
        const record_t *failure = &records[test->marker_count];
        assert(strcmp(failure->stage, test->stage) == 0);
        assert(strcmp(failure->domain, test->domain) == 0);
        assert(failure->error == (strcmp(test->domain, "errno") == 0 ? EIO :
                                 strcmp(test->domain, "esp") == 0 ? BSP_FAILURE : 0));
        assert(maps == test->mapped_count && unmaps == maps);
        assert(closes == (is("bsp") || is("open") ? 0U : 1U));
        assert(streamoffs == closes && camera.fd == -1);
        return 0;
    }
    bool cache_case = is("bsp-retry") || is("open-cache") || is("success-cache");
    if (is("bsp-retry") || is("open-cache")) {
        assert(result == (is("bsp-retry") ? BSP_FAILURE : ESP_FAIL));
        assert(closes == 0 && maps == 0);
    } else {
        assert(result == ESP_OK && camera.fd == 7);
        assert(camera.width == CAMERA_SENSOR_WIDTH && camera.height == CAMERA_SENSOR_HEIGHT);
        assert(camera.stride == CAMERA_SENSOR_WIDTH * 2);
        assert(camera.format == (is("rgb565x") ? V4L2_PIX_FMT_RGB565X : V4L2_PIX_FMT_RGB565));
        assert(record_count == 1 && log_count == 1 + pipeline_count);
        assert_marker();
        assert(maps == 2 && unmaps == 0 && closes == 0 && streamoffs == 0);
        assert(dequeues == (is("delayed") ? 20U : 1U));
        assert(requeues + 1 == dequeues);
        close_camera_frame(&camera);
        assert(unmaps == 2 && closes == 1 && streamoffs == 1);
    }
    if (cache_case) {
        unsigned expected_starts = is("bsp-retry") ? 2 : 1;
        unsigned expected_timeout_sets = is("success-cache") ? 2 : 1;
        scenario = "success";
        record_count = log_count = gfmt_calls = maps = unmaps = closes = 0;
        streamoffs = dequeues = initial_queues = requeues = 0;
        pipeline_count = 0;
        assert(capture_rgb565_frame(0, &camera) == ESP_OK);
        assert(bsp_calls == expected_starts && record_count == 1);
        assert(timeout_sets == expected_timeout_sets);
        assert_marker();
        close_camera_frame(&camera);
        assert(unmaps == 2 && closes == 1);
    }
    return 0;
}
