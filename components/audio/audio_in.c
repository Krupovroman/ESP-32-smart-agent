#include "audio_in.h"
#include "audio_inject_priv.h"
#include "board_pins.h"

#include <math.h>
#include <string.h>
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "audio_in";

#define FRAME_SAMPLES  320                       // 20 ms at 16 kHz
#define REC_CAP        (AUDIO_IN_RATE * AUDIO_IN_MAX_MS / 1000)

static i2s_chan_handle_t s_rx;
static SemaphoreHandle_t s_lock;
static bool s_running;                           // init completed: the capture task runs
static int16_t *s_rec;                           // PSRAM, REC_CAP samples
static size_t s_rec_len, s_rec_max;
static volatile bool s_recording;
static volatile int s_level;
static volatile bool s_right_slot = true;
static volatile int s_shift = 12;
static volatile bool s_reconfig;
static volatile uint32_t s_rate = AUDIO_IN_RATE;
static volatile uint32_t s_pending_rate;
static audio_clip_t s_last;
static bool s_have_last;
static int16_t s_peak;
static double s_sum_sq;

static esp_err_t apply_slot(bool right)
{
    i2s_std_slot_config_t slot = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    slot.slot_mask = right ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT;
    i2s_channel_disable(s_rx);
    esp_err_t err = i2s_channel_reconfig_std_slot(s_rx, &slot);
    i2s_channel_enable(s_rx);
    return err;
}

static int dbfs_x10(double sum_sq, size_t n)
{
    if (n == 0 || sum_sq <= 0) {
        return -960;
    }
    double rms = sqrt(sum_sq / n) / 32768.0;
    return (int)lrint(200.0 * log10(rms));
}

static void frame_stats(const int16_t *pcm, size_t n, double *sum_sq, int16_t *peak)
{
    *sum_sq = 0;
    *peak = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t s = pcm[i];
        *sum_sq += (double)s * s;
        int16_t a = s < 0 ? (int16_t)(s == -32768 ? 32767 : -s) : (int16_t)s;
        if (a > *peak) *peak = a;
    }
}

static void capture_task(void *arg)
{
    int32_t *raw = heap_caps_malloc(FRAME_SAMPLES * sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    int16_t pcm[FRAME_SAMPLES];
    float dc_x1 = 0, dc_y1 = 0;
    for (;;) {
        if (s_pending_rate) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(s_pending_rate);
            i2s_channel_disable(s_rx);
            i2s_channel_reconfig_std_clock(s_rx, &clk);
            i2s_channel_enable(s_rx);
            s_rate = s_pending_rate;
            s_pending_rate = 0;
            xSemaphoreGive(s_lock);
        }
        if (s_reconfig) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            apply_slot(s_right_slot);
            s_reconfig = false;
            xSemaphoreGive(s_lock);
        }
        size_t n = 0;
        double sum_sq = 0;
        int16_t peak = 0;
        // fast injection fills a recording without waiting for the I2S clock
        bool skip_mic = s_recording && audio_inject_fast();
        size_t got = 0;
        if (!skip_mic) {
            if (i2s_channel_read(s_rx, raw, FRAME_SAMPLES * sizeof(int32_t), &got, pdMS_TO_TICKS(200)) != ESP_OK) {
                continue;
            }
        }
        n = got / sizeof(int32_t);
        int shift = s_shift;
        for (size_t i = 0; i < n; i++) {
            int32_t v = raw[i] >> shift;
            // DC blocker: y = x - x1 + 0.995 * y1
            float y = (float)v - dc_x1 + 0.995f * dc_y1;
            dc_x1 = (float)v;
            dc_y1 = y;
            int32_t s = (int32_t)y;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            pcm[i] = (int16_t)s;
            sum_sq += (double)s * s;
            int16_t a = s < 0 ? (int16_t)(s == -32768 ? 32767 : -s) : (int16_t)s;
            if (a > peak) peak = a;
        }
        if (s_recording) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            bool inject_end = false;
            if (s_recording && audio_inject_active()) {
                // injected samples replace the mic bit-exact: no shift, no DC blocker
                n = audio_inject_read(pcm, FRAME_SAMPLES);
                inject_end = n < FRAME_SAMPLES;
                frame_stats(pcm, n, &sum_sq, &peak);
            }
            size_t room = s_rec_max - s_rec_len;
            size_t take = n < room ? n : room;
            memcpy(&s_rec[s_rec_len], pcm, take * sizeof(int16_t));
            s_rec_len += take;
            s_sum_sq += sum_sq;
            if (peak > s_peak) s_peak = peak;
            if (s_rec_len >= s_rec_max || inject_end) {
                s_recording = false;
            }
            xSemaphoreGive(s_lock);
        }
        int db = dbfs_x10(sum_sq, n);
        int lvl = (db + 600) * 100 / 500;
        s_level = lvl < 0 ? 0 : lvl > 100 ? 100 : lvl;
    }
}

esp_err_t audio_in_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_rec = heap_caps_malloc(REC_CAP * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_rec, ESP_ERR_NO_MEM, TAG, "rec buffer");

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan.dma_desc_num = 6;
    chan.dma_frame_num = 240;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, NULL, &s_rx), TAG, "channel");

    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_IN_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_PIN_MIC_SCK,
            .ws = BOARD_PIN_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = BOARD_PIN_MIC_SD,
        },
    };
    std.slot_cfg.slot_mask = s_right_slot ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std), TAG, "std");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "enable");

    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(capture_task, "audio_in", 4096, NULL, 7, NULL, 0) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    s_running = true;
    ESP_LOGI(TAG, "mic ready (slot %s, shift %d)", s_right_slot ? "right" : "left", s_shift);
    return ESP_OK;
}

esp_err_t audio_in_start(uint32_t max_ms)
{
    // without the capture task nothing would ever end the recording (console "mic rec" waits)
    ESP_RETURN_ON_FALSE(s_running, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    if (max_ms == 0 || max_ms > AUDIO_IN_MAX_MS) {
        max_ms = AUDIO_IN_MAX_MS;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_rec_len = 0;
    s_rec_max = (size_t)s_rate * max_ms / 1000;
    if (s_rec_max > REC_CAP) {
        s_rec_max = REC_CAP;
    }
    s_sum_sq = 0;
    s_peak = 0;
    s_have_last = false;
    audio_inject_rewind();
    s_recording = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static void finish_clip(void)
{
    s_last.pcm = s_rec;
    s_last.samples = s_rec_len;
    s_last.ms = (uint32_t)(s_rec_len * 1000 / s_rate);
    s_last.rate = s_rate;
    s_last.peak = s_peak;
    s_last.rms_dbfs_x10 = dbfs_x10(s_sum_sq, s_rec_len);
    s_have_last = true;
}

esp_err_t audio_in_stop(audio_clip_t *out)
{
    if (!s_running) {
        if (out) {
            memset(out, 0, sizeof(*out));
        }
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_recording = false;
    finish_clip();
    if (out) {
        *out = s_last;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool audio_in_recording(void) { return s_recording; }

size_t audio_in_peek(const int16_t **pcm)
{
    *pcm = s_rec;
    if (!s_running) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = s_rec_len;
    xSemaphoreGive(s_lock);
    return n;
}

const audio_clip_t *audio_in_last(void)
{
    if (!s_have_last && !s_recording && s_rec_len > 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        finish_clip();   // recording ended by reaching max_ms
        xSemaphoreGive(s_lock);
    }
    return s_have_last ? &s_last : NULL;
}

int audio_in_level(void) { return s_level; }

void audio_in_set_rate(uint32_t rate)
{
    // the capture task applies the rate; without it this would wait forever
    if (s_running && rate >= 8000 && rate <= 48000 && rate != s_rate) {
        s_pending_rate = rate;
        while (s_pending_rate) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

uint32_t audio_in_get_rate(void) { return s_rate; }

void audio_in_set_format(bool right_slot, int shift)
{
    if (shift >= 0 && shift <= 20) {
        s_shift = shift;
    }
    if (right_slot != s_right_slot) {
        s_right_slot = right_slot;
        s_reconfig = true;
    }
}

esp_err_t audio_in_probe(uint32_t ms, audio_probe_t *out)
{
    ESP_RETURN_ON_FALSE(s_running, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(!s_recording, ESP_ERR_INVALID_STATE, TAG, "recording");
    ESP_RETURN_ON_FALSE(!audio_inject_active(), ESP_ERR_INVALID_STATE, TAG, "source is inject");
    bool orig = s_right_slot;
    for (int i = 0; i < 2; i++) {
        bool right = (i == 0);
        audio_in_set_format(right, s_shift);
        vTaskDelay(pdMS_TO_TICKS(120));      // let the slot switch and the DC blocker settle
        audio_in_start(ms);
        vTaskDelay(pdMS_TO_TICKS(ms + 40));
        audio_clip_t c;
        audio_in_stop(&c);
        size_t zeros = 0;
        for (size_t k = 0; k < c.samples; k++) {
            if (c.pcm[k] == 0) zeros++;
        }
        out->rms_dbfs_x10[i] = c.rms_dbfs_x10;
        out->zero_permille[i] = c.samples ? (int)(zeros * 1000 / c.samples) : 1000;
    }
    audio_in_set_format(orig, s_shift);
    return ESP_OK;
}

uint8_t *audio_clip_to_wav(const audio_clip_t *clip, size_t *len)
{
    size_t data = clip->samples * 2;
    uint8_t *w = heap_caps_malloc(44 + data, MALLOC_CAP_SPIRAM);
    if (!w) {
        return NULL;
    }
    uint32_t rate = clip->rate, byte_rate = clip->rate * 2, riff = 36 + data, d32 = data, fmt_len = 16;
    uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    memcpy(w, "RIFF", 4);           memcpy(w + 4, &riff, 4);
    memcpy(w + 8, "WAVEfmt ", 8);   memcpy(w + 16, &fmt_len, 4);
    memcpy(w + 20, &pcm, 2);        memcpy(w + 22, &ch, 2);
    memcpy(w + 24, &rate, 4);       memcpy(w + 28, &byte_rate, 4);
    memcpy(w + 32, &align, 2);      memcpy(w + 34, &bits, 2);
    memcpy(w + 36, "data", 4);      memcpy(w + 40, &d32, 4);
    memcpy(w + 44, clip->pcm, data);
    *len = 44 + data;
    return w;
}
