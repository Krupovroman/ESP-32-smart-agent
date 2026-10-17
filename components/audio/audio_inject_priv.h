#pragma once
// Hooks between the capture task (audio_in.c) and the injection source (audio_inject.c).
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The injection source is selected and has samples.
bool audio_inject_active(void);
// Active and not paced in real time: the capture task fills a recording at once.
bool audio_inject_fast(void);
// Restarts the injected clip from its first sample (called by audio_in_start).
void audio_inject_rewind(void);
// Copies up to n next injected samples; returns how many (fewer = end of the clip).
size_t audio_inject_read(int16_t *dst, size_t n);
