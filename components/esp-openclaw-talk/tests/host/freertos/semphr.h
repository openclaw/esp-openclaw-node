#pragma once
#include "FreeRTOS.h"
#include <stdlib.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool available;
    unsigned waiters;
} *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    SemaphoreHandle_t sem = calloc(1, sizeof(*sem));
    if (sem != NULL) {
        assert(pthread_mutex_init(&sem->mutex, NULL) == 0);
        assert(pthread_cond_init(&sem->changed, NULL) == 0);
    }
    return sem;
}
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    assert(pthread_mutex_lock(&sem->mutex) == 0);
    sem->available = true;
    assert(pthread_cond_signal(&sem->changed) == 0);
    assert(pthread_mutex_unlock(&sem->mutex) == 0);
    return pdTRUE;
}
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t wait)
{
    assert(pthread_mutex_lock(&sem->mutex) == 0);
    while (!sem->available && wait == portMAX_DELAY) {
        ++sem->waiters;
        assert(pthread_cond_broadcast(&sem->changed) == 0);
        assert(pthread_cond_wait(&sem->changed, &sem->mutex) == 0);
        --sem->waiters;
    }
    bool available = sem->available;
    sem->available = false;
    assert(pthread_mutex_unlock(&sem->mutex) == 0);
    return available ? pdTRUE : pdFALSE;
}
static inline void vSemaphoreDelete(SemaphoreHandle_t sem)
{
    assert(pthread_cond_destroy(&sem->changed) == 0);
    assert(pthread_mutex_destroy(&sem->mutex) == 0);
    free(sem);
}
