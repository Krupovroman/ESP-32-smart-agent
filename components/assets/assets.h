#pragma once
// Read-only asset pack in the "storage" partition (docs/architecture.md 1.0, tools/mkpack.py). The pack is a 4 KB
// header with up to 63 named entries, each 4 KB aligned. It is memory-mapped once through the flash cache: no RAM
// copy, no file system, no mount and never a format. Entries are reached by name; their data stays in flash.
// Written with "idf.py -p COM7 storage-flash"; a board without it simply reports ASSETS_ABSENT.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    ASSETS_ABSENT,      // no pack (blank or foreign partition): nothing is mapped
    ASSETS_BAD,         // header or entry CRC mismatch, unknown format, or the mapping failed
    ASSETS_UNCHECKED,   // header valid, entry CRCs not checked yet (assets_verify)
    ASSETS_OK,          // every entry matches its CRC-32
} assets_state_t;

typedef struct {
    const char *name;
    const uint8_t *data;    // memory-mapped flash, read-only; valid until reboot
    size_t size;
    uint32_t crc32;
    uint32_t aux;           // per-entry value from mkpack.py (a font's glyph count, a coverage map's code points)
} assets_entry_t;

// Maps the pack and checks its header (magic, format, header CRC, entry bounds): well under 1 ms. Any task, once.
esp_err_t assets_init(void);
// CRC-32 of every entry (estimated 50-80 ms per MB from flash): run it off taskLVGL. ESP_OK and ASSETS_OK when all
// match; ESP_ERR_INVALID_CRC and ASSETS_BAD otherwise. Call after assets_init.
esp_err_t assets_verify(void);
assets_state_t assets_state(void);
const char *assets_state_name(assets_state_t s);
// Pack label from mkpack.py ("" when absent), bytes mapped, verify time.
const char *assets_label(void);
size_t assets_size(void);
uint32_t assets_verify_ms(void);
// Entry by name (ASSETS_UNCHECKED or ASSETS_OK). Entries are only meaningful to their reader once verified.
bool assets_find(const char *name, assets_entry_t *out);
int assets_count(void);
bool assets_at(int i, assets_entry_t *out);
