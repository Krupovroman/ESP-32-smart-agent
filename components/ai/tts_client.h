#pragma once
// TTS through POST {tts_url}/audio/speech with response_format pcm (protocol §5): sanitise,
// split into sentences, route to a voice, stream the PCM into audio_out as it arrives.
// Whether to speak at all (text_only, volume 0) is the caller's decision.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "ai_client.h"

// "tts" task (PSRAM stack) and its queue; called by ai_client_init().
esp_err_t tts_client_init(void);
// Queues text for the speaker in lang (ISO 639-1); events AI_EV_TTS_* carry the turn.
esp_err_t tts_client_speak(uint32_t turn, const char *text, const char *lang);
// Stops turns up to and including turn (0 = all) and flushes the speaker.
void tts_client_cancel(uint32_t turn);
// True while a speak job is queued or running (the speaker may still be playing afterwards).
bool tts_client_busy(void);
// Stats of the last speak job.
void tts_client_last_stats(tts_stats_t *out);
// Route number attempt (0 = preferred) for lang: per-language override vo_<lang>, then
// tts_model/tts_voice, then the protocol §5.5 table, then the §5.6 fallbacks.
esp_err_t tts_route(const char *lang, char *model, size_t mlen, char *voice, size_t vlen, int attempt);
// Sample rate of a response: setting tts_rate, then Content-Type rate=, then the §5.2 model table.
uint32_t tts_rate_for(const char *model, const char *content_type, uint8_t *channels);
// Dev/test fixtures: synthesises text into a PSRAM buffer (caller frees) instead of the speaker.
esp_err_t tts_client_fetch(const char *text, const char *lang, int16_t **pcm, size_t *samples, uint32_t *rate);
// Protocol §5.1 sanitiser (markup, emoji, URLs, whitespace); returns the output length.
size_t tts_sanitize(const char *in, char *out, size_t len);
// Protocol §5.4 sentence split and packing; returns the chunk count (PSRAM, free with tts_split_free).
int tts_split(const char *text, char ***chunks);
void tts_split_free(char **chunks, int n);
