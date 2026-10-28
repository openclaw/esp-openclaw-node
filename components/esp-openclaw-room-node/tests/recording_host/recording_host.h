#pragma once

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define taskENTER_CRITICAL(m) assert(pthread_mutex_lock(m) == 0)
#define taskEXIT_CRITICAL(m) assert(pthread_mutex_unlock(m) == 0)
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(n) (n)
#define portMAX_DELAY UINT32_MAX
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
typedef struct test_queue *QueueHandle_t;
typedef struct test_gate *SemaphoreHandle_t;
typedef void *TaskHandle_t;
QueueHandle_t xQueueCreate(UBaseType_t count, UBaseType_t size);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait);
void vQueueDelete(QueueHandle_t queue);
BaseType_t xSemaphoreTake(SemaphoreHandle_t gate, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t gate);
BaseType_t xTaskCreateWithCaps(void (*fn)(void *), const char *name, uint32_t stack,
    void *arg, UBaseType_t priority, TaskHandle_t *task, UBaseType_t caps);
void vTaskDeleteWithCaps(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
int64_t esp_timer_get_time(void);
void *heap_caps_malloc(size_t bytes, unsigned caps);
void recording_host_free(void *pointer);
