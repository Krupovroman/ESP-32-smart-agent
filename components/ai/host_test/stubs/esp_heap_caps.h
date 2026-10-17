#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_DMA 8
#define heap_caps_malloc(n, caps) malloc(n)
#define heap_caps_calloc(n, s, caps) calloc(n, s)
#define heap_caps_realloc(p, n, caps) realloc(p, n)
