#pragma once

#include <stddef.h>
#include <stdlib.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_LOG_DEBUG 4
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGD(tag, ...) ((void)(tag))
#define ESP_LOG_BUFFER_HEX_LEVEL(tag, data, length, level) ((void)(tag))

void esp_fill_random(void *buffer, size_t length);
void *srp_test_malloc(size_t size);
void *srp_test_calloc(size_t count, size_t size);
void srp_test_free(void *pointer);
