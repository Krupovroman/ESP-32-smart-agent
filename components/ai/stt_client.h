#pragma once
// STT fallback through POST {stt_url}/audio/transcriptions (protocol §6).
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "audio_in.h"
#include "orhttp.h"

// Transcribes a clip (lang NULL = auto-detect, used in translator mode). Blocks; call it from
// the ai task (TLS needs its large stack). ESP_ERR_NOT_FINISHED when the turn was cancelled.
esp_err_t stt_client_transcribe(uint32_t turn, const audio_clip_t *clip, const char *lang, char *text,
                                size_t text_len);
