// Audio commands: mic probe/rec/dump/level/fmt/rate/load/src, spk tone/vol/stat.
#include "dc_internal.h"
#include "audio_in.h"
#include "audio_out.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MIC_LOAD_MAX (44 + 4096 + AUDIO_IN_RATE * 2 * (AUDIO_IN_MAX_MS / 1000))   // header slack + 30 s

static void print_clip(const audio_clip_t *c)
{
    printf("{\"ms\":%u,\"samples\":%u,\"rate\":%u,\"peak\":%d,\"rms_dbfs\":%.1f,\"src\":\"%s\"}\n", (unsigned)c->ms,
           (unsigned)c->samples, (unsigned)c->rate, c->peak, c->rms_dbfs_x10 / 10.0,
           audio_in_get_source() == AUDIO_SRC_INJECT ? "inject" : "mic");
}

typedef struct {
    uint8_t *buf;
} load_ctx_t;

static esp_err_t load_sink(void *ctx, const uint8_t *data, size_t len, size_t offset)
{
    memcpy(((load_ctx_t *)ctx)->buf + offset, data, len);
    return ESP_OK;
}

// mic load bytes=N crc32=X: upload a 16 kHz mono s16 WAV into the injection buffer.
static int mic_load(int argc, char **argv)
{
    const char *b = dc_arg(argc, argv, "bytes"), *c = dc_arg(argc, argv, "crc32");
    if (!b || !c) {
        return dc_fail("usage: mic load bytes=<n> crc32=<hex>");
    }
    size_t bytes = strtoul(b, NULL, 10);
    uint32_t crc = strtoul(c, NULL, 16);
    if (bytes < 46 || bytes > MIC_LOAD_MAX) {
        return dc_fail("size %u outside 46..%u", (unsigned)bytes, (unsigned)MIC_LOAD_MAX);
    }
    load_ctx_t ctx = { .buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM) };
    if (!ctx.buf) {
        return dc_fail("no memory for %u bytes", (unsigned)bytes);
    }
    esp_err_t e = dc_upload(bytes, crc, load_sink, &ctx);
    if (e == ESP_OK) {
        e = audio_in_inject_load(ctx.buf, bytes);
        if (e != ESP_OK) {
            dc_fail("not a 16 kHz mono s16 WAV (%s)", esp_err_to_name(e));
        }
    }
    free(ctx.buf);
    if (e != ESP_OK) {
        return e;
    }
    size_t n = audio_in_inject_samples();
    printf("{\"samples\":%u,\"ms\":%u,\"bytes\":%u}\n", (unsigned)n, (unsigned)(n * 1000 / AUDIO_IN_RATE),
           (unsigned)bytes);
    return 0;
}

// mic src mic|inject [fast]
static int mic_src(int argc, char **argv)
{
    if (argc > 2) {
        bool inject = !strcmp(argv[2], "inject");
        if (!inject && strcmp(argv[2], "mic")) {
            return dc_fail("usage: mic src mic|inject [fast]");
        }
        bool fast = argc > 3 && !strcmp(argv[3], "fast");
        esp_err_t e = audio_in_set_source(inject ? AUDIO_SRC_INJECT : AUDIO_SRC_MIC, !fast);
        if (e != ESP_OK) {
            dc_fail("%s", inject ? "load a WAV first (mic load) at 16 kHz capture rate" : "cannot switch");
            return e;
        }
    }
    bool inj = audio_in_get_source() == AUDIO_SRC_INJECT;
    printf("{\"src\":\"%s\",\"realtime\":%s,\"inject_samples\":%u}\n", inj ? "inject" : "mic",
           audio_in_inject_realtime() ? "true" : "false", (unsigned)audio_in_inject_samples());
    return 0;
}

static int cmd_mic(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "";
    if (!strcmp(sub, "probe")) {
        audio_probe_t pr;
        uint32_t ms = argc > 2 ? (uint32_t)atoi(argv[2]) : 600;
        if (audio_in_probe(ms, &pr) != ESP_OK) {
            return dc_fail("busy (recording, or source is inject)");
        }
        printf("{\"right\":{\"rms_dbfs\":%.1f,\"zero\":%.3f},\"left\":{\"rms_dbfs\":%.1f,\"zero\":%.3f}}\n",
               pr.rms_dbfs_x10[0] / 10.0, pr.zero_permille[0] / 1000.0, pr.rms_dbfs_x10[1] / 10.0,
               pr.zero_permille[1] / 1000.0);
        return 0;
    }
    if (!strcmp(sub, "rec")) {
        uint32_t ms = argc > 2 ? (uint32_t)atoi(argv[2]) : 3000;
        audio_in_start(ms);
        while (audio_in_recording()) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        const audio_clip_t *c = audio_in_last();
        if (!c) {
            return dc_fail("no clip");
        }
        print_clip(c);
        return 0;
    }
    if (!strcmp(sub, "dump")) {
        const audio_clip_t *c = audio_in_last();
        if (!c) {
            return dc_fail("no clip");
        }
        size_t len = 0;
        uint8_t *wav = audio_clip_to_wav(c, &len);
        if (!wav) {
            return dc_fail("no memory");
        }
        char meta[48];
        snprintf(meta, sizeof(meta), "\"rate\":%u,\"ch\":1,\"fmt\":\"s16le\"", (unsigned)c->rate);
        int rc = dc_blob("wav", wav, len, meta);
        free(wav);
        return rc;
    }
    if (!strcmp(sub, "rate")) {
        if (argc > 2) {
            uint32_t rate = (uint32_t)atoi(argv[2]);
            // the injected clip is 16 kHz: recording it at another rate would mislabel it
            if (rate != AUDIO_IN_RATE && audio_in_get_source() == AUDIO_SRC_INJECT) {
                return dc_fail("source is inject (16 kHz only): mic src mic first");
            }
            audio_in_set_rate(rate);
        }
        printf("{\"rate\":%u}\n", (unsigned)audio_in_get_rate());
        return 0;
    }
    if (!strcmp(sub, "level")) {
        printf("{\"level\":%d}\n", audio_in_level());
        return 0;
    }
    if (!strcmp(sub, "fmt") && argc > 3) {
        audio_in_set_format(!strcmp(argv[2], "right"), atoi(argv[3]));
        return 0;
    }
    if (!strcmp(sub, "load")) {
        return mic_load(argc, argv);
    }
    if (!strcmp(sub, "src")) {
        return mic_src(argc, argv);
    }
    return dc_fail("usage: mic probe [ms] | rec <ms> | dump | level | fmt right|left <shift> | rate [hz] | "
                   "load bytes=N crc32=X | src mic|inject [fast]");
}

static int cmd_spk(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "";
    if (!strcmp(sub, "tone")) {
        uint32_t hz = argc > 2 ? (uint32_t)atoi(argv[2]) : 440;
        uint32_t ms = argc > 3 ? (uint32_t)atoi(argv[3]) : 500;
        uint8_t amp = argc > 4 ? (uint8_t)atoi(argv[4]) : 50;
        audio_out_tone(hz, ms, amp);
        return 0;
    }
    if (!strcmp(sub, "vol") && argc > 2) {
        audio_out_set_volume((uint8_t)atoi(argv[2]));
        printf("{\"volume\":%u}\n", audio_out_get_volume());
        return 0;
    }
    if (!strcmp(sub, "stat")) {
        audio_out_stats_t st;
        audio_out_get_stats(&st);
        printf("{\"rate\":%u,\"played\":%llu,\"queued\":%u,\"write_errors\":%u,\"volume\":%u,\"busy\":%d}\n",
               (unsigned)st.rate, (unsigned long long)st.bytes_played, (unsigned)st.queued_bytes,
               (unsigned)st.write_errors, st.volume, audio_out_busy());
        return 0;
    }
    return dc_fail("usage: spk tone <hz> <ms> [amp] | vol <0-100> | stat");
}

void cmd_audio_register(void)
{
    devconsole_register("mic", "mic probe [ms] | rec <ms> | dump | level | fmt right|left <shift> | rate [hz] | "
                        "load bytes=N crc32=X | src mic|inject [fast]", cmd_mic);
    devconsole_register("spk", "spk tone <hz> <ms> [amp] | vol <0-100> | stat", cmd_spk);
}
