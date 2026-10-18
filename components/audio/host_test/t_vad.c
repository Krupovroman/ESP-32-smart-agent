// Speech gate tests on synthetic 16 kHz clips. The levels follow the bench measurements: PC fan noise at
// about -45 dBFS, speech peaks at about -22 dBFS.
#include "vad.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 16000

static int s_checks, s_fail;
#define CHECK(c)                                                              \
    do {                                                                      \
        s_checks++;                                                           \
        if (!(c)) {                                                           \
            s_fail++;                                                         \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                     \
    } while (0)

static uint32_t s_rng = 12345;

static double urand(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return ((s_rng >> 8) & 0xFFFFFF) / (double)0x800000 - 1.0;   // -1..1
}

// roughly Gaussian noise with the given RMS in dBFS
static double noise(double rms_db)
{
    double g = (urand() + urand() + urand() + urand()) / 1.1547;   // unit variance
    return g * 32768.0 * pow(10.0, rms_db / 20.0);
}

static int16_t clamp16(double v)
{
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

// fan: stationary noise with a slow +-2 dB wobble
static void fan(int16_t *p, size_t n, double db, size_t t0)
{
    for (size_t i = 0; i < n; i++) {
        double wob = 2.0 * sin(2 * 3.14159265 * 0.5 * (double)(t0 + i) / RATE);
        p[i] = clamp16(noise(db + wob));
    }
}

// speech-like: syllables of 160 ms at rms_db separated by 60 ms gaps 20 dB lower, on top of the background
static void speech(int16_t *p, size_t n, double rms_db, double bg_db, size_t t0)
{
    for (size_t i = 0; i < n; i++) {
        size_t ms = i * 1000 / RATE;
        bool on = ms % 220 < 160;
        double env = on ? sin(3.14159265 * (double)(ms % 220) / 160.0) : 0.0;   // rise and fall
        double v = noise(bg_db) + (on ? env * 1.4 * noise(rms_db) : noise(rms_db - 20));
        v += on ? env * 0.5 * 32768.0 * pow(10.0, rms_db / 20.0) * sin(2 * 3.14159265 * 180.0 * (double)(t0 + i) / RATE) : 0;
        p[i] = clamp16(v);
    }
}

static int16_t *clip(size_t ms)
{
    int16_t *p = calloc(ms * RATE / 1000 + 1, sizeof(int16_t));
    return p;
}

static void report(const char *name, const vad_result_t *r)
{
    static const char *const v[] = { "ok", "too_short", "no_speech", "silent" };
    printf("    %-28s %-9s speech %4u ms (longest %4u) of %5u, floor %.1f thr %.1f peak %.1f\n", name, v[r->verdict],
           (unsigned)r->speech_ms, (unsigned)r->longest_ms, (unsigned)r->total_ms, r->floor_db_x10 / 10.0,
           r->thr_db_x10 / 10.0, r->peak_db_x10 / 10.0);
}

static void t_gate(void)
{
    printf("t_gate\n");
    vad_cfg_t c;
    vad_default_cfg(&c);
    vad_result_t r;

    // fan only, 3 s: the hallucination case of the bench test must never reach the AI
    int16_t *p = clip(3000);
    fan(p, 48000, -45, 0);
    vad_analyse(p, 48000, &c, VAD_NO_HINT, &r);
    report("fan 3 s", &r);
    CHECK(r.verdict == VAD_NO_SPEECH);
    vad_analyse(p, 48000, &c, -470, &r);
    report("fan 3 s, ambient hint", &r);
    CHECK(r.verdict == VAD_NO_SPEECH);
    // a louder fan
    fan(p, 48000, -35, 0);
    vad_analyse(p, 48000, &c, -370, &r);
    report("loud fan 3 s", &r);
    CHECK(r.verdict == VAD_NO_SPEECH);
    free(p);

    // fan + 1.3 s of speech at -22 dBFS
    p = clip(4000);
    fan(p, 24000, -45, 0);
    speech(p + 24000, 20800, -22, -45, 24000);
    fan(p + 44800, 64000 - 44800, -45, 44800);
    vad_analyse(p, 64000, &c, VAD_NO_HINT, &r);
    report("fan + speech 1.3 s", &r);
    CHECK(r.verdict == VAD_OK && r.speech_ms >= 700 && r.speech_ms <= 1500);
    uint8_t env[32];
    vad_envelope(p, 64000, r.floor_db_x10, env);
    CHECK(env[2] < 80 && env[16] > env[2] + 60);       // noise low, speech high
    free(p);

    // quiet speech: 15 dB over the fan
    p = clip(3000);
    fan(p, 16000, -45, 0);
    speech(p + 16000, 16000, -30, -45, 16000);
    fan(p + 32000, 16000, -45, 32000);
    vad_analyse(p, 48000, &c, -470, &r);
    report("quiet speech (+15 dB)", &r);
    CHECK(r.verdict == VAD_OK);
    free(p);

    // a single 200 ms syllable: too short; a 40 ms click: not heard
    p = clip(2000);
    fan(p, 32000, -45, 0);
    speech(p + 12000, 3200, -22, -45, 12000);
    vad_analyse(p, 32000, &c, VAD_NO_HINT, &r);
    report("one syllable", &r);
    CHECK(r.verdict == VAD_TOO_SHORT || r.verdict == VAD_NO_SPEECH);
    CHECK(r.speech_ms < c.min_speech_ms);
    fan(p, 32000, -45, 0);
    for (size_t i = 12000; i < 12640; i++) {
        p[i] = clamp16(p[i] + noise(-15));
    }
    vad_analyse(p, 32000, &c, VAD_NO_HINT, &r);
    report("40 ms click", &r);
    CHECK(r.verdict == VAD_NO_SPEECH);
    free(p);

    // the user talks through the whole clip: the ambient hint keeps the floor at the room level
    p = clip(2000);
    speech(p, 32000, -22, -45, 0);
    vad_analyse(p, 32000, &c, -470, &r);
    report("speech only, hint", &r);
    CHECK(r.verdict == VAD_OK);
    vad_analyse(p, 32000, &c, VAD_NO_HINT, &r);
    report("speech only, no hint", &r);
    CHECK(r.verdict == VAD_OK);
    free(p);

    // TTS-made fixture: digital silence around the speech (injected audio, no hint)
    p = clip(2000);
    speech(p + 4800, 16000, -20, -200, 4800);
    vad_analyse(p, 32000, &c, VAD_NO_HINT, &r);
    report("fixture with digital silence", &r);
    CHECK(r.verdict == VAD_OK && r.thr_db_x10 == c.abs_min_db_x10);
    free(p);

    // dead microphone: zeros
    p = clip(1000);
    vad_analyse(p, 16000, &c, VAD_NO_HINT, &r);
    report("zeros", &r);
    CHECK(r.verdict == VAD_SILENT_INPUT);
    // shorter than one frame
    vad_analyse(p, 100, &c, VAD_NO_HINT, &r);
    CHECK(r.verdict == VAD_NO_SPEECH && r.total_ms == 6);
    free(p);
}

static void t_live(void)
{
    printf("t_live\n");
    vad_cfg_t c;
    vad_default_cfg(&c);
    // 1 s fan, 1.3 s speech, 2 s fan: fed in 20 ms frames as the recording grows
    size_t n = 4300 * 16;
    int16_t *p = clip(4300);
    fan(p, 16000, -45, 0);
    speech(p + 16000, 20800, -22, -45, 16000);
    fan(p + 36800, n - 36800, -45, 36800);
    vad_live_t v;
    vad_live_init(&v, &c, -470);
    uint32_t stop_ms = 0;
    for (size_t o = 0; o + VAD_FRAME <= n; o += VAD_FRAME) {
        vad_live_feed(&v, p + o, VAD_FRAME);
        if (!stop_ms && v.speech_ms >= 400 && v.silence_ms >= 1200) {
            stop_ms = (uint32_t)((o + VAD_FRAME) * 1000 / RATE);
        }
    }
    printf("    auto-stop at %u ms (speech ends at 2300 ms), speech %u ms\n", (unsigned)stop_ms, (unsigned)v.speech_ms);
    CHECK(stop_ms >= 3400 && stop_ms <= 3700);
    // fan only never triggers, with or without a hint
    fan(p, n, -45, 0);
    vad_live_init(&v, &c, VAD_NO_HINT);
    vad_live_feed(&v, p, n / VAD_FRAME * VAD_FRAME);
    printf("    fan only: speech %u ms\n", (unsigned)v.speech_ms);
    CHECK(v.speech_ms < 100);
    vad_live_init(&v, &c, -470);
    vad_live_feed(&v, p, n / VAD_FRAME * VAD_FRAME);
    CHECK(v.speech_ms < 100);
    free(p);

    // a long utterance in a reverberant room that trails off: 4 s at -24 dBFS, then 3 s at -32 dBFS, the gaps
    // between syllables only 8 dB under them. The tracked floor creeps up towards the gap level; it must not
    // overtake the soft end, or a translator (tr_vad) turn is sent mid-sentence
    n = 10000 * 16;
    p = clip(10000);
    fan(p, 16000, -45, 0);
    for (size_t i = 16000; i < 16000 + 7 * RATE; i++) {
        size_t ms = (i - 16000) * 1000 / RATE;
        double db = ms < 4000 ? -24 : -32;
        bool on = ms % 220 < 160;
        double env = on ? sin(3.14159265 * (double)(ms % 220) / 160.0) : 0.0;
        p[i] = clamp16(noise(-45) + (on ? env * 1.4 * noise(db) : noise(db - 8)));
    }
    fan(p + 16000 + 7 * RATE, n - 16000 - 7 * RATE, -45, 16000 + 7 * RATE);
    vad_live_init(&v, &c, -470);
    stop_ms = 0;
    for (size_t o = 0; o + VAD_FRAME <= n; o += VAD_FRAME) {
        vad_live_feed(&v, p + o, VAD_FRAME);
        if (!stop_ms && v.speech_ms >= 400 && v.silence_ms >= 1200) {
            stop_ms = (uint32_t)((o + VAD_FRAME) * 1000 / RATE);
        }
    }
    printf("    trailing-off speech: auto-stop at %u ms (speech ends at 8000 ms)\n", (unsigned)stop_ms);
    CHECK(stop_ms >= 9000 && stop_ms <= 9700);      // 1.2 s of silence after the end, not during the soft part
    // the same without an ambient hint (injected audio): the slow creep still keeps the soft end
    vad_live_init(&v, &c, VAD_NO_HINT);
    stop_ms = 0;
    for (size_t o = 0; o + VAD_FRAME <= n; o += VAD_FRAME) {
        vad_live_feed(&v, p + o, VAD_FRAME);
        if (!stop_ms && v.speech_ms >= 400 && v.silence_ms >= 1200) {
            stop_ms = (uint32_t)((o + VAD_FRAME) * 1000 / RATE);
        }
    }
    printf("    trailing-off speech, no hint: auto-stop at %u ms\n", (unsigned)stop_ms);
    CHECK(stop_ms >= 8000);
    free(p);
}

int main(void)
{
    t_gate();
    t_live();
    printf("%d checks, %d failures\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
