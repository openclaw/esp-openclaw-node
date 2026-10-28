#pragma once

void chat_host_register_test(void (*test)(void), const char *name, int line);
#define CHAT_JOIN_INNER(a, b) a##b
#define CHAT_JOIN(a, b) CHAT_JOIN_INNER(a, b)
#define TEST_CASE(name, tags) \
    static void CHAT_JOIN(chat_case_, __LINE__)(void); \
    static void __attribute__((constructor)) CHAT_JOIN(chat_register_, __LINE__)(void) \
    { chat_host_register_test(CHAT_JOIN(chat_case_, __LINE__), name, __LINE__); } \
    static void CHAT_JOIN(chat_case_, __LINE__)(void)
