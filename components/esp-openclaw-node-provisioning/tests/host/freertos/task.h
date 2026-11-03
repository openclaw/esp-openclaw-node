#pragma once
#include "queue.h"
typedef void (*TaskFunction_t)(void *);
typedef void *TaskHandle_t;
BaseType_t xTaskCreate(TaskFunction_t task, const char *name, uint32_t stack_size,
    void *context, UBaseType_t priority, TaskHandle_t *handle);
