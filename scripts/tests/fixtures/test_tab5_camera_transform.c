#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_INVALID_STATE, ESP_ERR_NOT_SUPPORTED,
       ESP_ERR_INVALID_SIZE, ESP_ERR_NO_MEM, PPA_FAILURE };
enum { MALLOC_CAP_SPIRAM = 1, PPA_SRM_COLOR_MODE_RGB565,
       PPA_SRM_COLOR_MODE_RGB888, PPA_TRANS_MODE_BLOCKING };
typedef enum {
    PPA_SRM_ROTATION_ANGLE_0, PPA_SRM_ROTATION_ANGLE_90,
    PPA_SRM_ROTATION_ANGLE_180, PPA_SRM_ROTATION_ANGLE_270,
} ppa_srm_rotation_angle_t;
typedef struct {
    void *buffer;
    size_t buffer_size;
    uint32_t pic_w, pic_h, block_w, block_h;
    int srm_cm;
} picture_t;
typedef struct {
    picture_t in, out;
    ppa_srm_rotation_angle_t rotation_angle;
    float scale_x, scale_y;
    int mode;
} ppa_srm_oper_config_t;
typedef struct {
    uint32_t width, height;
    void *buffers[2];
    struct { unsigned index; } frame;
} camera_frame_t;

static int test_rotation;
#define BSP_CAMERA_ROTATION test_rotation
#define TAG "camera-transform-fixture"
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_RETURN_ON_FALSE(condition, error, tag, ...) do { \
    if (!(condition)) return (error); \
} while (0)
#define ESP_RETURN_ON_ERROR(call, tag, ...) do { \
    esp_err_t error = (call); \
    if (error != ESP_OK) return error; \
} while (0)

static void *camera_ppa_srm = (void *)&test_rotation;
static size_t camera_ppa_alignment = 64;
static void *allocation;
static size_t allocation_bytes;
static unsigned allocations, frees, ppa_calls;
static uint32_t written_width, written_height;
static bool fail_allocation, fail_ppa;

static void *heap_caps_aligned_calloc(size_t alignment, size_t count, size_t size, int caps)
{
    assert(allocation == NULL && count == 1 && caps == MALLOC_CAP_SPIRAM);
    assert(alignment == camera_ppa_alignment && size % alignment == 0);
    ++allocations;
    if (fail_allocation) return NULL;
    allocation = aligned_alloc(alignment, size);
    assert(allocation != NULL && (uintptr_t)allocation % 16 == 0);
    allocation_bytes = size;
    memset(allocation, 0, size);
    return allocation;
}

static void heap_caps_free(void *buffer)
{
    assert(buffer != NULL && buffer == allocation);
    ++frees;
    free(buffer);
    allocation = NULL;
}

static esp_err_t ppa_do_scale_rotate_mirror(void *client, const ppa_srm_oper_config_t *config)
{
    assert(client == camera_ppa_srm && config->mode == PPA_TRANS_MODE_BLOCKING);
    assert(config->in.srm_cm == PPA_SRM_COLOR_MODE_RGB565);
    assert(config->out.srm_cm == PPA_SRM_COLOR_MODE_RGB888);
    assert(config->out.buffer == allocation && config->out.buffer_size == allocation_bytes);
    assert(config->in.block_w == config->in.pic_w && config->in.block_h == config->in.pic_h);
    ++ppa_calls;
    if (fail_ppa) return PPA_FAILURE;

    /* Pinned PPA hardware truncates each scale to four fractional bits. */
    unsigned x_sixteenths = (unsigned)(config->scale_x * 16);
    unsigned y_sixteenths = (unsigned)(config->scale_y * 16);
    assert(x_sixteenths >= 1 && x_sixteenths <= 16);
    assert(y_sixteenths >= 1 && y_sixteenths <= 16);
    written_width = config->in.block_w * x_sixteenths / 16;
    written_height = config->in.block_h * y_sixteenths / 16;
    if (config->rotation_angle == PPA_SRM_ROTATION_ANGLE_90 ||
        config->rotation_angle == PPA_SRM_ROTATION_ANGLE_270) {
        uint32_t swap = written_width;
        written_width = written_height;
        written_height = swap;
    }
    assert(written_width <= config->out.pic_w && written_height <= config->out.pic_h);
    assert((size_t)config->out.pic_w * config->out.pic_h * 3 <= allocation_bytes);
    uint8_t *output = config->out.buffer;
    for (uint32_t y = 0; y < written_height; ++y) {
        memset(output + (size_t)y * config->out.pic_w * 3, 0xa5, written_width * 3);
    }
    return ESP_OK;
}

#include "camera_transform_under_test.inc"

typedef struct {
    int rotation, max_width;
    uint32_t input_width, input_height, output_width, output_height;
    esp_err_t result;
} geometry_case_t;

static void check_geometry(geometry_case_t test)
{
    allocations = frees = ppa_calls = 0;
    written_width = written_height = 0;
    allocation_bytes = 0;
    test_rotation = test.rotation;
    uint8_t input;
    camera_frame_t camera = {
        .width = test.input_width, .height = test.input_height,
        .buffers = {&input, NULL}, .frame.index = 0,
    };
    camera_transformed_frame_t transformed;
    memset(&transformed, 0x55, sizeof(transformed));
    esp_err_t result = transform_camera_frame(&camera, test.max_width, &transformed);
    assert(result == test.result);
    if (result != ESP_OK) {
        assert(transformed.data == NULL && transformed.data_size == 0);
        assert(transformed.width == 0 && transformed.height == 0 && allocation == NULL);
        if (fail_allocation) assert(allocations == 1 && ppa_calls == 0 && frees == 0);
        else if (fail_ppa) assert(allocations == 1 && ppa_calls == 1 && frees == 1);
        else assert(allocations == 0 && ppa_calls == 0 && frees == 0);
        return;
    }
    if (transformed.width != written_width || transformed.height != written_height) {
        fprintf(stderr, "packed image %ux%u differs from PPA-written extent %ux%u\n",
                transformed.width, transformed.height, written_width, written_height);
        abort();
    }
    assert(transformed.width == test.output_width && transformed.height == test.output_height);
    assert(transformed.width <= (uint32_t)test.max_width);
    assert(transformed.data_size == (size_t)test.output_width * test.output_height * 3);
    assert(transformed.data_size <= CAMERA_MAX_PIXELS * 3U);
    assert(allocations == 1 && ppa_calls == 1 && frees == 0);
    /* JPEG receives exactly packed RGB888, never an unwritten margin or allocation tail. */
    for (size_t i = 0; i < transformed.data_size; ++i) assert(transformed.data[i] == 0xa5);
    for (size_t i = transformed.data_size; i < allocation_bytes; ++i) assert(transformed.data[i] == 0);
    heap_caps_free(transformed.data);
    assert(frees == 1);
}

int main(int argc, char **argv)
{
    struct rlimit core_limit = {0, 0};
    assert(setrlimit(RLIMIT_CORE, &core_limit) == 0);
    assert(argc == 2);
    geometry_case_t requested = {90, 640, 1280, 720, 630, 1120, ESP_OK};
    if (strcmp(argv[1], "requested-640") == 0) {
        check_geometry(requested);
    } else if (strcmp(argv[1], "geometry") == 0) {
        const geometry_case_t cases[] = {
            {270, 640, 1280, 720, 630, 1120, ESP_OK},
            {90, 720, 1280, 720, 720, 1280, ESP_OK},
            {270, 2048, 1280, 720, 720, 1280, ESP_OK},
            {0, 1280, 1280, 720, 1280, 720, ESP_OK},
            {180, 640, 1280, 720, 640, 360, ESP_OK},
            {90, 64, 1280, 720, 45, 80, ESP_OK},
            {0, 80, 1280, 720, 80, 45, ESP_OK},
            {0, 64, 1280, 720, 0, 0, ESP_ERR_INVALID_SIZE},
            {90, 629, 1280, 720, 585, 1040, ESP_OK},
            {90, 640, 2048, 1024, 0, 0, ESP_ERR_NOT_SUPPORTED},
            {90, 640, 1280, 0, 0, 0, ESP_ERR_NOT_SUPPORTED},
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) check_geometry(cases[i]);
    } else if (strcmp(argv[1], "allocation-failure") == 0) {
        fail_allocation = true;
        requested.result = ESP_ERR_NO_MEM;
        check_geometry(requested);
    } else {
        assert(strcmp(argv[1], "ppa-failure") == 0);
        fail_ppa = true;
        requested.result = PPA_FAILURE;
        check_geometry(requested);
    }
    return 0;
}
