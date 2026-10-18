#include "audio_out.h"
#include "board_pins.h"

#include <math.h>
#include <string.h>
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "audio_out";

// 2 MB in PSRAM (~43 s at 24 kHz) so a whole TTS reply can be fetched while it plays
// and the shared HTTPS connection is released early (architecture 1.6)
#define RING_BYTES   (2 * 1024 * 1024)
#define CHUNK_SAMPLES 240

static i2s_chan_handle_t s_tx;
static StreamBufferHandle_t s_ring;
static volatile uint32_t s_rate = 24000;
static volatile uint32_t s_pending_rate;
static volatile uint8_t s_channels = 1;
static volatile uint8_t s_volume = 70;
static volatile int32_t s_gain_q16;
static volatile int s_level;
static volatile bool s_flush;
static volatile bool s_playing;
static uint64_t s_bytes_played;
static uint32_t s_write_errors;

static void update_gain(void)
{
    uint32_t v = s_volume;
    s_gain_q16 = (int32_t)(v * v * 65536 / 10000);
}

static void play_task(void *arg)
{
    int16_t in[CHUNK_SAMPLES * 2];
    int32_t *out = heap_caps_malloc(CHUNK_SAMPLES * sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    for (;;) {
        if (s_pending_rate) {
            uint32_t rate = s_pending_rate;
            s_pending_rate = 0;
            i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
            i2s_channel_disable(s_tx);
            i2s_channel_reconfig_std_clock(s_tx, &clk);
            i2s_channel_enable(s_tx);
            s_rate = rate;
        }
        if (s_flush) {
            // Drain as the (only) reader instead of xStreamBufferReset(): a reset that lands while
            // the TTS writer is inside xStreamBufferSend() zeroes head and tail under it, and the
            // writer's later head store makes the ring report the whole previous reply as queued
            // again (replayed after a barge-in). Receiving moves only the tail: safe with one writer.
            s_flush = false;
            while (xStreamBufferReceive(s_ring, in, sizeof(in), 0) > 0) {
            }
            s_level = 0;
        }
        size_t frame_bytes = s_channels * sizeof(int16_t);
        size_t got = xStreamBufferReceive(s_ring, in, CHUNK_SAMPLES * frame_bytes, pdMS_TO_TICKS(20));
        if (got < frame_bytes) {
            s_playing = false;
            s_level = 0;
            continue;     // DMA keeps sending zeros (auto_clear)
        }
        s_playing = true;
        size_t n = got / frame_bytes;
        int32_t gain = s_gain_q16;
        uint64_t sum_sq = 0;
        for (size_t i = 0; i < n; i++) {
            int32_t s = s_channels == 2 ? ((int32_t)in[2 * i] + in[2 * i + 1]) / 2 : in[i];
            int64_t v = (int64_t)s * gain;            // s16 * Q16 = s32 range
            if (v > INT32_MAX) v = INT32_MAX;
            if (v < INT32_MIN) v = INT32_MIN;
            out[i] = (int32_t)v;
            sum_sq += (uint64_t)((int64_t)s * s);
        }
        size_t written = 0;
        if (i2s_channel_write(s_tx, out, n * sizeof(int32_t), &written, pdMS_TO_TICKS(200)) != ESP_OK) {
            s_write_errors++;
        }
        s_bytes_played += n * frame_bytes;
        double rms = sqrt((double)sum_sq / n) / 32768.0;
        int db = rms > 0 ? (int)(20.0 * log10(rms)) : -96;
        int lvl = (db + 60) * 100 / 50;
        s_level = lvl < 0 ? 0 : lvl > 100 ? 100 : lvl;
    }
}

esp_err_t audio_out_init(void)
{
    update_gain();
    s_ring = xStreamBufferCreateWithCaps(RING_BYTES, 1, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_ring, ESP_ERR_NO_MEM, TAG, "ring");

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = 8;
    chan.dma_frame_num = 240;
    chan.auto_clear_after_cb = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &s_tx, NULL), TAG, "channel");

    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(24000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_PIN_SPK_BCLK,
            .ws = BOARD_PIN_SPK_LRCK,
            .dout = BOARD_PIN_SPK_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    std.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;   // mono sample duplicated to L and R
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std), TAG, "std");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "enable");

    xTaskCreatePinnedToCore(play_task, "audio_out", 4096, NULL, 8, NULL, 0);
    ESP_LOGI(TAG, "speaker ready");
    return ESP_OK;
}

esp_err_t audio_out_begin(uint32_t rate, uint8_t channels)
{
    ESP_RETURN_ON_FALSE(channels == 1 || channels == 2, ESP_ERR_INVALID_ARG, TAG, "channels");
    ESP_RETURN_ON_FALSE(rate >= 8000 && rate <= 48000, ESP_ERR_INVALID_ARG, TAG, "rate");
    // wait for the previous stream to drain before changing the format
    while (xStreamBufferBytesAvailable(s_ring) > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_channels = channels;
    if (rate != s_rate) {
        s_pending_rate = rate;
        while (s_pending_rate) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    return ESP_OK;
}

size_t audio_out_write(const void *s16le, size_t bytes, TickType_t wait)
{
    return xStreamBufferSend(s_ring, s16le, bytes, wait);
}

void audio_out_stop(void)
{
    s_flush = true;
}

bool audio_out_busy(void)
{
    return s_playing || xStreamBufferBytesAvailable(s_ring) > 0;
}

void audio_out_set_volume(uint8_t pct)
{
    s_volume = pct > 100 ? 100 : pct;
    update_gain();
}

uint8_t audio_out_get_volume(void) { return s_volume; }

esp_err_t audio_out_tone(uint32_t hz, uint32_t ms, uint8_t amplitude_pct)
{
    audio_out_begin(s_rate, 1);
    const uint32_t rate = s_rate;
    const uint32_t total = rate * ms / 1000;
    const float amp = 32767.0f * (amplitude_pct > 100 ? 100 : amplitude_pct) / 100.0f;
    const uint32_t fade = rate / 100;       // 10 ms fade in/out, no clicks
    int16_t buf[256];
    float phase = 0, step = 2.0f * (float)M_PI * hz / rate;
    for (uint32_t i = 0; i < total;) {
        uint32_t n = total - i < 256 ? total - i : 256;
        for (uint32_t k = 0; k < n; k++, i++) {
            float env = 1.0f;
            if (i < fade) env = (float)i / fade;
            else if (total - i < fade) env = (float)(total - i) / fade;
            buf[k] = (int16_t)(amp * env * sinf(phase));
            phase += step;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
        audio_out_write(buf, n * sizeof(int16_t), portMAX_DELAY);
    }
    return ESP_OK;
}

int audio_out_level(void) { return s_level; }

void audio_out_get_stats(audio_out_stats_t *s)
{
    s->rate = s_rate;
    s->bytes_played = s_bytes_played;
    s->write_errors = s_write_errors;
    s->queued_bytes = xStreamBufferBytesAvailable(s_ring);
    s->volume = s_volume;
}
