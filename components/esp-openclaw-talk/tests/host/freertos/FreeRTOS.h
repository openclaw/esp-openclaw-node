#pragma once

#include <assert.h>

typedef unsigned portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0U
extern unsigned talk_host_critical_depth;

/* Callbacks are scheduled explicitly by the tests, not by host threads. Track
 * critical sections so the fake transport can reject submission while locked. */
#define portENTER_CRITICAL(lock) do { \
    ++*(lock); \
    ++talk_host_critical_depth; \
} while (0)
#define portEXIT_CRITICAL(lock) do { \
    assert(*(lock) > 0 && talk_host_critical_depth > 0); \
    --*(lock); \
    --talk_host_critical_depth; \
} while (0)
