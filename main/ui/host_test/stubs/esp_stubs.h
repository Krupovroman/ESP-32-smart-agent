#pragma once
// ESP-IDF / FreeRTOS stand-ins for main/ui/ui_font.c, main/ui/ui_str.c and components/assets/assets.c on the PC
// (implemented in t_font.c). Tasks run synchronously; a semaphore wait pumps lv_timer_handler instead.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

typedef int esp_err_t;
#define ESP_OK                  0
#define ESP_FAIL                -1
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_NOT_FOUND       0x105
#define ESP_ERR_TIMEOUT         0x107
#define ESP_ERR_INVALID_CRC     0x109
const char *esp_err_to_name(esp_err_t e);

#define MALLOC_CAP_8BIT         (1 << 2)
#define MALLOC_CAP_SPIRAM       (1 << 10)
#define MALLOC_CAP_INTERNAL     (1 << 11)
static inline void *heap_caps_malloc(size_t n, uint32_t caps) { (void)caps; return malloc(n); }
static inline void *heap_caps_calloc(size_t a, size_t n, uint32_t caps) { (void)caps; return calloc(a, n); }
static inline void heap_caps_free(void *p) { free(p); }
static inline size_t heap_caps_get_allocated_size(void *p) { return _msize(p); }

#define ESP_LOGE(tag, fmt, ...) printf("E %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) printf("I %s: " fmt "\n", tag, ##__VA_ARGS__)

int64_t esp_timer_get_time(void);

typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef void (*TaskFunction_t)(void *);
#define pdPASS  1
#define pdTRUE  1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                           UBaseType_t prio, TaskHandle_t *h, BaseType_t core, uint32_t caps);
void vTaskDeleteWithCaps(TaskHandle_t h);
static inline void vTaskDelay(uint32_t t) { (void)t; }
static inline UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t h) { (void)h; return 0; }
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, uint32_t t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
void vSemaphoreDelete(SemaphoreHandle_t s);

typedef enum { ESP_PARTITION_TYPE_APP = 0, ESP_PARTITION_TYPE_DATA = 1 } esp_partition_type_t;
#define ESP_PARTITION_SUBTYPE_ANY 0xff
typedef enum { ESP_PARTITION_MMAP_DATA, ESP_PARTITION_MMAP_INST } esp_partition_mmap_memory_t;
typedef uint32_t esp_partition_mmap_handle_t;
typedef struct {
    uint32_t address, size;
    char label[17];
} esp_partition_t;
const esp_partition_t *esp_partition_find_first(esp_partition_type_t t, int st, const char *label);
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t n);
esp_err_t esp_partition_mmap(const esp_partition_t *p, size_t off, size_t n, esp_partition_mmap_memory_t m,
                             const void **out, esp_partition_mmap_handle_t *h);
uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len);
