// Binary transfers of the dev console: BEGIN/D/END blobs to the PC and chunked uploads
// with ACK windows from the PC (docs/architecture.md §7.2), plus base64 and CRC checks.
#include "dc_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"

static const char *TAG = "devcon";

#define UPLOAD_IDLE_MS 5000

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t dc_b64_encode(const uint8_t *in, size_t n, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? B64[v & 63] : '=';
    }
    return o;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int dc_b64_decode(const char *in, size_t n, uint8_t *out, size_t out_max)
{
    if (n % 4 != 0) {
        return -1;
    }
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4) {
        int a = b64_val(in[i]), b = b64_val(in[i + 1]);
        bool last = i + 4 == n;
        int c = (last && in[i + 2] == '=') ? -2 : b64_val(in[i + 2]);
        int d = (last && in[i + 3] == '=') ? -2 : b64_val(in[i + 3]);
        if (a < 0 || b < 0 || c == -1 || d == -1 || (c == -2 && d != -2)) {
            return -1;
        }
        uint32_t v = ((uint32_t)a << 18) | ((uint32_t)b << 12) | ((uint32_t)(c < 0 ? 0 : c) << 6) | (uint32_t)(d < 0 ? 0 : d);
        size_t k = c < 0 ? 1 : d < 0 ? 2 : 3;
        if (o + k > out_max) {
            return -1;
        }
        out[o++] = (uint8_t)(v >> 16);
        if (k > 1) out[o++] = (uint8_t)(v >> 8);
        if (k > 2) out[o++] = (uint8_t)v;
    }
    return (int)o;
}

bool dc_codec_selfcheck(void)
{
    static const char digits[] = "123456789";
    bool ok = esp_rom_crc32_le(0, (const uint8_t *)digits, 9) == 0xCBF43926u;
    // chained CRC must equal the one-shot CRC (uploads are checked chunk by chunk)
    ok &= esp_rom_crc32_le(esp_rom_crc32_le(0, (const uint8_t *)digits, 4), (const uint8_t *)digits + 4, 5) ==
          0xCBF43926u;
    char enc[12];
    uint8_t dec[8];
    ok &= dc_b64_encode((const uint8_t *)"foobar", 6, enc) == 8 && !memcmp(enc, "Zm9vYmFy", 8);
    ok &= dc_b64_encode((const uint8_t *)"fo", 2, enc) == 4 && !memcmp(enc, "Zm8=", 4);
    ok &= dc_b64_decode("Zm9vYg==", 8, dec, sizeof(dec)) == 4 && !memcmp(dec, "foob", 4);
    ok &= dc_b64_decode("Zm9v!mFy", 8, dec, sizeof(dec)) == -1;
    if (!ok) {
        ESP_LOGE(TAG, "CRC/base64 self-check FAILED");
    }
    return ok;
}

int dc_blob(const char *kind, const void *data, size_t len, const char *meta)
{
    const uint8_t *p = data;
    uint16_t id = dc_cur_id();
    uint32_t crc = esp_rom_crc32_le(0, p, len);
    char *b64 = heap_caps_malloc(DC_BLOB_CHUNK / 3 * 4 + 4, MALLOC_CAP_SPIRAM);
    if (!b64) {
        return dc_fail("no memory for blob");
    }
    if (dc_line_begin(true)) {
        dc_line_printf("@%u BEGIN %s {", id, kind);
        if (meta && meta[0]) {
            dc_line_puts(meta);
            dc_line_put(",", 1);
        }
        dc_line_printf("\"bytes\":%u,\"crc32\":\"%08lx\"}", (unsigned)len, (unsigned long)crc);
        dc_line_end();
    }
    uint32_t seq = 0;
    for (size_t off = 0; off < len; off += DC_BLOB_CHUNK, seq++) {
        size_t n = len - off < DC_BLOB_CHUNK ? len - off : DC_BLOB_CHUNK;
        size_t m = dc_b64_encode(p + off, n, b64);
        if (dc_line_begin(true)) {
            dc_line_printf("@%u D %u ", id, (unsigned)seq);
            dc_line_put(b64, m);
            dc_line_end();
        }
    }
    free(b64);
    if (dc_line_begin(true)) {
        dc_line_printf("@%u END {\"bytes\":%u,\"crc32\":\"%08lx\",\"lines\":%u}", id, (unsigned)len,
                       (unsigned long)crc, (unsigned)seq);
        dc_line_end();
    }
    return 0;
}

static void upload_reply(const char *what, const char *fmt_json)
{
    if (dc_line_begin(true)) {
        dc_line_printf("@%u %s %s", dc_cur_id(), what, fmt_json);
        dc_line_end();
    }
}

esp_err_t dc_upload(size_t bytes, uint32_t crc32, dc_sink_fn sink, void *ctx)
{
    uint8_t *buf = heap_caps_malloc(DC_UP_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);   // flash writes read it
    if (!buf) {
        dc_fail("no memory");
        return ESP_ERR_NO_MEM;
    }
    char ready[48];
    snprintf(ready, sizeof(ready), "{\"chunk\":%u,\"window\":%u}", DC_UP_CHUNK, DC_UP_WINDOW);
    upload_reply("READY", ready);

    esp_err_t err = ESP_OK;
    size_t got = 0;
    uint32_t crc = 0, seq = 0;
    for (;;) {
        char *line;
        int n = dc_read_line(&line, UPLOAD_IDLE_MS);
        if (n == -1) {
            dc_fail("upload timeout after %u bytes", (unsigned)got);
            err = ESP_ERR_TIMEOUT;
            break;
        }
        if (n == -2) {
            dc_fail("upload line too long");
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (!strcmp(line, "END")) {
            if (got != bytes) {
                dc_fail("size mismatch: got %u of %u", (unsigned)got, (unsigned)bytes);
                err = ESP_ERR_INVALID_SIZE;
            } else if (crc != crc32) {
                dc_fail("crc mismatch: %08lx != %08lx", (unsigned long)crc, (unsigned long)crc32);
                err = ESP_ERR_INVALID_CRC;
            }
            break;
        }
        if (!strcmp(line, "ABORT")) {
            dc_fail("aborted by host");
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        char *end = NULL;
        unsigned long s = (line[0] == 'D' && line[1] == ' ') ? strtoul(line + 2, &end, 10) : 0;
        if (!end || *end != ' ' || s != seq) {
            dc_fail("unexpected line (want D %u)", (unsigned)seq);
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        const char *b = end + 1;
        int m = dc_b64_decode(b, strlen(b), buf, DC_UP_CHUNK);
        if (m <= 0) {
            dc_fail("bad base64 in D %u", (unsigned)seq);
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        if (got + (size_t)m > bytes) {
            dc_fail("more data than announced");
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        err = sink(ctx, buf, (size_t)m, got);
        if (err != ESP_OK) {
            dc_fail("write failed at %u: %s", (unsigned)got, esp_err_to_name(err));
            break;
        }
        crc = esp_rom_crc32_le(crc, buf, (size_t)m);
        got += (size_t)m;
        if (++seq % DC_UP_WINDOW == 0) {
            char ack[16];
            snprintf(ack, sizeof(ack), "%u", (unsigned)(seq - 1));
            upload_reply("ACK", ack);
        }
    }
    free(buf);
    return err;
}
