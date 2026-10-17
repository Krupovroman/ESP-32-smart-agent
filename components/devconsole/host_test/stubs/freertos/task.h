#pragma once
#include "freertos/FreeRTOS.h"
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *arg,
                                   UBaseType_t prio, TaskHandle_t *h, BaseType_t core);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
eTaskState eTaskGetState(TaskHandle_t t);
void vTaskDelay(TickType_t t);
