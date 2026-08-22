/* Host-only FreeRTOS subset for the Agent unit tests. */
#pragma once

#include <stdint.h>

typedef int BaseType_t;
typedef uint32_t TickType_t;

#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX

