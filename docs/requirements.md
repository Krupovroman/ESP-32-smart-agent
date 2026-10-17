# Requirements

Device: Waveshare ESP32-S3-Touch-LCD-1.85 rev 2.1 (see `hardware.md`). User language: Russian.

## Functional

1. **Assistant mode** — the user presses a button (on-screen and/or the physical BOOT button), speaks,
   and the AI answers. The answer appears as a chat message on screen **and** is spoken aloud.
   A mode toggle switches voice replies off (text only) and back on.
2. **Translator mode** — the user or the person they talk to speaks; the device speaks the translation.
   The two languages are selectable. The device should work out which of the two was spoken and
   translate into the other.
3. **Settings** — AI provider (base URL + API key), AI model, TTS model/voice, Wi-Fi, volume,
   brightness, UI language, etc. Default provider: **OpenRouter** with a cheap model.
   The user has a working OpenRouter key.
4. **On-screen keyboard** for manual input, designed for the round 360×360 display (RU + EN layouts).
5. Speakers are not connected yet; everything must also work with no speaker (text-only).

## Non-functional

- **Beautiful, animated UI** designed for a round screen (nothing important outside the circle,
  smooth state animations).
- **Rollback**: if anything goes wrong during development it must be possible to go back —
  factory flash backup (done), git commits + tags per milestone, A/B OTA slots with bootloader
  rollback, documented manual recovery (BOOT+RESET download mode).
- Secrets (API key, Wi-Fi password) never in git.

## Decisions taken (2026-10-17)

- Toolchain: ESP-IDF v6.1 (installed via EIM to `C:\esp`), C, LVGL 9.5.x via esp_lvgl_port 2.9.
- Board bring-up reference: xiaozhi-esp32 `main/boards/waveshare/esp32-s3-touch-lcd-1.85` (MIT —
  keep the notice). Touch (CST816) must be added ourselves; xiaozhi never initialises it.
- AI: OpenRouter `chat/completions` with `input_audio` (16 kHz mono WAV, base64), structured JSON
  output; default model `google/gemini-3.1-flash-lite`, reasoning effort `minimal`.
- TTS: OpenRouter `/audio/speech`, `response_format: pcm`, default `microsoft/mai-voice-2.1-flash`
  with `ru-RU-Masha:MAI-Voice-2.1-Flash`; alternative `google/gemini-3.8-flash-lite-tts`.
- Every endpoint (chat / STT / TTS) has its own base URL + key + model so any OpenAI-compatible
  service can be plugged in.
- Dev tooling: a USB serial dev console that can take screenshots (lv_snapshot → PNG on the PC) and
  inject touches, so the UI can be verified without someone looking at the device.
