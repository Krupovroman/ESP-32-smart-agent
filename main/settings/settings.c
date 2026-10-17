#include "settings.h"

#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";
static const char *NS = "cfg";

#define S(k, t, dn, ds, mn, mx, ml, fl) { k, t, dn, ds, mn, mx, ml, fl },
static const setting_desc_t s_table[] = {
#include "settings_table.inc"
};
#undef S
#define N_SETTINGS (sizeof(s_table) / sizeof(s_table[0]))

typedef union {
    int32_t num;
    char *str;   // PSRAM copy
} value_t;

static value_t s_val[N_SETTINGS];
static SemaphoreHandle_t s_lock;
static void (*s_cb)(const char *key, void *ctx);
static void *s_cb_ctx;

static int index_of(const char *key)
{
    for (int i = 0; i < (int)N_SETTINGS; i++) {
        if (!strcmp(s_table[i].key, key)) {
            return i;
        }
    }
    return -1;
}

const setting_desc_t *settings_find(const char *key)
{
    int i = index_of(key);
    return i < 0 ? NULL : &s_table[i];
}

const setting_desc_t *settings_table(size_t *n)
{
    *n = N_SETTINGS;
    return s_table;
}

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (d) {
        memcpy(d, s, n);
    }
    return d;
}

static void load_one(nvs_handle_t h, int i)
{
    const setting_desc_t *d = &s_table[i];
    if (d->type == ST_STR) {
        size_t len = 0;
        char *v = NULL;
        if (nvs_get_str(h, d->key, NULL, &len) == ESP_OK && len > 0) {
            v = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
            if (v && nvs_get_str(h, d->key, v, &len) != ESP_OK) {
                free(v);
                v = NULL;
            }
        }
        s_val[i].str = v ? v : dup_str(d->def_str ? d->def_str : "");
        return;
    }
    uint32_t v = d->def_num;
    if (nvs_get_u32(h, d->key, &v) != ESP_OK) {
        v = d->def_num;
    }
    s_val[i].num = (int32_t)v;
}

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS layout changed, erasing");
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init");
    s_lock = xSemaphoreCreateMutex();

    nvs_handle_t h = 0;
    bool open = nvs_open(NS, NVS_READONLY, &h) == ESP_OK;   // fails until the first write: defaults
    for (int i = 0; i < (int)N_SETTINGS; i++) {
        if (open) {
            load_one(h, i);
        } else if (s_table[i].type == ST_STR) {
            s_val[i].str = dup_str(s_table[i].def_str ? s_table[i].def_str : "");
        } else {
            s_val[i].num = (int32_t)s_table[i].def_num;
        }
    }
    if (open) {
        nvs_close(h);
    }
    ESP_LOGI(TAG, "%u settings loaded", (unsigned)N_SETTINGS);
    return ESP_OK;
}

int32_t settings_get_int(const char *key)
{
    int i = index_of(key);
    if (i < 0 || s_table[i].type == ST_STR) {
        ESP_LOGE(TAG, "no int setting %s", key);
        return 0;
    }
    return s_val[i].num;
}

size_t settings_get_str(const char *key, char *buf, size_t len)
{
    int i = index_of(key);
    if (len == 0) {
        return 0;
    }
    buf[0] = 0;
    if (i < 0 || s_table[i].type != ST_STR) {
        ESP_LOGE(TAG, "no str setting %s", key);
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(buf, s_val[i].str ? s_val[i].str : "", len);
    xSemaphoreGive(s_lock);
    return strlen(buf);
}

bool settings_is_set(const char *key)
{
    int i = index_of(key);
    return i >= 0 && s_table[i].type == ST_STR && s_val[i].str && s_val[i].str[0];
}

static void notify(const char *key)
{
    if (s_cb) {
        s_cb(key, s_cb_ctx);
    }
}

esp_err_t settings_set_int(const char *key, int32_t v)
{
    int i = index_of(key);
    ESP_RETURN_ON_FALSE(i >= 0 && s_table[i].type != ST_STR, ESP_ERR_NOT_FOUND, TAG, "%s", key);
    ESP_RETURN_ON_FALSE(v >= s_table[i].min && v <= s_table[i].max, ESP_ERR_INVALID_ARG, TAG, "%s out of range", key);
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = nvs_set_u32(h, key, (uint32_t)v);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_RETURN_ON_ERROR(err, TAG, "write %s", key);
    s_val[i].num = v;
    notify(key);
    return ESP_OK;
}

esp_err_t settings_set_str(const char *key, const char *v)
{
    int i = index_of(key);
    ESP_RETURN_ON_FALSE(i >= 0 && s_table[i].type == ST_STR, ESP_ERR_NOT_FOUND, TAG, "%s", key);
    ESP_RETURN_ON_FALSE(strlen(v) <= s_table[i].max_len, ESP_ERR_INVALID_SIZE, TAG, "%s too long", key);
    if (!strcmp(key, "ai_url") || !strcmp(key, "tts_url") || !strcmp(key, "stt_url") || !strcmp(key, "ota_url")) {
        ESP_RETURN_ON_FALSE(v[0] == 0 || !strncmp(v, "https://", 8), ESP_ERR_INVALID_ARG, TAG, "%s must be https", key);
    }
    char *copy = dup_str(v);
    ESP_RETURN_ON_FALSE(copy, ESP_ERR_NO_MEM, TAG, "oom");
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, key, v);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        free(copy);
        ESP_LOGE(TAG, "write %s: %s", key, esp_err_to_name(err));
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    char *old = s_val[i].str;
    s_val[i].str = copy;
    xSemaphoreGive(s_lock);
    free(old);
    notify(key);
    return ESP_OK;
}

esp_err_t settings_set_from_string(const char *key, const char *v)
{
    const setting_desc_t *d = settings_find(key);
    ESP_RETURN_ON_FALSE(d, ESP_ERR_NOT_FOUND, TAG, "unknown %s", key);
    if (d->type == ST_STR) {
        return settings_set_str(key, v);
    }
    char *end = NULL;
    long n = strtol(v, &end, 0);
    ESP_RETURN_ON_FALSE(end && *end == 0 && end != v, ESP_ERR_INVALID_ARG, TAG, "not a number");
    return settings_set_int(key, (int32_t)n);
}

void settings_format(const char *key, char *buf, size_t len)
{
    const setting_desc_t *d = settings_find(key);
    if (!d) {
        strlcpy(buf, "?", len);
        return;
    }
    if (d->type != ST_STR) {
        snprintf(buf, len, "%ld", (long)settings_get_int(key));
        return;
    }
    char tmp[256];
    settings_get_str(key, tmp, sizeof(tmp));
    size_t n = strlen(tmp);
    if (!(d->flags & ST_SECRET) || n == 0) {
        strlcpy(buf, tmp, len);
    } else if (n <= 10) {
        snprintf(buf, len, "*** (%u)", (unsigned)n);
    } else {
        snprintf(buf, len, "%.6s...%s (%u)", tmp, tmp + n - 4, (unsigned)n);
    }
    memset(tmp, 0, sizeof(tmp));
}

esp_err_t settings_reset(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "open");
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    for (int i = 0; i < (int)N_SETTINGS; i++) {
        if (s_table[i].type == ST_STR) {
            char *old = s_val[i].str;
            s_val[i].str = dup_str(s_table[i].def_str ? s_table[i].def_str : "");
            free(old);
        } else {
            s_val[i].num = (int32_t)s_table[i].def_num;
        }
    }
    notify("*");
    return ESP_OK;
}

void settings_on_change(void (*cb)(const char *key, void *ctx), void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
}
