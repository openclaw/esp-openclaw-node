#pragma once
#include "freertos/FreeRTOS.h"
typedef void *QueueHandle_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t size);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout);
void vQueueDelete(QueueHandle_t queue);
