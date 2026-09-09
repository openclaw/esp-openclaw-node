#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#define ESP_OK 0
#define ESP_FAIL -1
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
       VIDIOC_QBUF, VIDIOC_STREAMON, VIDIOC_DQBUF, VIDIOC_STREAMOFF };
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
    } else {
        memcpy(validation_detail, line, (size_t)length + 1);
    }
    errno = EBUSY;
}
#define ESP_LOGE(tag, ...) do { (void)(tag); capture_log(__VA_ARGS__); } while (0)
#define ESP_LOGI(tag, ...) do { (void)(tag); capture_log(__VA_ARGS__); } while (0)
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
    errno = EIO;
    return is("open") || is("open-cache") ? -1 : 7;
}
static int camera_close(int fd)
{
    assert(fd == 7);
    ++closes;
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
    errno = EBADF;
    return -1;
}
static int camera_ioctl(int fd, int request, void *argument)
{
    assert(fd == 7);
    errno = ESTALE;
    if (request == VIDIOC_STREAMOFF) {
        assert(*(int *)argument == V4L2_BUF_TYPE_VIDEO_CAPTURE);
        ++streamoffs;
        errno = EBADF;
        return -1;
    }
    if (request == VIDIOC_G_FMT) {
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
    int64_t current = clock_us;
    clock_us += 1000;
    return current;
}
#define open camera_open
#define close camera_close
#define mmap camera_mmap
#define munmap camera_munmap
#define ioctl camera_ioctl
#include "camera_capture_under_test.inc"

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
        assert(log_count == record_count + (detail ? 1U : 0U));
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
        assert(record_count == 1 && log_count == 1);
        assert_marker();
        assert(maps == 2 && unmaps == 0 && closes == 0 && streamoffs == 0);
        assert(dequeues == (is("delayed") ? 20U : 1U));
        assert(requeues + 1 == dequeues);
        close_camera_frame(&camera);
        assert(unmaps == 2 && closes == 1 && streamoffs == 1);
    }
    if (cache_case) {
        unsigned expected_starts = is("bsp-retry") ? 2 : 1;
        scenario = "success";
        record_count = log_count = gfmt_calls = maps = unmaps = closes = 0;
        streamoffs = dequeues = initial_queues = requeues = 0;
        assert(capture_rgb565_frame(0, &camera) == ESP_OK);
        assert(bsp_calls == expected_starts && record_count == 1);
        assert_marker();
        close_camera_frame(&camera);
        assert(unmaps == 2 && closes == 1);
    }
    return 0;
}
