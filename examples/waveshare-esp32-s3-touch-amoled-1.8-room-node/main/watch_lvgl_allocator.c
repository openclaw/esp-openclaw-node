#include "lvgl.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM
#include "esp_heap_caps.h"

/* Small widget/style allocations also belong in PSRAM. Ordinary malloc keeps
 * these in the internal pool needed by the Watch's radio and task stacks. */
#define DISPLAY_MEMORY (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *memory, size_t bytes)
{
    (void)memory;
    (void)bytes;
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    (void)pool;
}

void *lv_malloc_core(size_t size)
{
    return heap_caps_malloc(size, DISPLAY_MEMORY);
}

void *lv_realloc_core(void *memory, size_t size)
{
    return heap_caps_realloc(memory, size, DISPLAY_MEMORY);
}

void lv_free_core(void *memory)
{
    heap_caps_free(memory);
}

void lv_mem_monitor_core(lv_mem_monitor_t *monitor)
{
    /* Like the clib backend, this allocator has no private LVGL pool. */
    (void)monitor;
}

lv_result_t lv_mem_test_core(void)
{
    /* Match LVGL's clib backend. A full PSRAM integrity scan can exceed the
     * interrupt watchdog; explicit heap diagnostics own that operation. */
    return LV_RESULT_OK;
}
#endif
