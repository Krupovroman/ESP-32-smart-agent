#pragma once
// Included by LVGL's lv_mem_core_builtin.c only (see main/CMakeLists.txt): the TLSF pool lives in PSRAM.
#include "esp_heap_caps.h"
#define LV_MEM_POOL_ALLOC(size) heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
