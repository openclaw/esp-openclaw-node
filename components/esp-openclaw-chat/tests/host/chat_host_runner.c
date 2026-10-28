#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "chat_host_runner.h"

static struct { void (*run)(void); const char *name; int line; } tests[64];
static size_t count;
void chat_host_register_test(void (*test)(void), const char *name, int line)
{
    if (count == sizeof(tests) / sizeof(tests[0])) abort();
    tests[count].run = test;
    tests[count].name = name;
    tests[count++].line = line;
}
void setUp(void) {}
void tearDown(void) {}
int main(int argc, char **argv)
{
    UnityBegin("test_esp_openclaw_chat.c");
    size_t selected = 0;
    for (size_t i = 0; i < count; ++i) {
        if (argc > 1 && strstr(tests[i].name, argv[1]) == NULL) continue;
        ++selected;
        UnityDefaultTestRun(tests[i].run, tests[i].name, tests[i].line);
    }
    if (selected == 0) { fprintf(stderr, "No chat tests matched\n"); return 2; }
    return UnityEnd();
}
