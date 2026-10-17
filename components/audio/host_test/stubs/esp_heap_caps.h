#pragma once
// Host stub: PSRAM allocations come from the C heap.
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
static inline void *heap_caps_malloc(size_t n, int caps)
{
    (void)caps;
    return malloc(n);
}
