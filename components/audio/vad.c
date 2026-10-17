// Energy speech detector (vad.h). Integer sums, one log10f per 20 ms frame.
#include "vad.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"

#define HIST_MIN        VAD_DB_SILENT    // 1 dB histogram buckets from -96 to 0 dBFS
#define HIST_BUCKETS    97
#define FLOOR_PCTL      15               // the floor is the 15th percentile of the frame levels
#define HINT_MARGIN     30               // an ambient hint counts as floor + 3 dB (it is a 20 ms minimum)
#define LIVE_RISE_X100  5                // live floor rise per frame: 0.05 dB, i.e. 2.5 dB/s
#define SILENT_PEAK     2                // a clip whose samples never exceed +-2 LSB carries no signal
#define ENV_RANGE       400              // envelope: floor .. floor + 40 dB

void vad_default_cfg(vad_cfg_t *c)
{
    c->snr_db_x10 = 120;
    c->hang_db_x10 = 60;
    c->abs_min_db_x10 = -550;
    c->min_speech_ms = 300;
    c->heard_ms = 100;
}

static int db_of(uint64_t sum_sq, size_t n)
{
    if (n == 0 || sum_sq == 0) {
        return VAD_DB_SILENT;
    }
    float ms = (float)sum_sq / (float)n;
    // 10*log10(mean square) in dB x10, minus full scale 10*log10(32768^2) = 90.3 dB
    int db = (int)lrintf(100.0f * log10f(ms)) - 903;
    return db < VAD_DB_SILENT ? VAD_DB_SILENT : (db > 0 ? 0 : db);
}

int vad_frame_db(const int16_t *pcm, size_t n)
{
    uint64_t s = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t v = pcm[i];
        s += (uint64_t)(v * v);
    }
    return db_of(s, n);
}

static int threshold(const vad_cfg_t *c, int floor_db_x10)
{
    int thr = floor_db_x10 + c->snr_db_x10;
    return thr < c->abs_min_db_x10 ? c->abs_min_db_x10 : thr;
}

void vad_analyse(const int16_t *pcm, size_t n, const vad_cfg_t *c, int hint_db_x10, vad_result_t *out)
{
    memset(out, 0, sizeof(*out));
    out->floor_db_x10 = out->thr_db_x10 = out->peak_db_x10 = out->rms_db_x10 = VAD_DB_SILENT;
    out->total_ms = (uint32_t)(n / (VAD_FRAME / VAD_FRAME_MS));
    out->verdict = VAD_NO_SPEECH;
    size_t nf = n / VAD_FRAME;
    if (!pcm || nf == 0) {
        return;
    }
    // pass 1: frame levels into a histogram, clip RMS and peak. The levels are kept (3 KB for 30 s, PSRAM)
    // so pass 2 does not read the whole clip from PSRAM again; without memory they are recomputed.
    int16_t *lv = heap_caps_malloc(nf * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    uint16_t hist[HIST_BUCKETS];
    memset(hist, 0, sizeof(hist));
    uint64_t total = 0;
    int32_t peak = 0;
    for (size_t f = 0; f < nf; f++) {
        const int16_t *p = pcm + f * VAD_FRAME;
        uint64_t s = 0;
        for (size_t i = 0; i < VAD_FRAME; i++) {
            int32_t v = p[i];
            s += (uint64_t)(v * v);
            int32_t a = v < 0 ? -v : v;
            if (a > peak) {
                peak = a;
            }
        }
        total += s;
        int db = db_of(s, VAD_FRAME);
        if (lv) {
            lv[f] = (int16_t)db;
        }
        hist[(db - HIST_MIN) / 10]++;
        if (db > out->peak_db_x10) {
            out->peak_db_x10 = db;
        }
    }
    out->rms_db_x10 = db_of(total, nf * VAD_FRAME);
    if (peak <= SILENT_PEAK) {
        out->verdict = VAD_SILENT_INPUT;
        free(lv);
        return;
    }
    size_t target = nf * FLOOR_PCTL / 100, cum = 0;
    int b = 0;
    for (; b < HIST_BUCKETS - 1; b++) {
        cum += hist[b];
        if (cum > target) {
            break;
        }
    }
    int floor = HIST_MIN + b * 10 + 5;
    if (hint_db_x10 != VAD_NO_HINT && hint_db_x10 + HINT_MARGIN < floor) {
        floor = hint_db_x10 + HINT_MARGIN;     // a clip that is speech almost throughout
    }
    int thr = threshold(c, floor);
    out->floor_db_x10 = floor;
    out->thr_db_x10 = thr;

    // pass 2: speech frames with hysteresis
    bool in = false;
    uint32_t speech = 0, run = 0, longest = 0;
    for (size_t f = 0; f < nf; f++) {
        int db = lv ? lv[f] : vad_frame_db(pcm + f * VAD_FRAME, VAD_FRAME);
        if (db >= thr) {
            in = true;
        } else if (in && db < thr - c->hang_db_x10) {
            in = false;
        }
        if (in) {
            speech++;
            run++;
            if (run > longest) {
                longest = run;
            }
        } else {
            run = 0;
        }
    }
    free(lv);
    out->speech_ms = speech * VAD_FRAME_MS;
    out->longest_ms = longest * VAD_FRAME_MS;
    if (out->speech_ms < c->heard_ms) {
        out->verdict = VAD_NO_SPEECH;
    } else if (out->speech_ms < c->min_speech_ms) {
        out->verdict = VAD_TOO_SHORT;
    } else {
        out->verdict = VAD_OK;
    }
}

void vad_envelope(const int16_t *pcm, size_t n, int floor_db_x10, uint8_t env[32])
{
    memset(env, 0, 32);
    size_t seg = n / 32;
    if (!pcm || seg == 0) {
        return;
    }
    for (int i = 0; i < 32; i++) {
        int v = (vad_frame_db(pcm + (size_t)i * seg, seg) - floor_db_x10) * 255 / ENV_RANGE;
        env[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
}

void vad_live_init(vad_live_t *v, const vad_cfg_t *c, int hint_db_x10)
{
    memset(v, 0, sizeof(*v));
    v->cfg = *c;
    if (hint_db_x10 != VAD_NO_HINT) {
        v->floor_x100 = hint_db_x10 * 10;
        v->started = true;
    }
}

void vad_live_feed(vad_live_t *v, const int16_t *pcm, size_t n)
{
    for (size_t o = 0; o + VAD_FRAME <= n; o += VAD_FRAME) {
        int db = vad_frame_db(pcm + o, VAD_FRAME);
        // minimum tracking: falls to any quieter frame at once, creeps up during speech
        if (!v->started || db * 10 < v->floor_x100) {
            v->floor_x100 = db * 10;
            v->started = true;
        } else {
            v->floor_x100 += LIVE_RISE_X100;
        }
        int thr = threshold(&v->cfg, v->floor_x100 / 10);
        if (db >= thr) {
            v->in_speech = true;
        } else if (v->in_speech && db < thr - v->cfg.hang_db_x10) {
            v->in_speech = false;
        }
        v->frames++;
        if (v->in_speech) {
            v->speech_ms += VAD_FRAME_MS;
            v->silence_ms = 0;
        } else {
            v->silence_ms += VAD_FRAME_MS;
        }
    }
}
