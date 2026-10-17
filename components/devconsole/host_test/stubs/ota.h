#pragma once
// Host stand-in for components/ota/ota.h (only what devconsole.c reads).
#include <stdbool.h>
#include <stdint.h>
typedef int esp_ota_img_states_t;
typedef enum { OTA_ST_NOT_RUN, OTA_ST_OK, OTA_ST_FAIL, OTA_ST_SKIPPED } ota_selftest_t;
typedef struct {
    char running[17], next[17], boot[17];
    esp_ota_img_states_t state;
    uint32_t boot_cnt;
    ota_selftest_t selftest;
} ota_info_t;
void ota_get_info(ota_info_t *o);
const char *ota_state_name(esp_ota_img_states_t s);
const char *ota_selftest_name(ota_selftest_t s);
