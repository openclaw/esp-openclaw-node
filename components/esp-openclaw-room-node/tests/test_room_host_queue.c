/* Exercise the fixture's actual byte arithmetic without allocating gigabytes. */
#include <limits.h>
#include "host/room_host_fakes.c"

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "unsupported") == 0) {
        room_diagnostics_request_open();
        return 1;
    }
    if (argc == 2 && strcmp(argv[1], "out-of-bounds") == 0) {
        struct host_queue invalid = {.capacity = 2, .size = 3, .storage_size = 3};
        (void)queue_item_bytes(&invalid, 2);
        return 1;
    }

    size_t bytes = 0;
    host_require(!queue_storage_size(0, 1, &bytes), "reject zero capacity");
    host_require(!queue_storage_size(1, 0, &bytes), "reject zero item size");
    host_require(!queue_storage_size(SIZE_MAX / 2 + 1, 2, &bytes), "reject size_t overflow");
    host_require(queue_storage_size(SIZE_MAX / 2, 2, &bytes) && bytes == SIZE_MAX - 1,
        "accept largest even storage size");
    struct host_queue dimensions = {.capacity = SIZE_MAX / 2, .size = 2, .storage_size = bytes};
    host_require(queue_item_bytes(&dimensions, dimensions.capacity) == SIZE_MAX - 1,
        "byte arithmetic preserves full size_t range");
#if SIZE_MAX > UINT_MAX
    host_require(queue_storage_size(UINT_MAX, 2, &bytes) && bytes == (size_t)UINT_MAX + UINT_MAX,
        "allocation exceeds unsigned range without wrapping");
    dimensions = (struct host_queue){.capacity = UINT_MAX, .size = 2, .storage_size = bytes};
    host_require(queue_item_bytes(&dimensions, UINT_MAX) == bytes,
        "send offset and receive length exceed unsigned range without wrapping");
#endif

    const unsigned char items[][3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
    unsigned char out[3];
    QueueHandle_t value = xQueueCreate(3, sizeof(items[0]));
    for (size_t i = 0; i < 3; ++i)
        host_require(xQueueSend(value, items[i], 0) == pdTRUE, "enqueue distinct items");
    host_require(xQueueSend(value, items[0], 0) == pdFALSE, "full queue rejects send");
    for (size_t i = 0; i < 3; ++i) {
        host_require(xQueueReceive(value, out, 0) == pdTRUE && memcmp(out, items[i], sizeof(out)) == 0,
            "FIFO compaction preserves every item");
    }
    host_require(xQueueSend(value, items[2], 0) == pdTRUE &&
        xQueueReceive(value, out, 0) == pdTRUE && memcmp(out, items[2], sizeof(out)) == 0,
        "drained queue can be reused");
    host_release_resources();
    puts("room host queue arithmetic and FIFO passed");
    return 0;
}
