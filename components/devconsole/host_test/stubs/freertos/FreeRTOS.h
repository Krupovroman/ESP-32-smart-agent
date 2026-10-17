#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef enum { eRunning, eReady, eBlocked, eSuspended, eDeleted, eInvalid } eTaskState;
#define taskSCHEDULER_RUNNING 2
bool xPortInIsrContext(void);
BaseType_t xTaskGetSchedulerState(void);
