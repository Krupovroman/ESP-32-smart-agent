#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef struct {
    uint32_t rate;
    uint64_t bytes_played;
    uint32_t write_errors;
    size_t queued_bytes;
    uint8_t volume;
} audio_out_stats_t;

// I2S0 TX to the PCM5101 (BCLK 48, LRCK 38, DOUT 47); mono samples go to both channels.
// A playback task drains a 256 KB PSRAM ring into DMA; silence is sent when it is empty.
esp_err_t audio_out_init(void);

// Prepares a stream of 16-bit little-endian PCM at the given rate (1 or 2 channels).
esp_err_t audio_out_begin(uint32_t rate, uint8_t channels);
size_t audio_out_write(const void *s16le, size_t bytes, TickType_t wait);
// Drops everything queued (barge-in / cancel).
void audio_out_stop(void);
// True while queued audio is still being played.
bool audio_out_busy(void);

void audio_out_set_volume(uint8_t pct);
uint8_t audio_out_get_volume(void);

// Queues a sine tone (blocking until queued).
esp_err_t audio_out_tone(uint32_t hz, uint32_t ms, uint8_t amplitude_pct);

int audio_out_level(void);
void audio_out_get_stats(audio_out_stats_t *s);
