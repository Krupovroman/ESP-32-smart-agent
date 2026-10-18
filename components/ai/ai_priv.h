#pragma once
// Internal interfaces shared by the files of the ai component.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "ai_client.h"
#include "tts_client.h"

#define AI_HIST_MAX_N   10        // ring size; the setting picks how many are sent
#define AI_HIST_MAX_CP  3000      // protocol §4.3 budget in code points

// ---- ai_util.c --------------------------------------------------------------------------------
void *ai_malloc(size_t n);                               // PSRAM first, internal as a fallback
void *ai_calloc(size_t n, size_t sz);
char *ai_strdup(const char *s);
char *ai_strndup(const char *s, size_t n);
int64_t ai_now_ms(void);
// Settings that may be missing from the table get def (the table is owned by components/settings).
int32_t cfg_int(const char *key, int32_t def);
void cfg_str(const char *key, char *buf, size_t len, const char *def);
// tts_url / stt_url fall back to ai_url when empty.
void cfg_url(const char *key, char *buf, size_t len);
// tts_key / stt_key for the endpoint url; empty falls back to ai_key only when url is on the
// ai_url host. Returns false for another host without its own key: the request then goes
// without Authorization (a keyless service) instead of carrying the AI key there.
bool cfg_key(const char *key, const char *url, char *buf, size_t len);
// base + path with exactly one '/' between them.
void ai_join_url(char *out, size_t len, const char *base, const char *path);
bool ai_net_up(void);
bool ai_time_synced(void);
// Sleeps in 50 ms slices; returns false when ab() fired.
bool ai_sleep_ms(uint32_t ms, or_abort_fn ab, void *ctx);
bool ai_contains_ci(const char *s, const char *w);
const char *ai_json_str(const cJSON *o, const char *k);
double ai_json_num(const cJSON *o, const char *k, double def);
// Value of a depth-1 string field even in truncated JSON (protocol §2.3 P3 salvage); PSRAM or NULL.
char *ai_json_salvage(const char *json, const char *key);
// UTF-8 helpers (lengths in code points).
uint32_t utf8_next(const char **p);                      // 0 at the end, U+FFFD on bad bytes
size_t utf8_put(uint32_t cp, char *out);                 // writes 1-4 bytes
size_t utf8_cp_len(const char *s);
// Copy of s limited to max_cp code points, "…" appended when cut (PSRAM).
char *utf8_trunc_dup(const char *s, size_t max_cp);
// Host of an http(s) URL without "www." and the port ("https://www.gismeteo.ru/x" -> "gismeteo.ru"); "" if none.
void ai_url_host(const char *url, char *out, size_t len);
// Web answers (protocol §2.8): drops markdown links "[label](http…)" (a host or number label with them), footnote
// marks "[1]", bare http(s) URLs and the brackets or spaces they leave, in place. host (may be NULL) gets the host
// of the first link when it is still empty.
void ai_strip_links(char *s, char *host, size_t host_len);

// ---- ai_client.c ------------------------------------------------------------------------------
void ai_emit(const ai_event_t *ev);
// Runs fn in the ai task and waits for it (inline when already there).
esp_err_t ai_run_sync(esp_err_t (*fn)(void *ctx), void *ctx);
bool ai_turn_cancelled(uint32_t turn);
// Abort callback for or_req_t: ctx points at a uint32_t turn.
bool ai_abort_turn(void *ctx);
// or_abort_fn (ctx unused): true while a job (a turn) waits in the ai queue behind this one.
bool ai_job_waiting(void *ctx);
// Unused stack bytes (high-water mark) of the ai and tts tasks, 0 before they start.
uint32_t ai_task_stack_free(void);
uint32_t tts_task_stack_free(void);

// ---- ai_prompts.c -----------------------------------------------------------------------------
// Web search shape of an assistant request (protocol §2.8); the values match the ai_web setting.
typedef enum {
    AI_WEB_OFF,                   // no search possible: the prompt says there is no internet
    AI_WEB_AUTO,                  // schema has needs_web; the turn is re-run with the plugin when it is true
    AI_WEB_ON,                    // "plugins":[{"id":"web"}] on this request
} ai_web_t;
// PSRAM; {WEB} gets the rule line of web, {DATETIME} filled or its line removed.
char *ai_prompt_assistant(ai_web_t web);
char *ai_prompt_translator(const char *a, const char *b);
const char *ai_lang_name(const char *code);              // English name used in the prompt
bool ai_lang_in_enum(const char *code);                  // assistant "lang" enum (other excluded)
void ai_datetime_ru(char *out, size_t len);              // "" when the clock is not set

// ---- ai_history.c -----------------------------------------------------------------------------
void hist_init(void);
void hist_clear(void);
// Clears on a mode change, a chat model change or after idle_min minutes (0 = never).
void hist_touch(int mode, const char *model, int idle_min);
void hist_add(const char *transcript, const char *reply, const char *lang);
int hist_count(void);
// Appends up to max_n exchanges within max_cp code points as user/assistant messages, oldest first.
void hist_append_messages(cJSON *messages, int max_n, int max_cp);

// ---- ai_models.c ------------------------------------------------------------------------------
void ai_model_from_json(const cJSON *m, ai_model_t *out);
bool ai_model_cache_get(const char *id, ai_model_t *out);
void ai_model_cache_put(const ai_model_t *m);
// Common GET in the ai task: JSON body (PSRAM) of base_url + path; silent background retries
// unless once is set.
esp_err_t ai_get_json(const char *base_url, const char *path, size_t cap, bool once, char **body, int *status,
                      or_err_t *err);
// TLS pre-warm in the ai task: GET /key, then GET /model/{model} when it is not cached; single
// attempts that stop as soon as a turn is waiting in the queue (pending() true).
void ai_prewarm_gets(const char *model, bool (*pending)(void));

// ---- stt_client.c -----------------------------------------------------------------------------
// Whisper call for a turn: text (PSRAM) and cost of the transcription.
esp_err_t stt_transcribe_ex(uint32_t turn, const int16_t *pcm, size_t samples, uint32_t rate, const char *lang,
                            char **text, double *cost, uint32_t *ms, or_err_t *err);

// ---- tts_voices.c -----------------------------------------------------------------------------
typedef struct {
    char model[97];
    char voice[96];
} tts_route_t;
int tts_routes(const char *lang, tts_route_t *out, int max);
const char *tts_model_default_voice(const char *model, const char *lang);
void tts_route_fail(const char *model, const char *lang, bool all_langs);
bool tts_route_failed(const char *model, const char *lang);
double tts_price_per_char(const char *model);
bool tts_speed_unsupported(const char *model);
void tts_speed_mark_unsupported(const char *model);
// Content-Type "audio/pcm;rate=24000;channels=1" -> media type (lowercase), rate and channels (0 = absent).
void tts_parse_ct(const char *ct, char *media, size_t mlen, uint32_t *rate, uint8_t *channels);
