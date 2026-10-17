// Injected microphone source for tests: a WAV uploaded from the PC replaces the I2S samples
// of recordings, bit-exact (no shift, no DC blocker), so AI turns run without anyone speaking.
#include "audio_in.h"
#include "audio_inject_priv.h"

#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "audio_inject";

#define INJECT_MAX_SAMPLES ((size_t)AUDIO_IN_RATE * AUDIO_IN_MAX_MS / 1000)

static SemaphoreHandle_t s_lock;
static int16_t *s_pcm;              // PSRAM copy of the samples
static size_t s_n, s_pos;
static volatile audio_src_t s_src = AUDIO_SRC_MIC;
static volatile bool s_realtime = true;

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool lazy_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock != NULL;
}

esp_err_t audio_in_inject_load(const uint8_t *wav, size_t len)
{
    ESP_RETURN_ON_FALSE(lazy_init(), ESP_ERR_NO_MEM, TAG, "lock");
    ESP_RETURN_ON_FALSE(wav && len >= 12 && !memcmp(wav, "RIFF", 4) && !memcmp(wav + 8, "WAVE", 4),
                        ESP_ERR_INVALID_ARG, TAG, "not a RIFF/WAVE file");
    const uint8_t *data = NULL;
    size_t data_len = 0;
    bool fmt_ok = false;
    size_t off = 12;
    while (off + 8 <= len) {
        const uint8_t *ck = wav + off;
        uint32_t ck_len = rd32(ck + 4);
        size_t body = off + 8;
        size_t avail = len - body;
        if (!memcmp(ck, "fmt ", 4)) {
            ESP_RETURN_ON_FALSE(ck_len >= 16 && avail >= 16, ESP_ERR_INVALID_SIZE, TAG, "short fmt chunk");
            uint16_t fmt = rd16(wav + body), ch = rd16(wav + body + 2), bits = rd16(wav + body + 14);
            uint32_t rate = rd32(wav + body + 4);
            ESP_RETURN_ON_FALSE(fmt == 1 && ch == 1 && rate == AUDIO_IN_RATE && bits == 16, ESP_ERR_NOT_SUPPORTED, TAG,
                                "need PCM 16 kHz mono s16, got fmt %u, %u ch, %u Hz, %u bit", fmt, ch,
                                (unsigned)rate, bits);
            fmt_ok = true;
        } else if (!memcmp(ck, "data", 4)) {
            data = wav + body;
            data_len = ck_len < avail ? ck_len : avail;   // tolerate a streamed (oversized) length
            break;
        }
        if (ck_len > avail) {
            break;
        }
        off = body + ck_len + (ck_len & 1);   // chunks are padded to even sizes
    }
    ESP_RETURN_ON_FALSE(fmt_ok && data, ESP_ERR_INVALID_ARG, TAG, "fmt or data chunk missing");
    size_t n = data_len / 2;
    ESP_RETURN_ON_FALSE(n > 0 && n <= INJECT_MAX_SAMPLES, ESP_ERR_INVALID_SIZE, TAG, "%u samples (max %u)",
                        (unsigned)n, (unsigned)INJECT_MAX_SAMPLES);
    int16_t *pcm = heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(pcm, ESP_ERR_NO_MEM, TAG, "inject buffer");
    memcpy(pcm, data, n * sizeof(int16_t));   // WAV is little-endian, like the ESP32-S3

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int16_t *old = s_pcm;
    s_pcm = pcm;
    s_n = n;
    s_pos = 0;
    xSemaphoreGive(s_lock);
    free(old);
    ESP_LOGI(TAG, "loaded %u samples (%u ms)", (unsigned)n, (unsigned)(n * 1000 / AUDIO_IN_RATE));
    return ESP_OK;
}

esp_err_t audio_in_set_source(audio_src_t src, bool realtime)
{
    ESP_RETURN_ON_FALSE(lazy_init(), ESP_ERR_NO_MEM, TAG, "lock");
    if (src == AUDIO_SRC_INJECT) {
        ESP_RETURN_ON_FALSE(s_n > 0, ESP_ERR_INVALID_STATE, TAG, "no WAV loaded");
        ESP_RETURN_ON_FALSE(audio_in_get_rate() == AUDIO_IN_RATE, ESP_ERR_INVALID_STATE, TAG,
                            "capture rate must be %u Hz", AUDIO_IN_RATE);
    }
    s_realtime = realtime;
    s_src = src;
    return ESP_OK;
}

audio_src_t audio_in_get_source(void)
{
    return s_src;
}

bool audio_in_inject_realtime(void)
{
    return s_realtime;
}

size_t audio_in_inject_samples(void)
{
    return s_n;
}

bool audio_inject_active(void)
{
    return s_src == AUDIO_SRC_INJECT && s_n > 0;
}

bool audio_inject_fast(void)
{
    return audio_inject_active() && !s_realtime;
}

void audio_inject_rewind(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_pos = 0;
        xSemaphoreGive(s_lock);
    }
}

size_t audio_inject_read(int16_t *dst, size_t n)
{
    if (!s_lock) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t k = s_n - s_pos < n ? s_n - s_pos : n;
    if (k > 0) {
        memcpy(dst, s_pcm + s_pos, k * sizeof(int16_t));
        s_pos += k;
    }
    xSemaphoreGive(s_lock);
    return k;
}
