#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int status;
    const char *content_type;
    const char *hdr_k[4], *hdr_v[4];
    const uint8_t *body;
    size_t body_len;
    bool chunked;
    int eagain_before_headers;     // fetch_headers timeouts (each advances the clock by the socket timeout)
    bool fail_connect;
    int tls_err, tls_flags;
    bool fail_write_at_open;       // header write fails (dead kept-alive socket)
    bool fin_before_headers;       // connection closed before any response byte
    size_t cut_after;              // read fails after this many body bytes (0 = never)
    bool conn_close;
    int read_piece;                // max bytes per read (0 = 700)
    int eagain_every;              // a timeout tick every N reads (0 = never)
} mock_reply_t;

typedef struct {
    char method[8];
    char url[300];
    char auth[256];
    char accept[64];
    char ctype[64];
    char title[64];
    char *body;                    // NUL-terminated copy
    size_t len;
    int64_t content_length;
    bool reused;
} mock_req_t;

#define MOCK_MAX 24
extern mock_reply_t g_script[MOCK_MAX];
extern int g_script_n, g_script_pos;
extern mock_req_t g_reqs[MOCK_MAX];
extern int g_nreq, g_connects, g_len_mismatch;
extern bool g_net_up;
extern uint32_t g_out_rate, g_out_begins;
extern uint8_t g_out_ch;
extern size_t g_out_bytes, g_out_stops;
extern uint8_t *g_out_buf;
extern size_t g_out_ring_cap, g_out_drain_bps, g_out_full_hits;   // speaker ring simulation (cap 0 = off)

void mock_reset(void);
void mock_push(const mock_reply_t *r);
void mock_push_json(int status, const char *json);
void host_set(const char *key, const char *val);
void host_settings_reset(void);
int host_fail(const char *file, int line, const char *what);
extern int g_fails, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) host_fail(__FILE__, __LINE__, #c); } while (0)
