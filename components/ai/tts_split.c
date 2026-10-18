// TTS text preparation: sanitiser (protocol §5.1) and sentence splitting / packing (§5.4).
// Lengths are counted in Unicode code points; UTF-8 sequences are never split.
#include "ai_priv.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_CP        1500     // TTS cap per turn (protocol §5.3 rule 9)
#define FIRST_MAX     200
#define FIRST_SHORT   40
#define LATER_MAX     400
#define LATER_EMIT    150
#define MIN_PIECE     20
#define CUT_FROM      150

static bool is_emoji(uint32_t c)
{
    return (c >= 0x1F000 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF) || c == 0xFE0F || c == 0x200D;
}

static bool is_ws(uint32_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0xA0;
}

size_t tts_sanitize(const char *in, char *out, size_t len)
{
    if (!len) {
        return 0;
    }
    size_t o = 0;
    bool pend_space = false, pend_nl = false;
    const char *p = in ? in : "";
    while (*p) {
        bool tok_start = p == in || strchr(" \t\r\n(<\"'", p[-1]);
        if (tok_start && (!strncasecmp(p, "http://", 7) || !strncasecmp(p, "https://", 8) ||
                          !strncasecmp(p, "www.", 4))) {
            while (*p && !strchr(" \t\r\n", *p)) {
                p++;          // URLs are deleted
            }
            continue;
        }
        const char *q = p;
        uint32_t c = utf8_next(&q);
        if (c == '*' || c == '_' || c == '#' || c == '`' || c == '~' || c == '>' || c == '|' || c == 0xFFFD ||
            is_emoji(c)) {
            p = q;
            continue;
        }
        if (c == '\n') {
            pend_nl = true;
            p = q;
            continue;
        }
        if (is_ws(c)) {
            pend_space = true;
            p = q;
            continue;
        }
        size_t n = (size_t)(q - p);
        if (o && (pend_nl || pend_space) && o + 1 < len) {
            out[o++] = pend_nl ? '\n' : ' ';
        }
        pend_nl = pend_space = false;
        if (o + n >= len) {
            break;
        }
        memcpy(out + o, p, n);
        o += n;
        p = q;
    }
    out[o] = 0;
    return o;
}

// ---- splitting --------------------------------------------------------------------------------

typedef struct {
    uint32_t *cp;              // code points
    size_t *off;               // byte offset of each code point (off[n] = text length)
    size_t n;
    const char *text;
} doc_t;

typedef struct {
    size_t a, b;               // code-point span [a, b)
} span_t;

static bool is_digit(uint32_t c)
{
    return c >= '0' && c <= '9';
}

static bool is_letter(uint32_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) ||
           (c >= 0x370 && c <= 0x52F) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3040 && c <= 0x30FF) ||
           (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0x600 && c <= 0x6FF);
}

static bool is_end_punct(uint32_t c)
{
    return c == '.' || c == '!' || c == '?' || c == 0x2026;
}

static bool is_cjk_end(uint32_t c)
{
    return c == 0x3002 || c == 0xFF01 || c == 0xFF1F;
}

static bool is_closer(uint32_t c)
{
    return c == 0xBB || c == '"' || c == '\'' || c == ')';
}

static uint32_t lower_cp(uint32_t c)
{
    if (c >= 'A' && c <= 'Z') {
        return c + 32;
    }
    if (c >= 0x410 && c <= 0x42F) {
        return c + 32;         // Cyrillic А-Я
    }
    if (c == 0x401) {
        return 0x451;          // Ё
    }
    return c;
}

// Token before index i (back to whitespace), lowercased, opening punctuation stripped.
static bool is_abbreviation(const doc_t *d, size_t start, size_t i)
{
    static const char *const ABBR[] = {
        "т.е", "т.д", "т.п", "т.к", "др", "пр", "г", "гг", "ул", "д", "им", "см", "стр", "руб", "коп", "тыс",
        "млн", "млрд", "н.э", "mr", "mrs", "ms", "dr", "st", "vs", "etc", "e.g", "i.e", "no",
    };
    size_t t = i;
    while (t > start && !is_ws(d->cp[t - 1])) {
        t--;
    }
    while (t < i && (d->cp[t] == '(' || d->cp[t] == 0xAB || d->cp[t] == '"' || d->cp[t] == '\'')) {
        t++;
    }
    size_t n = i - t;
    if (n == 0) {
        return false;
    }
    if (n == 1 && is_letter(d->cp[t])) {
        return true;           // initials: "А. С. Пушкин"
    }
    if (n > 8) {
        return false;
    }
    char tok[40];
    size_t o = 0;
    for (size_t k = t; k < i && o + 4 < sizeof(tok); k++) {
        o += utf8_put(lower_cp(d->cp[k]), tok + o);
    }
    tok[o] = 0;
    for (size_t k = 0; k < sizeof(ABBR) / sizeof(ABBR[0]); k++) {
        if (!strcmp(tok, ABBR[k])) {
            return true;
        }
    }
    return false;
}

// Sentence spans (trimmed), in order.
static size_t sentences(const doc_t *d, span_t *out, size_t max)
{
    size_t n = 0, start = 0, i = 0;
    while (i < d->n && n < max) {
        uint32_t c = d->cp[i];
        size_t end = 0, next = 0;
        bool cut = false;
        if (c == '\n') {
            end = i;
            next = i;
            while (next < d->n && d->cp[next] == '\n') {
                next++;
            }
            cut = true;
        } else if (is_cjk_end(c)) {
            next = i;
            while (next < d->n && is_cjk_end(d->cp[next])) {
                next++;
            }
            end = next;
            cut = true;
        } else if (is_end_punct(c)) {
            size_t j = i;
            while (j < d->n && is_end_punct(d->cp[j])) {
                j++;
            }
            size_t k = j;
            while (k < d->n && is_closer(d->cp[k])) {
                k++;
            }
            if (k == d->n || is_ws(d->cp[k])) {
                bool single_dot = j == i + 1 && c == '.';
                bool decimal = single_dot && i > 0 && is_digit(d->cp[i - 1]) && j < d->n && is_digit(d->cp[j]);
                if (!decimal && !(single_dot && is_abbreviation(d, start, i))) {
                    end = k;
                    next = k;
                    cut = true;
                }
            }
            if (!cut) {
                i = j;
                continue;
            }
        }
        if (cut) {
            size_t a = start, b = end;
            while (a < b && is_ws(d->cp[a])) a++;
            while (b > a && is_ws(d->cp[b - 1])) b--;
            if (b > a) {
                out[n].a = a;
                out[n].b = b;
                n++;
            }
            start = next;
            i = next;
            continue;
        }
        i++;
    }
    if (n < max) {
        size_t a = start, b = d->n;
        while (a < b && is_ws(d->cp[a])) a++;
        while (b > a && is_ws(d->cp[b - 1])) b--;
        if (b > a) {
            out[n].a = a;
            out[n].b = b;
            n++;
        }
    }
    return n;
}

static bool is_cjk_pause(uint32_t c)
{
    return c == 0xFF0C || c == 0x3001 || c == 0xFF1B || c == 0xFF1A;     // ， 、 ； ：
}

// Cuts [a, b) longer than limit at the last , ; : — – (or a CJK comma, which has no space after it) in
// [a+150, a+limit], else the last space.
static size_t cut_point(const doc_t *d, size_t a, size_t b, size_t limit)
{
    size_t hi = a + limit < b ? a + limit : b;
    size_t lo = a + (CUT_FROM < limit ? CUT_FROM : limit / 2);
    for (size_t c = hi; c > lo; c--) {
        uint32_t x = d->cp[c - 1];
        if (x == ',' || x == ';' || x == ':' || is_cjk_pause(x)) {
            return c;
        }
        if ((x == 0x2014 || x == 0x2013) && c >= 2 && is_ws(d->cp[c - 2]) && c < b && is_ws(d->cp[c])) {
            return c;          // after " — "
        }
    }
    for (size_t c = hi; c > a + 1; c--) {
        if (is_ws(d->cp[c - 1])) {
            return c - 1;
        }
    }
    return hi;
}

static size_t skip_ws(const doc_t *d, size_t i, size_t b)
{
    while (i < b && is_ws(d->cp[i])) {
        i++;
    }
    return i;
}

static size_t span_len(span_t s)
{
    return s.b - s.a;
}

static bool has_word(const doc_t *d, span_t s)
{
    for (size_t i = s.a; i < s.b; i++) {
        if (is_letter(d->cp[i]) || is_digit(d->cp[i])) {
            return true;
        }
    }
    return false;
}

static char *span_str(const doc_t *d, span_t s)
{
    size_t from = d->off[s.a], to = d->off[s.b];
    char *r = ai_strndup(d->text + from, to - from);
    for (char *p = r; p && *p; p++) {
        if (*p == '\n') {
            *p = ' ';
        }
    }
    return r;
}

int tts_split(const char *text, char ***chunks)
{
    *chunks = NULL;
    size_t cap = utf8_cp_len(text);
    if (cap == 0) {
        return 0;
    }
    bool capped = cap > MAX_CP;
    if (capped) {
        cap = MAX_CP;
    }
    doc_t d = { .text = text };
    d.cp = ai_malloc((cap + 1) * sizeof(uint32_t));
    d.off = ai_malloc((cap + 1) * sizeof(size_t));
    span_t *sent = ai_malloc(cap * sizeof(span_t));
    span_t *out = ai_malloc((cap + 1) * sizeof(span_t));
    int n_out = 0;
    if (!d.cp || !d.off || !sent || !out) {
        goto fail;
    }
    const char *p = text;
    while (d.n < cap) {
        d.off[d.n] = (size_t)(p - text);
        uint32_t c = utf8_next(&p);
        if (!c) {
            break;
        }
        d.cp[d.n++] = c;
    }
    d.off[d.n] = (size_t)(p - text);
    size_t ns = sentences(&d, sent, cap);
    if (capped && ns > 1 && sent[ns - 1].b == d.n) {
        ns--;                  // the cap cut the last sentence: drop the fragment
    }

    // Packing: chunk 1 low-latency (<= 200), later chunks greedy up to 400, emitted at >= 150.
    size_t i = 0;
    span_t cur = { 0, 0 };
    bool have = false, first = true;
    while (i < ns) {
        span_t s = sent[i];
        size_t limit = first ? FIRST_MAX : LATER_MAX;
        if (span_len(s) > limit) {
            if (have) {
                out[n_out++] = cur;
                have = false;
                first = false;
                limit = LATER_MAX;
            }
            size_t a = s.a;
            while (s.b - a > limit) {
                size_t c = cut_point(&d, a, s.b, limit);
                out[n_out++] = (span_t){ a, c };
                a = skip_ws(&d, c, s.b);
                first = false;
                limit = LATER_MAX;
            }
            cur = (span_t){ a, s.b };
            have = a < s.b;
            i++;
            if (have && span_len(cur) >= LATER_EMIT) {
                out[n_out++] = cur;
                have = false;
            }
            continue;
        }
        if (first) {
            cur = s;
            i++;
            if (span_len(cur) < FIRST_SHORT) {
                while (i < ns && sent[i].b - cur.a <= FIRST_MAX) {
                    cur.b = sent[i].b;
                    i++;
                }
            }
            out[n_out++] = cur;
            first = false;
            continue;
        }
        if (have && s.b - cur.a > LATER_MAX) {
            out[n_out++] = cur;
            have = false;
        }
        if (!have) {
            cur = s;
            have = true;
        } else {
            cur.b = s.b;
        }
        i++;
        if (span_len(cur) >= LATER_EMIT) {
            out[n_out++] = cur;
            have = false;
        }
    }
    if (have) {
        out[n_out++] = cur;
    }
    // Never a piece under 20 characters: merge it into the previous one (the first into the next).
    int k = 0;
    for (int j = 0; j < n_out; j++) {
        if (k > 0 && span_len(out[j]) < MIN_PIECE) {
            out[k - 1].b = out[j].b;
        } else {
            out[k++] = out[j];
        }
    }
    n_out = k;
    if (n_out > 1 && span_len(out[0]) < MIN_PIECE) {
        out[1].a = out[0].a;
        memmove(out, out + 1, (size_t)(n_out - 1) * sizeof(span_t));
        n_out--;
    }
    char **list = ai_calloc((size_t)(n_out ? n_out : 1), sizeof(char *));
    if (!list) {
        goto fail;
    }
    int m = 0;
    for (int j = 0; j < n_out; j++) {
        if (!has_word(&d, out[j])) {
            continue;          // empty or punctuation only
        }
        char *s = span_str(&d, out[j]);
        if (s) {
            list[m++] = s;
        }
    }
    free(d.cp);
    free(d.off);
    free(sent);
    free(out);
    *chunks = list;
    return m;

fail:
    free(d.cp);
    free(d.off);
    free(sent);
    free(out);
    return -1;
}

void tts_split_free(char **chunks, int n)
{
    for (int i = 0; chunks && i < n; i++) {
        free(chunks[i]);
    }
    free(chunks);
}
