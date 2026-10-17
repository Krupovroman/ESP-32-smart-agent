#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum { ST_U8, ST_U16, ST_U32, ST_STR } st_type_t;

#define ST_SECRET 0x01   // never printed in full

typedef struct {
    const char *key;
    st_type_t type;
    uint32_t def_num;
    const char *def_str;
    int32_t min, max;
    uint16_t max_len;
    uint8_t flags;
} setting_desc_t;

// nvs_flash_init (erasing on layout changes) and load of namespace "cfg" into a RAM cache.
esp_err_t settings_init(void);

int32_t settings_get_int(const char *key);
// Copies the value (always NUL-terminated); returns its length.
size_t settings_get_str(const char *key, char *buf, size_t len);
esp_err_t settings_set_int(const char *key, int32_t v);
esp_err_t settings_set_str(const char *key, const char *v);
// Parses v according to the key's type (used by the console and the phone portal).
esp_err_t settings_set_from_string(const char *key, const char *v);
// Printable value: secrets come back as "sk-or-…c3f9 (73)" or "" when empty.
void settings_format(const char *key, char *buf, size_t len);
bool settings_is_set(const char *key);
esp_err_t settings_reset(void);

const setting_desc_t *settings_table(size_t *n);
const setting_desc_t *settings_find(const char *key);

// Called (from the setter's task) after every successful change.
void settings_on_change(void (*cb)(const char *key, void *ctx), void *ctx);
