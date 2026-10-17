#pragma once
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
typedef void *SemaphoreHandle_t;
typedef struct { int x; } StaticSemaphore_t;
typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
typedef void *EventGroupHandle_t;
typedef uint32_t EventBits_t;
#define BIT0 1u
#define BIT1 2u
#define BIT2 4u
void vTaskDelay(TickType_t t);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *b);
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *b);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
void vSemaphoreDelete(SemaphoreHandle_t s);
QueueHandle_t xQueueCreate(UBaseType_t n, UBaseType_t sz);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t t);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t t);
#define uxQueueMessagesWaiting(q) ((void)(q), 0u)
BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *), const char *name, uint32_t stack, void *arg,
                                           UBaseType_t prio, TaskHandle_t *h, BaseType_t core, uint32_t caps);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
EventGroupHandle_t xEventGroupCreate(void);
EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t b);
EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t b);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t g, EventBits_t b, BaseType_t clr, BaseType_t all, TickType_t t);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t);
