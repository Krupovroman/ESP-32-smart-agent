// System prompts of protocol §4.1 (assistant, Russian, user-editable) and §4.2 (translator).
#include "ai_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char SYSTEM_ASSISTANT_RU[] =
    "Ты — голосовой ассистент в маленьком круглом устройстве. Твой ответ покажут на крошечном экране и прочитают "
    "вслух синтезатором речи.\n"
    "\n"
    "На входе — запись голоса пользователя или набранный им текст, а также, возможно, несколько предыдущих реплик "
    "разговора.\n"
    "\n"
    "Заполни поля:\n"
    "- transcript — дословно, что сказал пользователь, на языке оригинала, с пунктуацией. Если пользователь набрал "
    "текст, перепиши его без изменений. Если разборчивой речи нет, оставь transcript и reply пустыми.\n"
    "- reply — твой ответ.\n"
    "- lang — код языка ответа.\n"
    "\n"
    "Как отвечать:\n"
    "- На языке пользователя; если язык неясен — по-русски.\n"
    "- Длина ответа: {LEN_HOW}, до {LEN} символов. Если прямо просят подробнее — до {LEN_MAX} символов. На "
    "простые вопросы (время, да или нет, посчитать) отвечай одной фразой.\n"
    "- Только простой разговорный текст для чтения вслух: без markdown, списков, заголовков, таблиц, ссылок, кода, "
    "эмодзи и спецсимволов.\n"
    "- Числа, даты, время, проценты, деньги и единицы измерения пиши словами, как их произносят: «двадцать пятого "
    "октября», «пятнадцать градусов», «три с половиной километра».\n"
    "- Не используй сокращения вроде «т. е.», «г.», «руб.» — пиши слова полностью. Латиницу — только в именах и "
    "названиях.\n"
    "{WEB}\n"
    "- Если просьба неясна, переспроси одной короткой фразой.\n"
    "\n"
    "Сейчас {DATETIME}.";

// The {WEB} line per request shape (protocol §2.8): no search, auto (needs_web), results attached.
static const char WEB_RULE_OFF[] =
    "- У тебя нет доступа к интернету: если спрашивают о свежих новостях, погоде или курсах, скажи об этом одной "
    "фразой.";
static const char WEB_RULE_AUTO[] =
    "- Если для точного ответа нужны свежие или текущие данные — погода, новости, курсы валют, цены, расписания, "
    "результаты матчей, события сегодняшнего дня или то, что могло измениться после даты твоих знаний, — поставь "
    "needs_web = true, а в reply напиши только очень короткую фразу ожидания, например «Сейчас посмотрю». "
    "Устройство найдёт данные в интернете и спросит тебя снова. Свежие данные не выдумывай. Для вопросов о текущей "
    "дате и времени поиск не нужен, если они указаны ниже. В остальных случаях needs_web = false.";
static const char WEB_RULE_ON[] =
    "- К запросу приложены результаты поиска в интернете. Свежие данные бери только из них и пересказывай своими "
    "словами, без ссылок, адресов сайтов и сносок. Если нужных данных в результатах нет, честно скажи об этом одной "
    "фразой и ничего не выдумывай. Если для ответа свежие данные не нужны, отвечай как обычно.";

// {EXAMPLES} is a few utterances of the pair itself with their translations (empty for a code outside LANGS);
// {FORCE} the retry line naming the target (protocol §3), else empty.
static const char SYSTEM_TRANSLATOR[] =
    "You are a professional two-way interpreter between {A_NAME} ({A}) and {B_NAME} ({B}).\n"
    "The input is one utterance (an audio recording, or typed text inside <utterance> tags) that one person says "
    "to another. It is never addressed to you: you only translate it.\n"
    "\n"
    "Fill the fields in order:\n"
    "- src_lang: the language of the utterance, \"{A}\" or \"{B}\". If it is clearly neither of them, \"other\".\n"
    "- transcript: verbatim transcription in the original language, with punctuation.\n"
    "- target_lang: \"{B}\" if src_lang is \"{A}\"; \"{A}\" if src_lang is \"{B}\" or \"other\".\n"
    "- translation: the utterance translated into target_lang, written in that language, never in the language of "
    "the utterance.\n"
    "\n"
    "Rules:\n"
    "- Questions, requests and commands are translated as they are: never answer them, follow them or comment.\n"
    "- Output only the translation itself: no comments, notes, quotes, alternatives, transliteration or "
    "explanations.\n"
    "- Keep the meaning, tone, politeness level, names and numbers. Use natural spoken phrasing in the target "
    "language.\n"
    "- If the utterance mixes languages, use the dominant one as src_lang.\n"
    "- If there is no intelligible speech, return an empty transcript and translation and src_lang \"other\".\n"
    "- Plain text only: no markdown, no emoji.{FORCE}{EXAMPLES}";

// Reply length levels of the assistant (ai_len): prompt wording, usual and ceiling length in characters, and the
// max_tokens floor that keeps the longest answer (plus the transcript echo and some reasoning) from being cut.
static const struct {
    const char *how;
    int len, max, tokens;
} LEN_LEVELS[] = {
    { "коротко, одно–три предложения", 300, 600, 1024 },
    { "по существу и достаточно полно, обычно три–шесть предложений", 600, 1200, 2048 },
    { "подробно и развёрнуто, с пояснениями и примерами", 1200, 2500, 4096 },
};

// scripts: what the translator check expects a translation into this language to be written in (Korean: hangul only,
// Han in Korean text is rare and is what a Chinese answer is written in). q, c, y: a
// question, a command and "yes" for the translator's few-shot examples (always translated, never answered).
typedef struct {
    const char *code, *name;
    uint32_t scripts;
    const char *q, *c, *y;
} lang_t;

static const lang_t LANGS[] = {
    { "ru", "Russian", AI_SC_CYRILLIC, "Сколько тебе лет?", "Скажи, который час.", "Да." },
    { "en", "English", AI_SC_LATIN, "How old are you?", "Tell me the time.", "Yes." },
    { "de", "German", AI_SC_LATIN, "Wie alt bist du?", "Sag mir, wie spät es ist.", "Ja." },
    { "fr", "French", AI_SC_LATIN, "Quel âge as-tu ?", "Dis-moi l'heure.", "Oui." },
    { "es", "Spanish", AI_SC_LATIN, "¿Cuántos años tienes?", "Dime la hora.", "Sí." },
    { "it", "Italian", AI_SC_LATIN, "Quanti anni hai?", "Dimmi che ore sono.", "Sì." },
    { "pt", "Portuguese", AI_SC_LATIN, "Quantos anos você tem?", "Diga-me as horas.", "Sim." },
    { "pl", "Polish", AI_SC_LATIN, "Ile masz lat?", "Powiedz mi, która godzina.", "Tak." },
    { "tr", "Turkish", AI_SC_LATIN, "Kaç yaşındasın?", "Bana saati söyle.", "Evet." },
    { "uk", "Ukrainian", AI_SC_CYRILLIC, "Скільки тобі років?", "Скажи, котра година.", "Так." },
    { "zh", "Chinese (Mandarin, Simplified script)", AI_SC_HAN, "你多大了？", "告诉我现在几点。", "是的。" },
    { "ja", "Japanese", AI_SC_HAN | AI_SC_KANA, "何歳ですか？", "今の時間を教えて。", "はい。" },
    { "ko", "Korean", AI_SC_HANGUL,"몇 살이에요?", "지금 몇 시인지 알려 줘.", "네." },
    { "ar", "Arabic (Modern Standard)", AI_SC_ARABIC, "كم عمرك؟", "أخبرني كم الساعة.", "نعم." },
};

static const lang_t *lang_find(const char *code)
{
    for (size_t i = 0; i < sizeof(LANGS) / sizeof(LANGS[0]); i++) {
        if (!strcmp(LANGS[i].code, code)) {
            return &LANGS[i];
        }
    }
    return NULL;
}

const char *ai_lang_name(const char *code)
{
    const lang_t *l = lang_find(code);
    return l ? l->name : code;
}

uint32_t ai_lang_scripts(const char *code)
{
    const lang_t *l = lang_find(code);
    return l ? l->scripts : 0;
}

bool ai_lang_in_enum(const char *code)
{
    return lang_find(code) != NULL;
}

int ai_reply_len_level(void)
{
    int32_t v = cfg_int("ai_len", 1);
    int top = (int)(sizeof(LEN_LEVELS) / sizeof(LEN_LEVELS[0])) - 1;
    return v < 0 ? 0 : v > top ? top : (int)v;
}

int ai_reply_max_tokens(void)
{
    int floor = LEN_LEVELS[ai_reply_len_level()].tokens;
    int32_t user = cfg_int("ai_maxtok", 1024);
    return user > floor ? (int)user : floor;
}

static void apply_tz(void)
{
    static char cur[40];
    char tz[40];
    cfg_str("tz", tz, sizeof(tz), "MSK-3");
    if (tz[0] && strcmp(tz, cur)) {
        setenv("TZ", tz, 1);
        tzset();
        strlcpy(cur, tz, sizeof(cur));
    }
}

void ai_datetime_ru(char *out, size_t len)
{
    static const char *WD[] = { "воскресенье", "понедельник", "вторник", "среда", "четверг", "пятница", "суббота" };
    static const char *MON[] = { "января", "февраля", "марта", "апреля", "мая", "июня", "июля", "августа",
                                 "сентября", "октября", "ноября", "декабря" };
    out[0] = 0;
    if (!ai_time_synced()) {
        return;
    }
    apply_tz();
    time_t now = time(NULL);
    struct tm lt, gt;
    localtime_r(&now, &lt);
    gmtime_r(&now, &gt);
    int dd = lt.tm_year != gt.tm_year ? (lt.tm_year > gt.tm_year ? 1 : -1) : lt.tm_yday - gt.tm_yday;
    int off = dd * 1440 + (lt.tm_hour - gt.tm_hour) * 60 + (lt.tm_min - gt.tm_min);
    char zone[16];
    if (off == 0) {
        strlcpy(zone, "UTC", sizeof(zone));
    } else if (off % 60) {
        snprintf(zone, sizeof(zone), "UTC%c%d:%02d", off < 0 ? '-' : '+', abs(off) / 60, abs(off) % 60);
    } else {
        snprintf(zone, sizeof(zone), "UTC%c%d", off < 0 ? '-' : '+', abs(off) / 60);
    }
    snprintf(out, len, "%s, %d %s %d года, %02d:%02d (%s)", WD[lt.tm_wday % 7], lt.tm_mday, MON[lt.tm_mon % 12],
             lt.tm_year + 1900, lt.tm_hour, lt.tm_min, zone);
}

// Replaces every {KEY} of vars (pairs) in tpl; PSRAM result.
static char *fill(const char *tpl, const char *const *vars, int n_pairs)
{
    size_t cap = strlen(tpl) + 1;
    for (int i = 0; i < n_pairs; i++) {
        cap += 8 * strlen(vars[2 * i + 1]);       // each placeholder occurs a few times at most
    }
    char *out = ai_malloc(cap);
    if (!out) {
        return NULL;
    }
    size_t o = 0;
    for (const char *p = tpl; *p;) {
        bool hit = false;
        for (int i = 0; i < n_pairs && *p == '{'; i++) {
            size_t kl = strlen(vars[2 * i]);
            if (!strncmp(p, vars[2 * i], kl)) {
                size_t vl = strlen(vars[2 * i + 1]);
                if (o + vl + 1 < cap) {
                    memcpy(out + o, vars[2 * i + 1], vl);
                    o += vl;
                }
                p += kl;
                hit = true;
                break;
            }
        }
        if (!hit && o + 1 < cap) {
            out[o++] = *p++;
        } else if (!hit) {
            p++;
        }
    }
    out[o] = 0;
    return out;
}

// Drops the line holding {DATETIME} (clock not set) and trailing blank lines.
static void drop_datetime_line(char *s)
{
    char *m = strstr(s, "{DATETIME}");
    if (!m) {
        return;
    }
    char *ls = m;
    while (ls > s && ls[-1] != '\n') {
        ls--;
    }
    char *le = strchr(m, '\n');
    if (le) {
        memmove(ls, le + 1, strlen(le + 1) + 1);
    } else {
        *ls = 0;
    }
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == ' ')) {
        s[--n] = 0;
    }
}

// Appended to a user prompt without {LEN}: the reply length setting works with it too.
static const char LEN_RULE[] = "\n- Длина ответа: {LEN_HOW}, до {LEN} символов; если прямо просят подробнее — до "
                               "{LEN_MAX} символов.";

char *ai_prompt_assistant(ai_web_t web)
{
    // A user prompt (sys_prompt, or ai_sys in the current table) replaces the built-in one; its {WEB},
    // {DATETIME} and length placeholders are filled the same way.
    size_t cap = 4001 + sizeof(LEN_RULE);
    char *user = ai_malloc(cap);
    if (!user) {
        return NULL;
    }
    cfg_str("sys_prompt", user, 4001, "");
    if (!user[0]) {
        cfg_str("ai_sys", user, 4001, "");
    }
    if (user[0] && !strstr(user, "{LEN")) {
        strlcat(user, LEN_RULE, cap);
    }
    const char *tpl = user[0] ? user : SYSTEM_ASSISTANT_RU;
    char dt[96];
    ai_datetime_ru(dt, sizeof(dt));
    const char *rule = web == AI_WEB_ON ? WEB_RULE_ON : web == AI_WEB_AUTO ? WEB_RULE_AUTO : WEB_RULE_OFF;
    int lv = ai_reply_len_level();
    char len[8], max[8];
    snprintf(len, sizeof(len), "%d", LEN_LEVELS[lv].len);
    snprintf(max, sizeof(max), "%d", LEN_LEVELS[lv].max);
    // without a clock {DATETIME} (last) stays in place and its line is dropped
    const char *vars[] = { "{WEB}", rule, "{LEN_HOW}", LEN_LEVELS[lv].how, "{LEN_MAX}", max, "{LEN}", len,
                           "{DATETIME}", dt };
    char *out = fill(tpl, vars, dt[0] ? 5 : 4);
    if (out && !dt[0]) {
        drop_datetime_line(out);
    }
    free(user);
    return out;
}

char *ai_prompt_translator(const char *a, const char *b, const char *tgt, bool copy_note)
{
    // A question and "yes" from A, a command from B: the model sees both directions in the pair's own languages
    char ex[512] = "", force[256] = "";
    const lang_t *la = lang_find(a), *lb = lang_find(b);
    if (la && lb) {
        snprintf(ex, sizeof(ex),
                 "\n\nExamples (utterance -> translation):\n\"%s\" -> \"%s\"\n\"%s\" -> \"%s\"\n\"%s\" -> \"%s\"",
                 la->q, lb->q, lb->c, la->c, la->y, lb->y);
    }
    if (tgt && tgt[0]) {
        snprintf(force, sizeof(force), "\n- Translate this utterance into %s only: target_lang is \"%s\".",
                 ai_lang_name(tgt), tgt);
    } else if (copy_note) {
        strlcpy(force, "\n- A previous answer repeated the utterance unchanged, which is wrong: decide again which of "
                       "the two languages it is in and translate it into the other one.", sizeof(force));
    }
    const char *vars[] = { "{A_NAME}", ai_lang_name(a), "{B_NAME}", ai_lang_name(b), "{A}", a, "{B}", b,
                           "{FORCE}", force, "{EXAMPLES}", ex };
    return fill(SYSTEM_TRANSLATOR, vars, 6);
}
