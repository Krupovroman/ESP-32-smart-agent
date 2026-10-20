# Changelog

All notable changes to this project are listed here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions are the git tags of the
repository (`m1`, `m2` are bring-up milestones, `vX.Y-rcN` are release candidates tested on the board).

## [Unreleased]

### Changed
- Default chat model is now `google/gemini-3.5-flash-lite`; `gemini-3.1-flash-lite` became the fallback.

## [v0.8-rc1] - 2026-10-21

### Added
- Reply length setting (Settings → "Длина ответа": short / normal / detailed, normal is the new default and about
  twice as long as before); the token limit follows the level.
- Translator self-check: the model states `target_lang` before translating, and the device checks the alphabet of
  the translation and whether it just repeats the utterance; on a miss it retries once with the direction fixed.
- `tools/tr_eval.py`: PC-side check of tricky translator phrases.
- Public repository: Apache-2.0 license, README in Russian and English, changelog, contributing and security notes,
  issue and pull request templates, `.editorconfig`.

### Changed
- Translator prompt: the utterance is never addressed to the interpreter (questions and commands are translated,
  not answered), short examples, typed text wrapped in `<utterance>`; temperature 0 for translator requests.
- Battery model v4: a charger is detected against a baseline of earlier settled readings (also across a screen
  wake or a turn) and by a slow voltage trend; on battery the percent never goes up.
- Longer speech and console limits for long answers (3000 characters per reply).

### Fixed
- Translator sometimes answered the question or "translated" into the same language.
- Plugging in a wall charger raised the percent but the charging state was not shown.

## [v0.7-rc2] - 2026-10-18

### Changed
- Battery model v3: external power is detected by a voltage step (a wall charger with no USB data is seen within
  5–10 s), FULL by a voltage plateau, and the 100 % level (`bat_vfull`) is learned per board.
- The percent glides down after leaving "charged" or unplugging instead of jumping.

### Added
- Battery history log for the last ~12 hours (`bat log`).

### Fixed
- No false "USB: not enough power" at the end of a charge on a PC port.
- Brightness changes, Wi-Fi joins and the setup portal are no longer taken for a charger being plugged in.
- Power stays detected when macOS stops sending USB frames to an idle port.

## [v0.7-rc1] - 2026-10-18

### Added
- Chinese, Japanese and Korean in the translator: a Noto Sans CJK subset (21,803 glyphs) in a read-only asset pack in
  the `storage` partition, memory-mapped and CRC-checked, used as an LVGL Tiny TTF fallback font.
- `idf.py storage-flash` / `tools\flash.ps1 -Storage` to write the asset pack.
- Host test of the CJK fallback with real LVGL and PNG screenshots.

### Changed
- Long CJK sentences are split for TTS after `，、；：` instead of at a hard length.
- LVGL task stack raised to 12 KB for CJK glyph rasterising.

### Fixed
- A missing or corrupted pack drops a stored CJK translator pair instead of drawing empty boxes.

## [v0.6-rc3] - 2026-10-18

### Added
- Speech boost with a peak limiter (`spk_boost`, default 12 dB) and a 70 % output cap.
- Lock-screen charging animation: comet along the battery ring, breathing ring, glowing bolt, rising particles;
  a slow glow when full.
- Plug glyph for "USB power, not charging".

### Changed
- Honest power states: charging, full, USB but draining, battery. Readings taken under load (speech, requests)
  are skipped.

### Fixed
- No false "wall charger" after a long session; the lock-screen clock wraps while charging; the percent turns red
  below 15 % when draining.

## [v0.6-rc2] - 2026-10-18

### Added
- Web search for the assistant through OpenRouter's `web` plugin: Auto (the model asks for it with `needs_web`),
  Always or Off; "Searching the web…" status, a source note and a "Web search" settings row.

### Fixed
- Long assistant replies in the Home chat show their first line, not their end.

## [v0.6-rc1] - 2026-10-18

### Added
- Lock screen: PWR locks and unlocks with iris and padlock animations; clock, date and battery ring.
- Battery model with a Li-ion curve and charge detection; battery level on Home, warnings at 15 % and 5 %.
- New key roles: BOOT double tap switches Assistant ⇄ Translator, PWR short = lock, PWR 3 s = power off.

### Fixed
- UI freeze (LVGL task stuck in a loop) when opening the history by tapping a chat message.
- No touch talk while locked.

## [v0.5-rc2] - 2026-10-18

### Fixed
- 41 confirmed findings of the first code review, among them: the screen not dimming, a crash when Wi-Fi drops,
  garbled text on the setup page, wrong-language results for old translations.
- The API key is only ever sent over HTTPS, and URLs are never logged.
- Factory reset erases the whole NVS partition.
- Translator offers only languages the fonts can draw; the new pair is saved at once.
- Diagnostics gained microphone and speaker tests; "speaker" and orb face settings persist.
- Joining a hidden open network; English error texts in the English UI; a missing key gets its own card.

### Added
- `docs/STATUS.md` (project status, Russian).

## [v0.5-rc1] - 2026-10-18

First integrated firmware: assistant, translator and the full UI, verified on hardware.

### Added
- NVS settings table, Wi-Fi with saved networks and back-off, phone setup portal (SoftAP + captive DNS + form, QR).
- OpenRouter clients: chat with audio input and strict JSON output, TTS (progressive PCM), STT fallback (Whisper),
  model and voice pickers, key check.
- Dev console protocol v2 over USB-Serial-JTAG: screenshots, touch and audio injection, blobs, events;
  `tools/dev.py` v2 and its offline tests.
- A/B OTA (console push and HTTPS URL), boot self-test, automatic rollback, safe mode, rollback tooling
  (`release.ps1`, `restore.ps1`, `slot.ps1`).
- UI: theme, Inter fonts, orb with a face, rim, Home/chat/translator/history/reader screens, round keyboard,
  settings tree, Wi-Fi/provider/model/voice/translator screens, dialog, onboarding, ambient clock and sleep.
- App state machine: push-to-talk (hold and tap), pre-roll, speech gate, barge-in, text-only mode.
- On-device smoke test `tools/smoke.py` with synthetic voice fixtures; host tests of the AI client and the app.

### Changed
- Modules moved into ESP-IDF components with `MINIMAL_BUILD`.
- Power latch (GPIO7) held through software and watchdog resets; bootloader hook sets it right after ROM.
- lwIP TCP send buffer 23 KB (uploads 80 → 250 KB/s); 2 MB PSRAM playback ring.

## [m2] - 2026-10-17

### Added
- I2S microphone capture (ICS-43434, right slot, DC blocker, level meter, PSRAM recording, WAV dump) with a runtime
  sample-rate switch.
- PCM5101 speaker output (ring buffer, volume, test tone); console `mic` / `spk` commands and blob transfer.

## [m1] - 2026-10-17

### Added
- Board bring-up: power latch, I2C, TCA9554 expander, ST77916 panel-ID probe (two panel batches), CST816 touch.
- LVGL port and a dev console with screenshots and touch injection.
- Hardware reference, requirements, OpenRouter protocol spec and UI design drafts; factory flash backup notes.

[Unreleased]: https://github.com/Krupovroman/ESP-32-smart-agent/compare/v0.8-rc1...HEAD
[v0.8-rc1]: https://github.com/Krupovroman/ESP-32-smart-agent/compare/v0.7-rc2...v0.8-rc1
[v0.7-rc2]: https://github.com/Krupovroman/ESP-32-smart-agent/compare/v0.7-rc1...v0.7-rc2
[v0.7-rc1]: https://github.com/Krupovroman/ESP-32-smart-agent/compare/v0.6-rc3...v0.7-rc1
[v0.6-rc3]: https://github.com/Krupovroman/ESP-32-smart-agent/compare/v0.6-rc2...v0.6-rc3
[v0.6-rc2]: https://github.com/Krupovroman/ESP-32-smart-agent/compare/v0.6-rc1...v0.6-rc2
[v0.6-rc1]: https://github.com/Krupovroman/ESP-32-smart-agent/compare/v0.5-rc2...v0.6-rc1
[v0.5-rc2]: https://github.com/Krupovroman/ESP-32-smart-agent/compare/v0.5-rc1...v0.5-rc2
[v0.5-rc1]: https://github.com/Krupovroman/ESP-32-smart-agent/compare/m2...v0.5-rc1
[m2]: https://github.com/Krupovroman/ESP-32-smart-agent/compare/m1...m2
[m1]: https://github.com/Krupovroman/ESP-32-smart-agent/releases/tag/m1
