#pragma once
// Host stand-ins: one FIFO queue implementation, no tasks (tests call the app's handlers directly).
#include <stdint.h>
#include "esp_heap_caps.h"
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
typedef struct { int x; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }
#define taskENTER_CRITICAL(m) ((void)(m))
#define taskEXIT_CRITICAL(m) ((void)(m))
typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
void vTaskDelay(TickType_t t);
QueueHandle_t xQueueCreate(UBaseType_t n, UBaseType_t sz);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t t);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t t);
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                                   TaskHandle_t *h, BaseType_t core);
