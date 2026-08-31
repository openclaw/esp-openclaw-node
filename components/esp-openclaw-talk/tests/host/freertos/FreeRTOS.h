#pragma once

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
extern _Thread_local unsigned talk_host_critical_depth;
#define portENTER_CRITICAL(lock) do { \
    assert(pthread_mutex_lock(lock) == 0); \
    ++talk_host_critical_depth; \
} while (0)
#define portEXIT_CRITICAL(lock) do { \
    assert(talk_host_critical_depth > 0); \
    --talk_host_critical_depth; \
    assert(pthread_mutex_unlock(lock) == 0); \
} while (0)

typedef int BaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
