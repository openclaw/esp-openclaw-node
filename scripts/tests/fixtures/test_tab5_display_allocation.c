#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define BSP_LCD_H_RES 720
#define BSP_LCD_V_RES 1280
#define ESP_OK 0
#define ESP_LVGL_PORT_INIT_CONFIG() {0}
#define RETURN_NULL_ON_ERROR(expr, message) do { \
    if ((expr) != ESP_OK) return NULL; \
} while (0)

typedef int lv_display_t;
typedef struct {
    int task_affinity;
} lvgl_port_cfg_t;
typedef struct {
    void *io_handle;
    void *panel_handle;
    size_t buffer_size;
    bool double_buffer;
    int hres;
    int vres;
    bool monochrome;
    struct { bool swap_xy, mirror_x, mirror_y; } rotation;
    struct { bool buff_dma, buff_spiram, sw_rotate; } flags;
} lvgl_port_display_cfg_t;
typedef struct {
    struct { bool avoid_tearing; } flags;
} lvgl_port_display_dsi_cfg_t;

static bool encrypted;
static unsigned encryption_queries;
static unsigned port_starts;
static unsigned display_adds;
static int io_marker;
static int panel_marker;
static lv_display_t display_marker;
static lvgl_port_display_cfg_t observed;

bool esp_flash_encryption_enabled(void)
{
    encryption_queries++;
    return encrypted;
}

static int lvgl_port_init(const lvgl_port_cfg_t *config)
{
    assert(config->task_affinity == 1);
    port_starts++;
    return ESP_OK;
}

static lv_display_t *lvgl_port_add_disp_dsi(
    const lvgl_port_display_cfg_t *config,
    const lvgl_port_display_dsi_cfg_t *dsi)
{
    assert(port_starts == 1);
    assert(!dsi->flags.avoid_tearing);
    observed = *config;
    display_adds++;
    return &display_marker;
}

static lv_display_t *configure_display(void)
{
    void *io = &io_marker;
    void *panel = &panel_marker;
#include "display_config_under_test.inc"
    return display;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    assert(strcmp(argv[1], "encrypted") == 0 || strcmp(argv[1], "unencrypted") == 0);
    encrypted = strcmp(argv[1], "encrypted") == 0;
    assert(configure_display() == &display_marker);
    /* Pinned PPA SRM rejects external buffers when flash encryption is active. */
    assert(observed.flags.buff_dma == encrypted);
    assert(observed.flags.buff_spiram == !encrypted);
    assert(encryption_queries == 1);
    assert(display_adds == 1);
    assert(observed.io_handle == &io_marker && observed.panel_handle == &panel_marker);
    assert(observed.buffer_size == 720 * 40 && observed.double_buffer);
    assert(observed.hres == 720 && observed.vres == 1280 && !observed.monochrome);
    assert(observed.flags.sw_rotate);
    assert(!observed.rotation.swap_xy && !observed.rotation.mirror_x && !observed.rotation.mirror_y);
    return 0;
}
