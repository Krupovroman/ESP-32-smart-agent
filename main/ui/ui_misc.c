// Small helpers shared by the second-half screens (ui_scr.h): id shortening, the translator language table of
// protocol 4.2, the translator pair, the reference-counted rim comet.
#include "ui_internal.h"
#include "ui_rim.h"
#include "ui_scr.h"
#include "ui_str.h"
#include "ui_sym.h"
#include "ui_work.h"

#include <stdio.h>
#include <string.h>

void ui_short_model(const char *id, char *out, size_t len)
{
    const char *slash = id ? strchr(id, '/') : NULL;
    ui_str_copy(out, len, slash ? slash + 1 : (id ? id : ""));
}

void ui_short_voice(const char *voice, char *out, size_t len)
{
    // "ru-RU-Masha:MAI-Voice-2.1-Flash" -> "Masha"; flat ids ("Kore") stay as they are
    const char *v = voice ? voice : "";
    const char *colon = strchr(v, ':');
    size_t n = colon ? (size_t)(colon - v) : strlen(v);
    const char *start = v;
    for (const char *p = v; p < v + n; p++) {
        if (*p == '-') {
            start = p + 1;
        }
    }
    char tmp[64];
    size_t m = LV_MIN((size_t)(v + n - start), sizeof(tmp) - 1);
    memcpy(tmp, start, m);
    tmp[m] = 0;
    ui_str_copy(out, len, tmp);
}

void ui_url_host(const char *url, char *out, size_t len)
{
    const char *p = url ? strstr(url, "://") : NULL;
    p = p ? p + 3 : (url ? url : "");
    size_t n = strcspn(p, "/:");
    char tmp[96];
    n = LV_MIN(n, sizeof(tmp) - 1);
    memcpy(tmp, p, n);
    tmp[n] = 0;
    ui_str_copy(out, len, tmp);
}

// Protocol 4.2: code, Russian UI name, English UI name. Only scripts ui_16/20/28 can draw (Latin, Latin
// Extended-A, Cyrillic: ui.md 1.3): zh/ja/ko/ar stay out until a font or a romanised line covers them, because the
// sanitiser would erase their transcripts and translations
static const struct {
    const char *code, *ru, *en;
} s_langs[] = {
    { "ru", "Русский", "Russian" }, { "en", "Английский", "English" }, { "de", "Немецкий", "German" },
    { "fr", "Французский", "French" }, { "es", "Испанский", "Spanish" }, { "it", "Итальянский", "Italian" },
    { "pt", "Португальский", "Portuguese" }, { "pl", "Польский", "Polish" }, { "tr", "Турецкий", "Turkish" },
    { "uk", "Украинский", "Ukrainian" },
};

int ui_lang_count(void)
{
    return (int)(sizeof(s_langs) / sizeof(s_langs[0]));
}

const char *ui_lang_code(int i)
{
    return i >= 0 && i < ui_lang_count() ? s_langs[i].code : "";
}

static bool lang_offered(const char *code)
{
    for (int i = 0; i < ui_lang_count(); i++) {
        if (code && !strcmp(code, s_langs[i].code)) {
            return true;
        }
    }
    return false;
}

bool ui_lang_fix(char *a, char *b)
{
    // a stored code the table no longer offers falls back to en (ru when the other slot already is en)
    bool changed = false;
    if (!lang_offered(a)) {
        snprintf(a, 4, "%s", strcmp(b, "en") ? "en" : "ru");
        changed = true;
    }
    if (!lang_offered(b) || !strcmp(a, b)) {
        snprintf(b, 4, "%s", strcmp(a, "en") ? "en" : "ru");
        changed = true;
    }
    return changed;
}

const char *ui_lang_name(const char *code)
{
    for (int i = 0; i < ui_lang_count(); i++) {
        if (code && !strcmp(code, s_langs[i].code)) {
            return ui_str_lang() ? s_langs[i].en : s_langs[i].ru;
        }
    }
    return code ? code : "";
}

void ui_lang_pair(char *out, size_t len)
{
    // "Русский — Английский" when it fits a ui_16 subtitle (184 px), else "RU — EN"
    snprintf(out, len, "%s " TXT_EMDASH " %s", ui_lang_name(g_lang_a), ui_lang_name(g_lang_b));
    lv_point_t sz;
    lv_text_get_size(&sz, out, &ui_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (sz.x > 184) {
        char a[4] = { 0 }, b[4] = { 0 };
        for (int i = 0; i < 3 && g_lang_a[i]; i++) {
            a[i] = (char)(g_lang_a[i] >= 'a' && g_lang_a[i] <= 'z' ? g_lang_a[i] - 32 : g_lang_a[i]);
        }
        for (int i = 0; i < 3 && g_lang_b[i]; i++) {
            b[i] = (char)(g_lang_b[i] >= 'a' && g_lang_b[i] <= 'z' ? g_lang_b[i] - 32 : g_lang_b[i]);
        }
        snprintf(out, len, "%s " TXT_EMDASH " %s", a, b);
    }
}

void ui_set_langs(const char *a, const char *b)
{
    if (!a || !b || !a[0] || !b[0] || !strcmp(a, b)) {
        return;     // the UI enforces a != b
    }
    snprintf(g_lang_a, sizeof(g_lang_a), "%.3s", a);
    snprintf(g_lang_b, sizeof(g_lang_b), "%.3s", b);
    scr_home_langs_changed();
    ui_save_str("tr_lang_a", g_lang_a);
    ui_save_str("tr_lang_b", g_lang_b);
    ui_emit(UI_IN_LANGS, 0);
}

static int s_busy;

void ui_busy(bool on)
{
    s_busy += on ? 1 : -1;
    if (s_busy < 0) {
        s_busy = 0;
    }
    ui_rim_busy(s_busy > 0);
}
