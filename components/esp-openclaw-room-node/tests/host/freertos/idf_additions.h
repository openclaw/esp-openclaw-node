#pragma once
#include "FreeRTOS.h"
typedef struct host_task *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
    void *arg, UBaseType_t priority, TaskHandle_t *out);
BaseType_t xTaskCreateWithCaps(TaskFunction_t fn, const char *name, uint32_t stack,
    void *arg, UBaseType_t priority, TaskHandle_t *out, UBaseType_t caps);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
void vTaskDelay(TickType_t wait);
void vTaskDelete(TaskHandle_t task);
