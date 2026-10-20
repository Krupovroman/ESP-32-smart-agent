// Translator output check of protocol §3: the translation must be written in the target language's script and must
// not repeat the utterance. Text statistics only (no extra request); a failed check costs one retry in ai_client.c.
#include "ai_priv.h"

#include <stdlib.h>
#include <string.h>

uint32_t ai_script_of(uint32_t c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) ||
        (c >= 0x250 && c <= 0x2AF) || (c >= 0x1E00 && c <= 0x1EFF) || (c >= 0xFF21 && c <= 0xFF3A) ||
        (c >= 0xFF41 && c <= 0xFF5A)) {
        return AI_SC_LATIN;
    }
    if ((c >= 0x370 && c <= 0x3FF && c != 0x37E && c != 0x387) || (c >= 0x1F00 && c <= 0x1FFF)) {
        return AI_SC_GREEK;
    }
    if ((c >= 0x400 && c <= 0x52F && (c < 0x483 || c > 0x489)) || (c >= 0x1C80 && c <= 0x1C8F) ||
        (c >= 0xA640 && c <= 0xA69F)) {
        return AI_SC_CYRILLIC;
    }
    if (c >= 0x531 && c <= 0x58F) {
        return AI_SC_OTHER;                    // Armenian
    }
    if (c >= 0x5D0 && c <= 0x5F2) {
        return AI_SC_HEBREW;                   // letters only: points and punctuation are not counted
    }
    if ((c >= 0x620 && c <= 0x64A) || (c >= 0x66E && c <= 0x6D3) || (c >= 0x6FA && c <= 0x6FF) ||
        (c >= 0x750 && c <= 0x77F) || (c >= 0x8A0 && c <= 0x8C9) || (c >= 0xFB50 && c <= 0xFDFF) ||
        (c >= 0xFE70 && c <= 0xFEFC)) {
        return AI_SC_ARABIC;
    }
    if (c >= 0x900 && c <= 0x97F && (c < 0x964 || c > 0x96F)) {
        return AI_SC_DEVANAGARI;
    }
    if (c >= 0xE01 && c <= 0xE4E) {
        return AI_SC_THAI;
    }
    if ((c >= 0x1100 && c <= 0x11FF) || (c >= 0x3131 && c <= 0x318E) || (c >= 0xAC00 && c <= 0xD7A3)) {
        return AI_SC_HANGUL;
    }
    if ((c >= 0x3041 && c <= 0x30FA) || (c >= 0x30FC && c <= 0x30FF) || (c >= 0x31F0 && c <= 0x31FF) ||
        (c >= 0xFF66 && c <= 0xFF9D)) {
        return AI_SC_KANA;                     // the middle dot U+30FB is punctuation
    }
    if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0xF900 && c <= 0xFAFF) ||
        (c >= 0x20000 && c <= 0x2FFFF) || c == 0x3005) {
        return AI_SC_HAN;
    }
    if (c >= 0x980 && c <= 0x1CFF) {
        return AI_SC_OTHER;                    // the other Indic, South-East Asian, Georgian, Ethiopic ... letters
    }
    return 0;
}

// Upper case of the cased scripts (Latin, Latin-1, Greek, Cyrillic); lower() maps it down.
static bool is_upper(uint32_t c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7) || (c >= 0x391 && c <= 0x3A9) ||
           (c >= 0x400 && c <= 0x42F);
}

static uint32_t lower(uint32_t c)
{
    if (!is_upper(c)) {
        return c;
    }
    return c >= 0x400 && c <= 0x40F ? c + 80 : c + 32;
}

typedef struct {
    int letters;          // code points that are letters of some script
    int weight;           // letters for the script ratio: a CJK character counts 2 (it is a word or a syllable),
                          // a Latin name inside text of a non-Latin set 0 ("我在Google工作", "Открой YouTube")
    int weight_in;        // ... of the scripts asked for
    int cjk;              // Han, kana and hangul (no spaces between words)
    int kana;
    int words;            // whitespace-separated tokens with a letter
    int cap_words;        // ... starting with an upper-case letter
} stats_t;

// One run of Latin letters and digits (with - . _ @ ' + inside), counted when it ends.
typedef struct {
    int letters;
    bool active, at_start, first_upper, inner_upper, digit_or_at;
} run_t;

static void run_end(run_t *r, uint32_t set, stats_t *st)
{
    if (r->active && r->letters) {
        // in a non-Latin target a brand, model or place name stays in Latin: mixed case (iPhone, USB), digits or
        // @ (5G, a@b.c), or a capital that does not start a sentence (купить в Starbucks)
        bool name = !(set & AI_SC_LATIN) && (r->inner_upper || r->digit_or_at || (r->first_upper && !r->at_start));
        if (!name) {
            st->weight += r->letters;
            st->weight_in += (set & AI_SC_LATIN) ? r->letters : 0;
        }
    }
    memset(r, 0, sizeof(*r));
}

static bool is_digit(uint32_t c)
{
    return c >= '0' && c <= '9';
}

static void stats(const char *s, uint32_t set, stats_t *st)
{
    memset(st, 0, sizeof(*st));
    bool word_has = false, sentence_start = true;
    run_t run = { 0 };
    for (const char *p = s ? s : "";;) {
        uint32_t c = utf8_next(&p);
        uint32_t sc = c ? ai_script_of(c) : 0;
        bool latin = sc == AI_SC_LATIN;
        if (run.active && !latin && !is_digit(c)) {
            bool joins = false;
            if (c && c < 0x80 && strchr("-._@'+", (int)c)) {
                const char *q = p;
                uint32_t nx = utf8_next(&q);
                joins = ai_script_of(nx) == AI_SC_LATIN || is_digit(nx);
            }
            if (joins) {
                run.digit_or_at |= c == '@';
                continue;
            }
            run_end(&run, set, st);
        }
        if (latin || is_digit(c)) {
            if (!run.active) {
                run.active = true;
                run.at_start = sentence_start;
                run.first_upper = latin && is_upper(c);
            } else if (latin && is_upper(c)) {
                run.inner_upper = true;
            }
            run.digit_or_at |= is_digit(c);
            run.letters += latin;
        }
        bool ws = c == 0 || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xA0 || c == 0x3000;
        if (ws) {
            st->words += word_has;
            word_has = false;
            if (!c) {
                break;
            }
            continue;
        }
        if (c == '.' || c == '!' || c == '?' || c == 0x2026 || c == 0x3002 || c == 0xFF01 || c == 0xFF1F) {
            sentence_start = true;
        } else if (sc || is_digit(c)) {
            sentence_start = false;
        }
        if (!sc) {
            continue;                          // digits, punctuation, symbols, combining marks
        }
        st->letters++;
        bool cjk = (sc & (AI_SC_HAN | AI_SC_KANA | AI_SC_HANGUL)) != 0;
        st->cjk += cjk;
        st->kana += sc == AI_SC_KANA;
        if (!latin) {
            st->weight += cjk ? 2 : 1;
            st->weight_in += (sc & set) ? (cjk ? 2 : 1) : 0;
        }
        if (!word_has) {
            st->cap_words += is_upper(c);      // the first letter of the token decides
        }
        word_has = true;
    }
}

const char *ai_tr_src_by_script(const char *transcript, const char *a, const char *b)
{
    uint32_t sa = ai_lang_scripts(a), sb = ai_lang_scripts(b);
    if (!sa || !sb || (sa & sb)) {
        return NULL;
    }
    stats_t in_a, in_b;
    stats(transcript, sa, &in_a);
    stats(transcript, sb, &in_b);
    if (in_a.letters < 2) {
        return NULL;
    }
    // a clear majority of the letters: a Latin brand name inside a Russian sentence does not flip it
    if (in_a.weight && in_a.weight_in * 4 >= in_a.weight * 3) {
        return a;
    }
    if (in_b.weight && in_b.weight_in * 4 >= in_b.weight * 3) {
        return b;
    }
    return NULL;
}

// Letters and digits, lower-cased (PSRAM); NULL without memory.
static char *normalize(const char *s)
{
    size_t n = strlen(s ? s : "");
    char *o = ai_malloc(n + 1);
    if (!o) {
        return NULL;
    }
    size_t k = 0;
    for (const char *p = s ? s : "";;) {
        uint32_t c = utf8_next(&p);
        if (!c) {
            break;
        }
        if (ai_script_of(c) || (c >= '0' && c <= '9')) {
            k += utf8_put(lower(c), o + k);    // a lower-case letter never takes more bytes in these ranges
        }
    }
    o[k] = 0;
    return o;
}

const char *ai_tr_check(const char *transcript, const char *translation, const char *tgt, const char *other)
{
    uint32_t set = ai_lang_scripts(tgt);
    stats_t t;
    stats(translation, set, &t);
    if (t.letters < 2) {
        return NULL;                           // numbers, punctuation, a single letter: nothing to judge
    }
    char *nt = normalize(translation), *nu = normalize(transcript);
    bool copy = nt && nu && nt[0] && !strcmp(nt, nu);
    free(nt);
    free(nu);
    // a copy of two or more capitalised words is a name ("New York", "Google Maps"): kept as it is. Not in a pair
    // with German, which capitalises every noun ("Guten Morgen").
    bool de = !strcmp(tgt ? tgt : "", "de") || (other && !strcmp(other, "de"));
    bool names = copy && !de && t.words >= 2 && t.cap_words == t.words;
    if (set && t.weight_in * 2 < t.weight && !names) {
        return "script";
    }
    // Japanese is never written in Han alone beyond a word or two: that is Chinese (zh-ja answered in Chinese)
    if ((set & AI_SC_KANA) && !t.kana && t.cjk >= 5) {
        return "script";
    }
    // same-script pairs (en-de, ru-uk, zh-ja): one word may well be the same in both ("Taxi", "Hotel", "OK")
    if (copy && !names && ((t.words >= 2 && t.letters >= 4) || t.cjk >= 6)) {
        return "copy";
    }
    return NULL;
}
