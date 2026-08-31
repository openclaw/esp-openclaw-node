#pragma once
#include "FreeRTOS.h"
typedef struct host_timer *TimerHandle_t;
TimerHandle_t xTimerCreate(const char *name, TickType_t period, BaseType_t reload,
    void *id, void (*callback)(TimerHandle_t));
BaseType_t xTimerStart(TimerHandle_t timer, TickType_t wait);
BaseType_t xTimerStop(TimerHandle_t timer, TickType_t wait);
BaseType_t xTimerDelete(TimerHandle_t timer, TickType_t wait);
void *pvTimerGetTimerID(TimerHandle_t timer);
