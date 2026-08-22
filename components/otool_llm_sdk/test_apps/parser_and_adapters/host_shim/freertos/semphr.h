/* Host-only mutex shim. Tests are single-process but use a real Win32 lock. */
#pragma once

#include "freertos/FreeRTOS.h"

#include <stdlib.h>
#include <windows.h>

typedef CRITICAL_SECTION *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    SemaphoreHandle_t lock = (SemaphoreHandle_t)malloc(sizeof(*lock));
    if (lock != NULL) {
        InitializeCriticalSection(lock);
    }
    return lock;
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t lock, TickType_t timeout)
{
    (void)timeout;
    if (lock == NULL) {
        return pdFALSE;
    }
    EnterCriticalSection(lock);
    return pdTRUE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t lock)
{
    if (lock == NULL) {
        return pdFALSE;
    }
    LeaveCriticalSection(lock);
    return pdTRUE;
}

static inline void vSemaphoreDelete(SemaphoreHandle_t lock)
{
    if (lock != NULL) {
        DeleteCriticalSection(lock);
        free(lock);
    }
}

