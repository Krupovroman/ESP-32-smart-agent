#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM (1u << 10)
#define MALLOC_CAP_INTERNAL (1u << 11)
#define MALLOC_CAP_8BIT (1u << 2)
#define MALLOC_CAP_DMA (1u << 3)
#define heap_caps_malloc(n, caps) malloc(n)
#define heap_caps_calloc(n, sz, caps) calloc((n), (sz))
size_t heap_caps_get_free_size(uint32_t caps);
