#include "ui_str.h"
#include "ui_sym.h"
#include "ui_theme.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *ru, *en;
    uint8_t slot;
} str_def_t;

#define E(id, ru, en, slot) [id] = { ru, en, slot }

static const str_def_t s_tab[S__N] = {
    E(S_MODE_ASSISTANT, "Ассистент", "Assistant", SLOT_PILL),
    E(S_MODE_TRANSLATOR, "Переводчик", "Translator", SLOT_PILL),

    E(S_ST_LISTENING, "Слушаю" TXT_ELLIPSIS, "Listening" TXT_ELLIPSIS, SLOT_STATUS),
    E(S_ST_LISTENING_LANG, "Слушаю" TXT_ELLIPSIS " (%s)", "Listening" TXT_ELLIPSIS " (%s)", SLOT_STATUS),
    E(S_ST_LATCHED, "Нажмите " TXT_EMDASH " отправлю", "Tap to send", SLOT_STATUS),
    E(S_ST_CANCEL, "Отпустите " TXT_EMDASH " отмена", "Release to cancel", SLOT_STATUS),
    E(S_ST_LEFT_S, "Ещё %d с", "%d s left", SLOT_STATUS),
    E(S_ST_UPLOAD, "Отправляю %d %%", "Sending %d %%", SLOT_STATUS),
    E(S_ST_THINK, "Думаю" TXT_ELLIPSIS, "Thinking" TXT_ELLIPSIS, SLOT_STATUS),
    E(S_ST_THINK2, "Ещё немного" TXT_ELLIPSIS, "Almost there" TXT_ELLIPSIS, SLOT_STATUS),
    E(S_ST_THINK3, "Долго" TXT_ELLIPSIS " PWR " TXT_EMDASH " отмена", "Slow" TXT_ELLIPSIS " PWR cancels", SLOT_STATUS),
    E(S_ST_SPEAK_HINT, "Нажмите " TXT_EMDASH " стоп", "Tap to stop", SLOT_STATUS),
    E(S_ST_NOT_HEARD, "Не расслышал", "Didn't catch that", SLOT_STATUS),
    E(S_ST_TOO_SHORT, "Слишком коротко", "Too short", SLOT_STATUS),
    E(S_ST_CANCELLED, "Отменено", "Cancelled", SLOT_STATUS),
    E(S_ST_RETRY, "Повтор %d/2" TXT_ELLIPSIS, "Retry %d/2" TXT_ELLIPSIS, SLOT_STATUS),
    E(S_ST_BUSY_HINT, "PWR " TXT_EMDASH " отмена", "PWR cancels", SLOT_STATUS),

    E(S_HINT1, "Держите шар", "Hold the orb", SLOT_HINT),
    E(S_HINT2, "или BOOT", "or BOOT", SLOT_HINT),
    E(S_HINT1_TR, "Говорите", "Speak", SLOT_HINT),
    E(S_HINT2_TR, "%s или %s", "%s or %s", SLOT_HINT),

    E(S_TITLE_HISTORY, "История", "History", SLOT_TITLE),
    E(S_NEW_CHAT, "Новый разговор", "New chat", SLOT_BTN),
    E(S_DIVIDER, TXT_EMDASH " новый разговор " TXT_MIDDOT " %s " TXT_EMDASH,
      TXT_EMDASH " new conversation " TXT_MIDDOT " %s " TXT_EMDASH, SLOT_NONE),
    E(S_TTS_NOTE, "голос недоступен", "voice unavailable", SLOT_NONE),

    E(S_VOICE_OFF, "Голос выкл.", "Voice off", SLOT_TOAST),
    E(S_VOICE_ON, "Голос вкл.", "Voice on", SLOT_TOAST),
    E(S_FINISH_INPUT, "Сначала завершите ввод", "Finish typing first", SLOT_TOAST),
    E(S_USB_POWER, "Питание от USB", "Powered by USB", SLOT_TOAST),
    E(S_DONE, "Готово", "Done", SLOT_TOAST),
    E(S_NOT_YET, "Пока недоступно", "Not available yet", SLOT_TOAST),

    E(S_TOUCH_FMT, "x %d " TXT_MIDDOT " y %d " TXT_MIDDOT " %d Гц", "x %d " TXT_MIDDOT " y %d " TXT_MIDDOT " %d Hz",
      SLOT_TOAST),
    E(S_TOUCH_HELP, "Коснитесь экрана", "Touch the screen", SLOT_TOAST),

    // "Модель недоступна ›" / "Модель скоро отключат ›" of ui.md 5.6 exceed the 160 px fault pill in ui_20
    E(S_F_NO_WIFI, "Нет Wi-Fi", "No Wi-Fi", SLOT_FAULT),
    E(S_F_BAD_KEY, "Ключ не принят", "Key rejected", SLOT_FAULT),
    E(S_F_NO_CREDIT, "Нет средств", "No credit", SLOT_FAULT),
    E(S_F_LOW_CREDIT, "Мало средств", "Low credit", SLOT_FAULT),
    E(S_F_REGION, "Недоступно", "Unavailable", SLOT_FAULT),
    E(S_F_MODEL, "Нет модели", "No model", SLOT_FAULT),
    E(S_F_MODEL_EXP, "Модель истекает", "Model expiring", SLOT_FAULT),
    E(S_F_MIC, "Микрофон", "Microphone", SLOT_FAULT),

    E(S_C_WIFI_T, "Нет подключения к Wi-Fi", "Not connected to Wi-Fi", SLOT_CARD_T),
    E(S_C_WIFI_B, "Подключаюсь снова" TXT_ELLIPSIS, "Reconnecting" TXT_ELLIPSIS, SLOT_NONE),
    E(S_C_WIFI_A, "Настроить", "Set up", SLOT_BTN),
    E(S_C_KEY_T, "Ключ API не подошёл (401)", "API key rejected (401)", SLOT_CARD_T),
    E(S_C_KEY_A, "Заменить ключ", "Replace key", SLOT_BTN),
    E(S_C_CREDIT_T, "Закончились средства", "Out of credit", SLOT_CARD_T),
    E(S_C_CREDIT_B, "Пополните баланс на openrouter.ai", "Top up at openrouter.ai", SLOT_NONE),
    E(S_C_CREDIT_A, "Подробнее", "Details", SLOT_BTN),
    E(S_C_REGION_T, "Сервис недоступен", "Not available", SLOT_CARD_T),
    E(S_C_REGION_B, "для этого региона или аккаунта. Укажите другого провайдера",
      "in your region or for this account. Choose another provider", SLOT_NONE),
    E(S_C_REGION_A, "Провайдер", "Provider", SLOT_BTN),
    E(S_C_MODEL_T, "Модель недоступна", "Model unavailable", SLOT_CARD_T),
    E(S_C_MODEL_A, "Выбрать", "Choose", SLOT_BTN),
    E(S_C_RATE_T, "Слишком часто " TXT_EMDASH " подождите", "Too many requests " TXT_EMDASH " wait", SLOT_NONE),
    E(S_C_SERVER_T, "Сервер не ответил", "Server didn't respond", SLOT_CARD_T),
    E(S_C_RETRY_A, "Повторить", "Retry", SLOT_BTN),
    E(S_C_TLS_T, "Не удаётся связаться с сервером", "Can't reach the server", SLOT_NONE),
    E(S_C_DIAG_A, "Диагностика", "Diagnostics", SLOT_BTN),
    E(S_C_MIC_T, "Микрофон молчит", "Microphone is silent", SLOT_CARD_T),
    E(S_C_SPK_T, "Динамик не подключён", "No speaker connected", SLOT_CARD_T),
    E(S_C_SPK_B, "Отвечаю текстом", "Replying in text", SLOT_NONE),
    E(S_C_SPK_A, "Проверить звук", "Test sound", SLOT_BTN),
    E(S_C_ERROR_T, "Ошибка", "Error", SLOT_CARD_T),
};

static int s_lang;

const char *ui_s(ui_sid_t id)
{
    if ((unsigned)id >= S__N || !s_tab[id].ru) {
        return "?";
    }
    return s_lang ? s_tab[id].en : s_tab[id].ru;
}

void ui_str_set_lang(int lang) { s_lang = lang ? 1 : 0; }
int ui_str_lang(void) { return s_lang; }

// ---- UTF-8 helpers (local, so the sanitiser does not depend on LVGL's private text API)

static uint32_t utf8_next(const char *s, size_t *i)
{
    const uint8_t *p = (const uint8_t *)s + *i;
    uint32_t c = p[0];
    int n = 1;
    if (c >= 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        c = ((c & 0x07) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
        n = 4;
    } else if (c >= 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        c = ((c & 0x0F) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
        n = 3;
    } else if (c >= 0xC0 && (p[1] & 0xC0) == 0x80) {
        c = ((c & 0x1F) << 6) | (p[1] & 0x3Fu);
        n = 2;
    } else if (c >= 0x80) {
        c = 0xFFFD;   // stray continuation byte
    }
    *i += n;
    return c;
}

static size_t utf8_put(uint32_t c, char *out)
{
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xC0 | (c >> 6));
        out[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (c >> 18));
    out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
    out[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

static bool is_emoji(uint32_t c)
{
    return (c >= 0x1F000 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF) || (c >= 0xFE00 && c <= 0xFE0F) ||
           c == 0x200D || c == 0x20E3;
}

static bool font_has(const lv_font_t *font, uint32_t c)
{
    lv_font_glyph_dsc_t g;
    return lv_font_get_glyph_dsc(font, &g, c, 0);
}

size_t ui_str_sanitize(const char *in, char *out, size_t out_len, const lv_font_t *font)
{
    if (!out || out_len == 0) {
        return 0;
    }
    size_t o = 0, i = 0;
    int newlines = 0;           // newlines seen in the current run
    bool line_start = true;
    while (in && in[i]) {
        uint32_t c = utf8_next(in, &i);
        if (c == 0x2028 || c == 0x2029) {
            c = '\n';
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            if (o == 0) {
                continue;       // no leading blank lines
            }
            if (++newlines <= 2 && o + 1 < out_len) {
                out[o++] = '\n';
            }
            line_start = true;
            continue;
        }
        if (c == '\t') {
            c = ' ';
        }
        if (line_start && c == '-' && in[i] == ' ') {
            c = 0x2022;         // "- item" -> "• item"
        }
        if (c == '#' || c == '*' || c == '_' || c == '`' || c == '>' || c < 0x20 || is_emoji(c)) {
            continue;
        }
        if (font && c != ' ' && !font_has(font, c)) {
            continue;
        }
        if (line_start && c == ' ') {
            continue;           // indentation left by stripped markdown
        }
        char enc[4];
        size_t n = utf8_put(c, enc);
        if (o + n >= out_len) {
            break;
        }
        memcpy(out + o, enc, n);
        o += n;
        newlines = 0;
        line_start = false;
    }
    while (o > 0 && (out[o - 1] == '\n' || out[o - 1] == ' ')) {
        o--;
    }
    out[o] = 0;
    return o;
}

bool ui_str_font_has_all(const char *s, const lv_font_t *font)
{
    size_t i = 0;
    while (s && s[i]) {
        uint32_t c = utf8_next(s, &i);
        if (c != '\n' && c != ' ' && !font_has(font, c)) {
            return false;
        }
    }
    return true;
}

size_t ui_str_cp_count(const char *s)
{
    size_t n = 0;
    for (; s && *s; s++) {
        n += ((uint8_t)*s & 0xC0) != 0x80;
    }
    return n;
}

// ---- Fit test (ui.md 7.9): every string measured against its slot, both languages

typedef struct {
    const lv_font_t *font;
    int32_t max_w;
    const char *suffix;
} slot_def_t;

static slot_def_t slot_of(uint8_t slot)
{
    switch (slot) {
    case SLOT_PILL: return (slot_def_t){ &ui_20, 124, " " SYM_DOWN };   // pill text + chevron
    case SLOT_FAULT: return (slot_def_t){ &ui_20, 156, " " TXT_RSAQUO };  // fault pill (<= 160 wide)
    case SLOT_STATUS: return (slot_def_t){ &ui_16, 180, "" };           // conversation width (empty is 200)
    case SLOT_TITLE: return (slot_def_t){ &ui_20, 147, "" };            // arc title after the chevron
    case SLOT_HINT: return (slot_def_t){ &ui_16, 100, "" };
    case SLOT_TOAST: return (slot_def_t){ &ui_16, 180, "" };            // Home status slot (conversation)
    case SLOT_CARD_T: return (slot_def_t){ &ui_20, 190, "" };           // card row 1 after the icon
    case SLOT_BTN: return (slot_def_t){ &ui_20, 136, "" };              // 160 px action stadium minus padding
    default: return (slot_def_t){ NULL, 0, "" };
    }
}

// Expands printf conversions with wide sample values so the measurement is a worst case.
static void expand_sample(const char *fmt, char *out, size_t len)
{
    size_t o = 0;
    for (size_t i = 0; fmt[i] && o + 4 < len; i++) {
        if (fmt[i] == '%' && fmt[i + 1] == 'd') {
            memcpy(out + o, "100", 3);
            o += 3;
            i++;
        } else if (fmt[i] == '%' && fmt[i + 1] == 's') {
            memcpy(out + o, "WW", 2);
            o += 2;
            i++;
        } else if (fmt[i] == '%' && fmt[i + 1] == '%') {
            out[o++] = '%';
            i++;
        } else {
            out[o++] = fmt[i];
        }
    }
    out[o] = 0;
}

int ui_str_fit_check(void (*out)(const char *line))
{
    int bad = 0;
    char sample[160], line[224];
    for (int id = 0; id < S__N; id++) {
        slot_def_t sd = slot_of(s_tab[id].slot);
        if (!sd.font) {
            continue;
        }
        for (int lang = 0; lang < 2; lang++) {
            const char *txt = lang ? s_tab[id].en : s_tab[id].ru;
            char fmt[160];
            snprintf(fmt, sizeof(fmt), "%s%s", txt, sd.suffix);
            expand_sample(fmt, sample, sizeof(sample));
            lv_point_t sz;
            lv_text_get_size(&sz, sample, sd.font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            if (sz.x > sd.max_w) {
                bad++;
                if (out) {
                    snprintf(line, sizeof(line), "overflow id=%d %s w=%d max=%d \"%s\"", id, lang ? "en" : "ru",
                             (int)sz.x, (int)sd.max_w, sample);
                    out(line);
                }
            }
        }
    }
    return bad;
}
