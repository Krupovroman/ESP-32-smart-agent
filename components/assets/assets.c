// Asset pack in the "storage" partition (assets.h). Layout written by tools/mkpack.py, little-endian:
//   0     "RAPK", u16 format (1), u16 entry count, u32 pack size, u32 CRC-32 of header bytes 16..4095, label[48]
//   64    entries, 64 bytes each: name[32] (NUL-terminated), u32 offset (4 KB aligned), u32 size, u32 CRC-32,
//         u32 aux, 16 reserved bytes
//   4096  entry data
// The partition is found by label, so its subtype in partitions.csv (spiffs today) does not matter.
#include "assets.h"

#include <stdatomic.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "assets";

#define PACK_FORMAT     1
#define HDR_SIZE        4096
#define ENTRY_OFS       64
#define ENTRY_SIZE      64
#define MAX_ENTRIES     ((HDR_SIZE - ENTRY_OFS) / ENTRY_SIZE)
#define NAME_LEN        32
#define LABEL_LEN       48
#define CRC_SLICE       (256 * 1024)    // verify yields between slices

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t format;
    uint16_t count;
    uint32_t size;
    uint32_t hdr_crc;
    char label[LABEL_LEN];
} pack_hdr_t;

typedef struct __attribute__((packed)) {
    char name[NAME_LEN];
    uint32_t offset, size, crc32, aux;
    uint8_t reserved[16];
} pack_entry_t;

_Static_assert(sizeof(pack_hdr_t) == ENTRY_OFS, "pack header size");
_Static_assert(sizeof(pack_entry_t) == ENTRY_SIZE, "pack entry size");

static atomic_int s_state = ASSETS_ABSENT;
static const uint8_t *s_base;           // mapped pack (header included)
static esp_partition_mmap_handle_t s_map;
static size_t s_size;
static int s_count;
static uint32_t s_verify_ms;
static char s_label[LABEL_LEN + 1];

static const pack_entry_t *entry(int i)
{
    return (const pack_entry_t *)(s_base + ENTRY_OFS + (size_t)i * ENTRY_SIZE);
}

static bool header_ok(const uint8_t *h, size_t part_size)
{
    const pack_hdr_t *ph = (const pack_hdr_t *)h;
    if (memcmp(ph->magic, "RAPK", 4) != 0) {
        return false;
    }
    if (ph->format != PACK_FORMAT || ph->count > MAX_ENTRIES || ph->size < HDR_SIZE || ph->size > part_size) {
        ESP_LOGW(TAG, "pack format %u, %u entries, %u bytes: not usable", ph->format, ph->count,
                 (unsigned)ph->size);
        return false;
    }
    if (esp_rom_crc32_le(0, h + 16, HDR_SIZE - 16) != ph->hdr_crc) {
        ESP_LOGW(TAG, "pack header CRC mismatch");
        return false;
    }
    for (int i = 0; i < ph->count; i++) {
        const pack_entry_t *e = (const pack_entry_t *)(h + ENTRY_OFS + (size_t)i * ENTRY_SIZE);
        bool named = memchr(e->name, 0, NAME_LEN) != NULL && e->name[0];
        if (!named || e->offset < HDR_SIZE || (e->offset % HDR_SIZE) || e->offset > ph->size ||
            e->size > ph->size - e->offset) {
            ESP_LOGW(TAG, "pack entry %d out of bounds", i);
            return false;
        }
    }
    return true;
}

esp_err_t assets_init(void)
{
    if (s_base) {
        return ESP_OK;
    }
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                                        "storage");
    if (!p) {
        ESP_LOGW(TAG, "no storage partition");
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t *h = heap_caps_malloc(HDR_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!h) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_partition_read(p, 0, h, HDR_SIZE);
    if (err != ESP_OK) {
        heap_caps_free(h);
        atomic_store(&s_state, ASSETS_BAD);
        return err;
    }
    if (memcmp(h, "RAPK", 4) != 0) {
        heap_caps_free(h);
        ESP_LOGI(TAG, "no asset pack in storage (idf.py storage-flash writes it)");
        return ESP_ERR_NOT_FOUND;
    }
    if (!header_ok(h, p->size)) {
        heap_caps_free(h);
        atomic_store(&s_state, ASSETS_BAD);
        return ESP_ERR_INVALID_CRC;
    }
    const pack_hdr_t *ph = (const pack_hdr_t *)h;
    size_t size = ph->size;
    int count = ph->count;
    memcpy(s_label, ph->label, LABEL_LEN);
    s_label[LABEL_LEN] = 0;
    heap_caps_free(h);

    const void *base = NULL;
    err = esp_partition_mmap(p, 0, size, ESP_PARTITION_MMAP_DATA, &base, &s_map);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmap %u bytes: %s", (unsigned)size, esp_err_to_name(err));
        atomic_store(&s_state, ASSETS_BAD);
        return err;
    }
    s_base = base;
    s_size = size;
    s_count = count;
    atomic_store(&s_state, ASSETS_UNCHECKED);
    ESP_LOGI(TAG, "pack \"%s\": %d entries, %u bytes mapped", s_label, s_count, (unsigned)s_size);
    return ESP_OK;
}

esp_err_t assets_verify(void)
{
    assets_state_t st = (assets_state_t)atomic_load(&s_state);
    if (st == ASSETS_OK) {
        return ESP_OK;
    }
    if (st != ASSETS_UNCHECKED) {
        return st == ASSETS_ABSENT ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_CRC;
    }
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < s_count; i++) {
        const pack_entry_t *e = entry(i);
        uint32_t crc = 0;
        for (size_t o = 0; o < e->size; o += CRC_SLICE) {
            size_t n = e->size - o < CRC_SLICE ? e->size - o : CRC_SLICE;
            crc = esp_rom_crc32_le(crc, s_base + e->offset + o, (uint32_t)n);
            vTaskDelay(1);      // let the idle task run: a 5 MB pack takes a few hundred ms
        }
        if (crc != e->crc32) {
            ESP_LOGE(TAG, "entry %s: CRC %08lx, expected %08lx (re-flash: idf.py storage-flash)", e->name,
                     (unsigned long)crc, (unsigned long)e->crc32);
            atomic_store(&s_state, ASSETS_BAD);
            return ESP_ERR_INVALID_CRC;
        }
    }
    s_verify_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    atomic_store(&s_state, ASSETS_OK);
    ESP_LOGI(TAG, "pack verified in %u ms", (unsigned)s_verify_ms);
    return ESP_OK;
}

assets_state_t assets_state(void)
{
    return (assets_state_t)atomic_load(&s_state);
}

const char *assets_state_name(assets_state_t s)
{
    switch (s) {
    case ASSETS_OK: return "ok";
    case ASSETS_UNCHECKED: return "checking";
    case ASSETS_BAD: return "bad";
    default: return "absent";
    }
}

const char *assets_label(void)
{
    return s_label;
}

size_t assets_size(void)
{
    return s_size;
}

uint32_t assets_verify_ms(void)
{
    return s_verify_ms;
}

int assets_count(void)
{
    return s_base ? s_count : 0;
}

bool assets_at(int i, assets_entry_t *out)
{
    assets_state_t st = assets_state();
    if (!s_base || i < 0 || i >= s_count || (st != ASSETS_OK && st != ASSETS_UNCHECKED)) {
        return false;
    }
    const pack_entry_t *e = entry(i);
    if (out) {
        *out = (assets_entry_t){ e->name, s_base + e->offset, e->size, e->crc32, e->aux };
    }
    return true;
}

bool assets_find(const char *name, assets_entry_t *out)
{
    for (int i = 0; name && i < assets_count(); i++) {
        if (!strncmp(entry(i)->name, name, NAME_LEN)) {
            return assets_at(i, out);
        }
    }
    return false;
}
