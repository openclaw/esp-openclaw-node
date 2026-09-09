#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_SPIRAM 2
#define MALLOC_CAP_8BIT 4
#define ESP_BLOCK_SIZE 512
#define H_SDIO_RX_BLOCK_ONLY_XFER 1
#define ESP_SLAVE_LEN_MASK 0x3ffff
#define PACKET_LEN_INDEX 0
#define DRAM_ATTR
#define DRAM_STR(s) (s)
#define ESP_LOGD(tag, ...) ((void)(tag))

typedef int esp_err_t;
typedef struct { unsigned extra_heap_caps; size_t dma_alignment_bytes; } esp_dma_mem_info_t;
typedef struct { size_t total_free_bytes, largest_free_block; } multi_heap_info_t;
static const char *TAG = "fixture";
static uint8_t reg_buf[4];
static uint32_t sdio_rx_byte_count = 226009;
static struct {
    struct { uint8_t *buf; uint32_t buf_size; } buffer[2];
    int write_index;
} double_buf;
static unsigned calls, frees, last_caps;
static size_t last_size, last_alignment;
static bool fail_psram;

static void release(void *buffer) { ++frees; free(buffer); }
static struct { void (*_h_free)(void *); } functions = { release };
static struct { __typeof__(functions) *funcs; } g_h = { &functions };

static esp_err_t esp_dma_capable_malloc(size_t size, const esp_dma_mem_info_t *info,
                                       void **out, size_t *actual)
{
    ++calls;
    last_caps = info->extra_heap_caps ? info->extra_heap_caps : MALLOC_CAP_DMA;
    last_size = size;
    last_alignment = info->dma_alignment_bytes;
    printf("alloc caps=%u size=%zu alignment=%zu\n", last_caps, size, last_alignment);
    fflush(stdout);
    if ((last_caps == MALLOC_CAP_DMA && size > 3072) ||
            ((last_caps & MALLOC_CAP_SPIRAM) && fail_psram))
        return ESP_ERR_NO_MEM;
    assert(last_alignment == 64);
    size_t rounded = (size + 63) & ~(size_t)63;
    assert(posix_memalign(out, last_alignment, rounded) == 0);
    if (actual) *actual = rounded;
    return ESP_OK;
}

static void heap_caps_get_info(multi_heap_info_t *info, unsigned caps)
{
    assert(caps == MALLOC_CAP_DMA);
    info->total_free_bytes = 10915;
    info->largest_free_block = 3072;
}

static int esp_rom_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int count = vprintf(format, args);
    va_end(args);
    fflush(stdout);
    return count;
}

#include "sdio_rx_under_test.inc"

int main(int argc, char **argv)
{
    assert(argc == 2);
    struct rlimit core_limit = {0, 0};
    assert(setrlimit(RLIMIT_CORE, &core_limit) == 0);
    uint32_t reg = 875792993;
    memcpy(reg_buf, &reg, sizeof(reg));
    static uint8_t reader[16];
    double_buf.buffer[0].buf = reader;
    double_buf.buffer[0].buf_size = sizeof(reader);
    double_buf.write_index = 1;

    if (strcmp(argv[1], "legacy") == 0) {
        assert(sdio_rx_get_buffer(1024));
        assert(last_caps == MALLOC_CAP_DMA && last_size == 1024);
        assert(sdio_rx_get_buffer(2049));
        assert(last_caps == MALLOC_CAP_DMA && last_size == 2560);
    } else if (strcmp(argv[1], "alignment") == 0) {
        const uint32_t lengths[] = {1, 512, 513, 6024, 6145};
        for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
            uint8_t *buffer = sdio_rx_get_buffer(lengths[i]);
            assert(buffer && (uintptr_t)buffer % 64 == 0);
            assert(double_buf.buffer[1].buf_size >= ((lengths[i] + 511) / 512) * 512);
        }
        assert(last_caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    } else {
        assert(posix_memalign((void **)&double_buf.buffer[1].buf, 64, 3072) == 0);
        double_buf.buffer[1].buf_size = 3072;
        fail_psram = strcmp(argv[1], "failure") == 0;
        uint8_t *buffer = sdio_rx_get_buffer(6024);
        assert(buffer && calls == 1 && frees == 1);
        assert(last_size == 6144 && last_alignment == 64);
        assert(double_buf.buffer[1].buf_size == 6144);
        assert(last_caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        assert(sdio_rx_get_buffer(6024) == buffer && calls == 1 && frees == 1);
        assert(sdio_rx_get_buffer(6145) && calls == 2 && frees == 2);
        assert(last_size == 6656 && double_buf.buffer[1].buf_size == 6656);
    }
    assert(double_buf.buffer[0].buf == reader);
    assert(double_buf.buffer[0].buf_size == sizeof(reader));
    free(double_buf.buffer[1].buf);
    return 0;
}
