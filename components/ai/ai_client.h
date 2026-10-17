#pragma once
// OpenAI-compatible chat with audio input (docs/protocol.md §2-§4, §7, §8; architecture §1.10).
// All network work runs in the "ai" task (chat, STT, GETs) and the "tts" task (speech); both
// have PSRAM stacks and never write flash. Turns are asynchronous: results and errors are
// delivered to listeners as ai_event_t.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "orhttp.h"

typedef enum { AI_MODE_ASSISTANT, AI_MODE_TRANSLATOR } ai_mode_t;

// One turn. pcm is lent to ai until AI_EV_RELEASED (it is re-streamed on retries); text is copied.
typedef struct {
    uint32_t turn;                    // from ai_client_new_turn()
    ai_mode_t mode;
    const int16_t *pcm;               // voice input (mono s16), NULL for typed input
    size_t samples;
    uint32_t rate;                    // 16000
    const char *text;                 // typed input, used when pcm is NULL
    const char *lang_a, *lang_b;      // translator languages, NULL = settings tr_lang_a / tr_lang_b
    const char *src_lang;             // translator voice: source forced to lang_a or lang_b, NULL = detect
} ai_request_t;

typedef struct {
    uint32_t clip_ms;                 // audio sent after the silence trim
    uint32_t upload_ms;               // request start -> body sent (last attempt)
    uint32_t first_byte_ms;           // body sent -> response headers
    uint32_t ai_ms;                   // submit -> result parsed (STT and retries included)
    uint32_t stt_ms;                  // Whisper round trip, 0 when not used
    uint32_t retries;
    uint32_t bytes_up;                // request body bytes of the last attempt
    int http_status;
    bool reused;                      // the chat request ran on a kept-alive socket
} ai_timings_t;

typedef struct {
    uint32_t turn;
    ai_mode_t mode;
    char *transcript;                 // never NULL; valid during the callback only
    char *text;                       // reply (assistant) or translation (translator); never NULL
    char lang[8];                     // assistant: language of the reply; translator: target language
    char src_lang[8];                 // translator: detected language (A, B or "other")
    char tts_lang[8];                 // language to speak text in; "" = do not speak it
    char finish[16];                  // finish_reason
    char model[64], provider[32], gen_id[64];
    bool truncated;                   // P3: finish_reason "length", reply salvaged
    bool unheard;                     // P4: empty transcript, nothing to speak or store
    bool stt_used;                    // transcript came from the Whisper fallback
    bool other_lang;                  // translator: neither of the two languages
    const char *notice_ru;            // P3 / P4 text for the UI, or NULL
    double cost;                      // usage.cost of chat + STT (USD)
    int prompt_tokens, completion_tokens, audio_tokens, reasoning_tokens;
    ai_timings_t t;
} ai_result_t;

typedef struct {
    uint32_t turn;
    uint32_t chunks, chunks_done;
    uint32_t rate;
    uint8_t channels;
    uint32_t bytes;                   // PCM bytes queued to the speaker
    uint32_t ttfb_ms;                 // speak() -> first PCM byte
    uint32_t total_ms;                // speak() -> last chunk received
    uint32_t chars;                   // characters synthesised
    uint32_t retries, route_switches;
    char model[64], voice[64];
    double cost_est;                  // characters x model price (TTS responses carry no cost)
} tts_stats_t;

typedef enum {
    AI_EV_UPLOAD_DONE,                // body sent; i = upload ms
    AI_EV_RETRY,                      // i = retry number; err = reason (msg_retry_ru, else msg_ru)
    AI_EV_RESULT,                     // result
    AI_EV_ERROR,                      // final; err->msg_ru is the user-facing message
    AI_EV_RELEASED,                   // the turn's pcm is no longer read
    AI_EV_TTS_FIRST_AUDIO,            // i = ttfb ms; tts
    AI_EV_TTS_DONE,                   // every chunk was queued to the speaker; tts
    AI_EV_TTS_ERROR,                  // err (T1 / T2 / T3); tts
} ai_ev_type_t;

// Pointers are valid during the callback only; listeners copy what they keep and never block.
typedef struct {
    ai_ev_type_t type;
    uint32_t turn;
    int32_t i;
    const ai_result_t *result;
    const or_err_t *err;
    const tts_stats_t *tts;
} ai_event_t;

typedef void (*ai_event_cb_t)(const ai_event_t *ev, void *ctx);

// Summary of the last finished turn (for "ai last" / the state command).
typedef struct {
    uint32_t turn;
    ai_mode_t mode;
    bool valid, ok;
    char err_id[8];
    char err_msg[160];
    char model[64], provider[32], gen_id[64], finish[16];
    double cost;
    int prompt_tokens, completion_tokens, audio_tokens;
    bool truncated, unheard, stt_used;
    ai_timings_t t;
} ai_last_t;

// Chat model picker entry (protocol §7.1), about 160 bytes.
typedef struct {
    char id[65], name[49];
    float usd_in_m, usd_out_m, usd_audio_m, est_turn_usd;   // $ per 1M tokens; estimated $ per reference turn
    bool mandatory;                   // reasoning cannot be switched off
    bool audio, structured;           // input_modalities has audio; response_format + structured_outputs
    char min_effort[12];              // effort to send ("" = omit reasoning)
    char expires[11];                 // expiration_date (YYYY-MM-DD) or ""
} ai_model_t;

// TTS model picker entry (protocol §7.2).
typedef struct {
    char id[65], name[49];
    float usd_1k_ru;                  // estimated $ per 1000 Russian characters
    uint16_t n_voices;
    char first_voice[64];
} ai_tts_model_t;

// GET /key summary (protocol §8).
typedef struct {
    char label[48];
    bool has_limit, has_limit_remaining, has_budget, has_remaining, free_tier;
    double limit, limit_remaining;
    double budget_remaining, budget_limit, budget_spend;
    double remaining, remaining_of;   // min of the non-null caps, and the cap it belongs to
    double usage, usage_daily, usage_weekly, usage_monthly;
    char limit_reset[12];             // daily / weekly / monthly / ""
    char budget_resets_at[32], expires_at[32];
} ai_key_info_t;

// cJSON hooks into PSRAM, then the "ai" (chat/STT/GET) and "tts" tasks.
esp_err_t ai_client_init(void);
bool ai_client_ready(void);
// Adds an event listener (up to 4). Called from the ai/tts tasks: post, never block.
esp_err_t ai_add_listener(ai_event_cb_t cb, void *ctx);
// Next turn id (monotonic, shared by the app and the console).
uint32_t ai_client_new_turn(void);
// Queues a turn (non-blocking); ESP_ERR_TIMEOUT when the queue is full.
esp_err_t ai_client_submit(const ai_request_t *r);
// Cancels every turn up to and including turn (0 = all issued so far), chat and TTS.
void ai_client_cancel(uint32_t turn);
// Forgets the conversation (protocol §4.3 reset).
void ai_client_history_clear(void);
// Number of stored exchanges.
int ai_client_history_len(void);
// On push-to-talk: opens the TLS connection early with GET /key when no socket is live.
esp_err_t ai_client_prewarm(void);
// Summary of the last finished turn; false before the first one.
bool ai_client_last(ai_last_t *out);
// Sum of usage.cost (chat, STT) and TTS estimates since boot (USD).
double ai_client_session_cost(void);
void ai_client_add_cost(double usd);
// Last chat request with the audio and the key left out (prefix + "<N base64 chars>" + suffix).
size_t ai_client_last_request_redacted(char *out, size_t len);

// Ranked, filtered chat models (protocol §7.1). Page 0 refetches when older than 120 s;
// *total is the number of entries after filtering. Blocks (runs in the ai task).
esp_err_t ai_client_list_models(int page, ai_model_t *out, int max, int *n, int *total);
// TTS models (protocol §7.2), same paging rules.
esp_err_t ai_client_list_tts_models(int page, ai_tts_model_t *out, int max, int *n, int *total);
// '\n'-separated voices of a listed TTS model; locale (e.g. "ru-RU") filters microsoft/* voices.
size_t ai_client_tts_voices(const char *model, const char *locale, char *out, size_t len);
// GET /model/{id} (NULL = ai_model); ESP_ERR_NOT_FOUND on 404. Caches the entry for the effort rule.
esp_err_t ai_client_check_model(const char *id, ai_model_t *out, or_err_t *err);
// GET /key (protocol §8). err may be NULL.
esp_err_t ai_client_key_info(ai_key_info_t *out, or_err_t *err);
// Russian display lines of protocol §8 ("Осталось …", "Потрачено: …", warnings), '\n'-separated.
size_t ai_key_info_lines(const ai_key_info_t *k, char *out, size_t len);
