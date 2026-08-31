#pragma once

#include <stddef.h>

void talk_host_register_test(void (*test)(void), const char *name, int line);
extern _Thread_local unsigned talk_host_critical_depth;
extern unsigned talk_host_error_count;

#define TALK_HOST_JOIN_INNER(a, b) a##b
#define TALK_HOST_JOIN(a, b) TALK_HOST_JOIN_INNER(a, b)
#define TEST_CASE(name, tags) \
    static void TALK_HOST_JOIN(talk_case_, __LINE__)(void); \
    static void __attribute__((constructor)) TALK_HOST_JOIN(talk_register_, __LINE__)(void) \
    { \
        talk_host_register_test(TALK_HOST_JOIN(talk_case_, __LINE__), name, __LINE__); \
    } \
    static void TALK_HOST_JOIN(talk_case_, __LINE__)(void)
