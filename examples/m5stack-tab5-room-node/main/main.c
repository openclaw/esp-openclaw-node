#include <inttypes.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_openclaw_room_node.h"
#include "esp_openclaw_node_transport_diag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tab5_room_board.h"

enum {
    ALLOCATION_DISARMED,
    ALLOCATION_ARMED,
    ALLOCATION_WRITING,
    ALLOCATION_CAPTURED,
};

typedef struct {
    _Atomic(TaskHandle_t) task;
    atomic_uint state;
    size_t requested;
    uint32_t caps;
    const char *function;
} allocation_slot_t;

_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2,
               "Allocation capture requires lock-free atomics");

static DRAM_ATTR allocation_slot_t s_allocation_slots[2];
static const char s_allocator_names[][32] = {
    "heap_caps_malloc",
    "heap_caps_malloc_default",
    "heap_caps_calloc",
    "heap_caps_realloc",
    "heap_caps_realloc_default",
    "heap_caps_malloc_prefer",
    "heap_caps_calloc_prefer",
    "heap_caps_realloc_prefer",
};

static int allocation_role(const char *role)
{
    if (role != NULL && strcmp(role, "node") == 0) {
        return 0;
    }
    if (role != NULL && strcmp(role, "operator") == 0) {
        return 1;
    }
    return -1;
}

static void IRAM_ATTR allocation_failed(size_t requested, uint32_t caps, const char *function)
{
    if (xPortInIsrContext()) {
        return;
    }
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    for (unsigned i = 0; i < 2; ++i) {
        allocation_slot_t *slot = &s_allocation_slots[i];
        if (atomic_load_explicit(&slot->task, memory_order_acquire) != task) {
            continue;
        }
        unsigned expected = ALLOCATION_ARMED;
        if (!atomic_compare_exchange_strong_explicit(
                &slot->state, &expected, ALLOCATION_WRITING,
                memory_order_acquire, memory_order_relaxed)) {
            continue;
        }
        slot->requested = requested;
        slot->caps = caps;
        slot->function = function;
        /* The matching task consumes only a completely published first failure. */
        atomic_store_explicit(&slot->state, ALLOCATION_CAPTURED, memory_order_release);
    }
}

void esp_openclaw_node_transport_start_begin(const char *role)
{
    int index = allocation_role(role);
    if (index < 0) {
        return;
    }
    allocation_slot_t *slot = &s_allocation_slots[index];
    TaskHandle_t expected = NULL;
    if (!atomic_compare_exchange_strong_explicit(
            &slot->task, &expected, xTaskGetCurrentTaskHandle(),
            memory_order_acq_rel, memory_order_relaxed)) {
        return;
    }
    atomic_store_explicit(&slot->state, ALLOCATION_ARMED, memory_order_release);
}

void esp_openclaw_node_transport_start_end(const char *role, esp_err_t result)
{
    int index = allocation_role(role);
    if (index < 0) {
        return;
    }
    allocation_slot_t *slot = &s_allocation_slots[index];
    if (atomic_load_explicit(&slot->task, memory_order_acquire) != xTaskGetCurrentTaskHandle()) {
        return;
    }
    unsigned state = atomic_exchange_explicit(&slot->state, ALLOCATION_DISARMED, memory_order_acq_rel);
    bool captured = state == ALLOCATION_CAPTURED;
    size_t requested = captured ? slot->requested : 0;
    uint32_t caps = captured ? slot->caps : 0;
    const char *function = captured ? slot->function : NULL;
    atomic_store_explicit(&slot->task, NULL, memory_order_release);

    if (!captured && result == ESP_OK) {
        return;
    }
    unsigned allocator = 0;
    /* Pinned SDK callers pass static __func__ strings; never dereference in the callback. */
    if (function != NULL) {
        for (unsigned i = 0; i < sizeof(s_allocator_names) / sizeof(s_allocator_names[0]); ++i) {
            if (strncmp(function, s_allocator_names[i], sizeof(s_allocator_names[0])) == 0) {
                allocator = i + 1;
                break;
            }
        }
    }
    /* After SDK return: it may already have freed resources since the failure. */
    size_t free_bytes = captured ? heap_caps_get_free_size(caps) : 0;
    size_t largest = captured ? heap_caps_get_largest_free_block(caps) : 0;
    ESP_LOGI(
        "tab5_alloc",
        "alloc_diag phase=after_sdk_return_before_owner_cleanup role=%u sdk_err=%d "
        "captured=%u requested=%zu caps=%" PRIu32 " allocator=%u origin=0 "
        "heap_sampled=%u free=%zu largest=%zu",
        (unsigned)index + 1, result, (unsigned)captured, requested, caps, allocator,
        (unsigned)captured, free_bytes, largest);
}

void app_main(void)
{
    esp_err_t diagnostic_err = heap_caps_register_failed_alloc_callback(allocation_failed);
    if (diagnostic_err != ESP_OK) {
        ESP_LOGE("tab5_alloc", "alloc_diag_install err=%d", diagnostic_err);
    }
    esp_openclaw_room_node_config_t config = {0};
    ESP_ERROR_CHECK(tab5_room_board_config(&config));
    ESP_ERROR_CHECK(esp_openclaw_room_node_start(&config));
}
