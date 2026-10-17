#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define AUDIO_IN_RATE        16000
#define AUDIO_IN_MAX_MS      30000

typedef struct {
    const int16_t *pcm;     // 16 kHz mono s16, owned by audio_in (valid until the next start)
    size_t samples;
    uint32_t ms;
    int16_t peak;
    int rms_dbfs_x10;       // e.g. -423 = -42.3 dBFS
} audio_clip_t;

typedef struct {
    int rms_dbfs_x10[2];    // [0] = right slot, [1] = left slot
    int zero_permille[2];   // share of samples that were exactly 0
} audio_probe_t;

// I2S1 RX from the ICS-43434 (SCK 15, WS 2, SD 39). Starts a capture task that keeps the
// input level meter running; recording into PSRAM happens between start and stop.
esp_err_t audio_in_init(void);

esp_err_t audio_in_start(uint32_t max_ms);
esp_err_t audio_in_stop(audio_clip_t *out);
bool audio_in_recording(void);
// The last finished clip (also after stop), or NULL.
const audio_clip_t *audio_in_last(void);

// 0..100 from the RMS of the last 20 ms (log scale, -60 dBFS -> 0, -10 dBFS -> 100).
int audio_in_level(void);

// Which I2S slot carries the mic (true = right) and how far 32-bit samples are shifted down.
void audio_in_set_format(bool right_slot, int shift);
// Measures both slots for ms each (blocks; not while recording).
esp_err_t audio_in_probe(uint32_t ms, audio_probe_t *out);

// Wraps the PCM of a clip into a WAV file image in PSRAM (caller frees with free()).
uint8_t *audio_clip_to_wav(const audio_clip_t *clip, size_t *len);
