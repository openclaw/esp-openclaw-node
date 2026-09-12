#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Host-only type/log boundaries for the complete, hash-checked upstream mapper. */
typedef int esp_err_t;
typedef int cam_ctlr_color_t;
typedef int isp_color_t;
typedef int esp_cam_sensor_output_format_t;
enum { ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_NOT_SUPPORTED };
enum {
    CAM_CTLR_COLOR_RAW8, CAM_CTLR_COLOR_RAW10, CAM_CTLR_COLOR_RAW12,
    CAM_CTLR_COLOR_RGB565, CAM_CTLR_COLOR_RGB888, CAM_CTLR_COLOR_YUV420,
    CAM_CTLR_COLOR_YUV422, CAM_CTLR_COLOR_YUV422_UYVY, CAM_CTLR_COLOR_GRAY8
};
enum {
    ISP_COLOR_RAW8, ISP_COLOR_RAW10, ISP_COLOR_RAW12, ISP_COLOR_RGB565,
    ISP_COLOR_RGB888, ISP_COLOR_YUV420, ISP_COLOR_YUV422
};
enum {
    ESP_CAM_SENSOR_PIXFORMAT_RGB565_LE, ESP_CAM_SENSOR_PIXFORMAT_RGB565_BE,
    ESP_CAM_SENSOR_PIXFORMAT_YUV422_UYVY, ESP_CAM_SENSOR_PIXFORMAT_YUV422_YUYV,
    ESP_CAM_SENSOR_PIXFORMAT_YUV420, ESP_CAM_SENSOR_PIXFORMAT_RGB888,
    ESP_CAM_SENSOR_PIXFORMAT_RGB444, ESP_CAM_SENSOR_PIXFORMAT_RGB555,
    ESP_CAM_SENSOR_PIXFORMAT_BGR888, ESP_CAM_SENSOR_PIXFORMAT_RAW8,
    ESP_CAM_SENSOR_PIXFORMAT_RAW10, ESP_CAM_SENSOR_PIXFORMAT_RAW12,
    ESP_CAM_SENSOR_PIXFORMAT_GRAYSCALE
};
enum {
    V4L2_PIX_FMT_SBGGR8 = 100, V4L2_PIX_FMT_SBGGR10, V4L2_PIX_FMT_SBGGR12,
    V4L2_PIX_FMT_RGB565, V4L2_PIX_FMT_RGB565X, V4L2_PIX_FMT_RGB24,
    V4L2_PIX_FMT_YUV420, V4L2_PIX_FMT_UYVY, V4L2_PIX_FMT_VYUY,
    V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_YVYU, V4L2_PIX_FMT_GREY
};
typedef struct {
    bool isp_bypass_required;
    isp_color_t isp_input_fmt;
    isp_color_t isp_output_fmt;
    uint8_t isp_bpp;
    cam_ctlr_color_t csi_input_fmt;
    cam_ctlr_color_t csi_output_fmt;
} esp_video_csi_isp_in_out_format_t;

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define ESP_IDF_VERSION_VAL(major, minor, patch) (((major) << 16) | ((minor) << 8) | (patch))
#define ESP_VIDEO_CSI_DEVICE_CONV_FORMAT 0
#define CONFIG_ESP_VIDEO_ENABLE_ISP_VIDEO_DEVICE 1
#define ESP_LOGD(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_RETURN_ON_FALSE(condition, result, ...) do { if (!(condition)) return (result); } while (0)

static bool esp_video_isp_video_device_is_raw_bypass(void)
{
    return true;
}

#include "camera_mapper_under_test.inc"

#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "%s\n", message); return 1; } \
} while (0)

static int processing(void)
{
    const struct {
        uint32_t output;
        cam_ctlr_color_t csi;
        isp_color_t isp;
        uint8_t bpp;
    } cases[] = {
        {V4L2_PIX_FMT_RGB565, CAM_CTLR_COLOR_RGB565, ISP_COLOR_RGB565, 16},
        {V4L2_PIX_FMT_RGB24, CAM_CTLR_COLOR_RGB888, ISP_COLOR_RGB888, 24},
        {V4L2_PIX_FMT_YUV420, CAM_CTLR_COLOR_YUV420, ISP_COLOR_YUV420, 12},
    };
    for (size_t i = 0; i < ARRAY_SIZE(cases); ++i) {
        for (int raw10 = 0; raw10 <= 1; ++raw10) {
            esp_video_csi_isp_in_out_format_t format = {0};
            int sensor = raw10 ? ESP_CAM_SENSOR_PIXFORMAT_RAW10 : ESP_CAM_SENSOR_PIXFORMAT_RAW8;
            CHECK(esp_video_csi_check_format(sensor, cases[i].output, &format) == ESP_OK,
                  "supported processing format rejected");
            CHECK(!format.isp_bypass_required, "ISP processing was bypassed");
            CHECK(format.isp_input_fmt == (raw10 ? ISP_COLOR_RAW10 : ISP_COLOR_RAW8),
                  "ISP sensor input changed");
            CHECK(format.isp_output_fmt == cases[i].isp, "ISP output changed");
            CHECK(isp_color_to_bpp(format.isp_output_fmt) == cases[i].bpp, "output bit depth changed");
            CHECK(format.csi_output_fmt == cases[i].csi, "CSI output changed");
            CHECK(format.csi_input_fmt == cases[i].csi, "post-ISP CSI input mismatch");
            CHECK(format.csi_input_fmt == format.csi_output_fmt, "early-P4 CSI conversion requested");
        }
    }
    return 0;
}

static int bypass(void)
{
    const int sensors[] = {ESP_CAM_SENSOR_PIXFORMAT_RAW8, ESP_CAM_SENSOR_PIXFORMAT_RAW10};
    const uint32_t pixels[] = {V4L2_PIX_FMT_SBGGR8, V4L2_PIX_FMT_SBGGR10};
    const int colors[] = {CAM_CTLR_COLOR_RAW8, CAM_CTLR_COLOR_RAW10};
    for (size_t i = 0; i < ARRAY_SIZE(sensors); ++i) {
        for (int use_default = 0; use_default <= 1; ++use_default) {
            esp_video_csi_isp_in_out_format_t format = {0};
            CHECK(esp_video_csi_check_format(sensors[i], use_default ? 0 : pixels[i], &format) == ESP_OK,
                  "raw bypass rejected");
            CHECK(format.isp_bypass_required, "raw bypass changed");
            CHECK(format.csi_input_fmt == colors[i] && format.csi_output_fmt == colors[i],
                  "raw CSI passthrough changed");
            CHECK(format.isp_input_fmt == format.isp_output_fmt, "raw ISP passthrough changed");
            CHECK(format.isp_bpp == (i ? 10 : 8), "raw bypass bit depth changed");
        }
    }
    return 0;
}

static int unsupported(void)
{
    esp_video_csi_isp_in_out_format_t format = {0};
    CHECK(esp_video_csi_check_format(ESP_CAM_SENSOR_PIXFORMAT_RAW8, UINT32_MAX, &format)
          == ESP_ERR_NOT_SUPPORTED, "unknown output accepted");
    CHECK(esp_video_csi_check_format(ESP_CAM_SENSOR_PIXFORMAT_RAW12, V4L2_PIX_FMT_RGB565, &format)
          == ESP_ERR_NOT_SUPPORTED, "unsupported raw12 conversion accepted");
    CHECK(esp_video_csi_check_format(ESP_CAM_SENSOR_PIXFORMAT_RAW8, V4L2_PIX_FMT_YUYV, &format)
          == ESP_ERR_NOT_SUPPORTED, "unsupported old-chip output accepted");
    CHECK(esp_video_csi_check_format(-1, V4L2_PIX_FMT_RGB565, &format)
          == ESP_ERR_NOT_SUPPORTED, "unknown sensor accepted");
    CHECK(esp_video_csi_check_format(ESP_CAM_SENSOR_PIXFORMAT_RAW8, V4L2_PIX_FMT_RGB565, NULL)
          == ESP_ERR_INVALID_ARG, "null output accepted");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    if (strcmp(argv[1], "processing") == 0) return processing();
    if (strcmp(argv[1], "bypass") == 0) return bypass();
    if (strcmp(argv[1], "unsupported") == 0) return unsupported();
    return 2;
}
