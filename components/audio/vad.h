#pragma once
// Energy speech detector on 20 ms frames of 16 kHz mono s16. Two uses (docs/ui.md 5.1, 5.6):
//  - the speech gate after a recording: a clip with no speech (fan noise, silence) never reaches the AI,
//    which would otherwise hallucinate a transcript;
//  - the live detector that ends a latched / auto-stop recording after speech followed by silence.
// Levels are dBFS x10 (-450 = -45.0 dBFS); -960 stands for digital silence.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VAD_FRAME          320          // samples per 20 ms frame at 16 kHz
#define VAD_FRAME_MS       20
#define VAD_DB_SILENT      (-960)
#define VAD_NO_HINT        (-1000)      // no ambient level known (injected audio)

typedef struct {
    int snr_db_x10;            // speech starts at floor + snr (120 = 12 dB)
    int hang_db_x10;           // and lasts while above threshold - hang (hysteresis)
    int abs_min_db_x10;        // the threshold never goes below this (very quiet rooms, digital silence)
    uint32_t min_speech_ms;    // less speech than this: "too short"
    uint32_t heard_ms;         // less speech than this: "not heard" (nothing worth calling the AI for)
} vad_cfg_t;

typedef enum {
    VAD_OK,                    // enough speech: send it
    VAD_TOO_SHORT,             // some speech, under min_speech_ms
    VAD_NO_SPEECH,             // noise or silence only
    VAD_SILENT_INPUT,          // every sample (almost) zero: a dead microphone when the source is the mic
} vad_verdict_t;

typedef struct {
    int floor_db_x10;          // noise floor used (15th percentile of the frames, or the ambient hint)
    int thr_db_x10;            // speech threshold
    int peak_db_x10;           // loudest frame
    int rms_db_x10;            // whole clip
    uint32_t total_ms, speech_ms, longest_ms;   // longest_ms: longest continuous speech segment
    vad_verdict_t verdict;
} vad_result_t;

// Defaults: 12 dB over the floor, 6 dB hysteresis, threshold >= -55 dBFS, 300 ms of speech, 100 ms heard.
void vad_default_cfg(vad_cfg_t *c);

// dBFS x10 of n samples.
int vad_frame_db(const int16_t *pcm, size_t n);

// Whole-clip gate. hint_db_x10: ambient noise floor measured before the recording, or VAD_NO_HINT.
void vad_analyse(const int16_t *pcm, size_t n, const vad_cfg_t *c, int hint_db_x10, vad_result_t *out);

// 32-bar RMS envelope of the clip (0..255, 0 = floor, 255 = floor + 40 dB) for the waveform bubble.
void vad_envelope(const int16_t *pcm, size_t n, int floor_db_x10, uint8_t env[32]);

// Streaming detector: feed whole frames as they are recorded.
typedef struct {
    vad_cfg_t cfg;
    int floor_x100;            // tracked floor, dB x100 (falls at once, rises 1 dB/s)
    int cap_x100;              // the floor never rises above this (ambient hint + 3 dB; INT32_MAX without one)
    bool started, in_speech;
    uint32_t frames;
    uint32_t speech_ms;        // total speech so far
    uint32_t silence_ms;       // non-speech since the last speech frame
} vad_live_t;

void vad_live_init(vad_live_t *v, const vad_cfg_t *c, int hint_db_x10);
// n must be a multiple of VAD_FRAME (a remainder is ignored).
void vad_live_feed(vad_live_t *v, const int16_t *pcm, size_t n);
