# Round Assistant: Final UI/UX Specification

**Version:** 1.0, 2026-10-17. This is the build spec for the engineers.

**Base:** concept "Calm Watch", with grafts from "Rim-Centric" and "Expressive" (section 0).

**Target hardware and software:**
- Board: Waveshare ESP32-S3-Touch-LCD-1.85 rev 2.1.
- Display: ST77916 round IPS, 360×360, RGB565, over QSPI. At 80 MHz the bus moves about 40 MB/s. During bring-up at 40 MHz it moves about 20 MB/s, and every bus time in this document doubles.
- Touch: CST816T, single finger, on I2C1 (SDA GPIO1, SCL GPIO3, INT GPIO4).
- Software: ESP-IDF 6.1, LVGL 9.5.x through esp_lvgl_port 2.9.
- Rendering: PARTIAL mode with two 360×40 RGB565 draw buffers (28.8 KB each) in internal DMA SRAM. The LVGL heap and all other large data live in the 8 MB PSRAM.
- Buttons: BOOT (GPIO0, active low) and PWR (GPIO6, active low). GPIO7 is the power latch.
- UI languages: Russian (default) and English.

**Sources:**
- Project files: `docs/requirements.md`, `docs/hardware.md`, `docs/research/stack_2026-10-17.json`. The `verify:` corrections in the research file override the research entries.
- LVGL release/v9.5 sources I read for this spec:
  - `lv_buttonmatrix.c`: `update_map`, `get_button_from_point`, PRESSED/RELEASED handling.
  - `lv_keyboard.c`: `set_map`, `set_mode`, `update_ctrl_map`.
  - `lv_qrcode.c`, `lv_arc.c` (hit test), `lv_arclabel.c/.h`, `lv_observer.h`, `lv_label.h`, `lv_translation.h`.
  - `lv_obj.c` (default flags), `lv_display_private.h` (`LV_INV_BUF_SIZE`).
  - The LVGL v9.6 migration page.

---

## 0. Concept review and decision

### 0.1 Scores

Scale: 10 is best. For "implementation effort", 10 means the least effort.

| Criterion | Calm | Rim-Centric | Expressive |
|---|---|---|---|
| Round-geometry correctness | **9.** I re-derived all 7 keyboard layouts with the 9.5 `update_map` rounding, and every row fits w(d). The only error is the PWR ring at r 166–170, which crosses the 168 px margin. | **6.** Ring sectors are 50.6 px wide at r 145 but only 40–43 px at their inner edge. The core keys end at r 114 and the ring touch band starts at r 116, which leaves a 2 px gap and invites mis-hits. The south arclabel needs a negative centre offset, but the API takes `uint32_t`. | **8.** I verified the flick-keyboard rows (60×53 and 52×53 keys, worst corner r 166.9), the satellites and the cards. The header back zone lies partly outside the circle, which is harmless. |
| Touch usability | **8.** Every target is ≥ 50 px. Keys get +3 px of hit area on each side from the gap expansion, and there are no hidden gestures. The cost is 2 taps per letter. | **5.** The rim tap/scrub that opens the Mode Ring is exactly where a holding thumb rests. The CST816 is single-touch, so a resting thumb also blocks the orb. Ring letters sit under the bezel lip. | **7.** Keys and satellites are big. But flicks need learned directional slides, the "down" petal is hidden under the finger, and the swipe carousel is invisible. |
| Performance realism | **8.** Steady animation stays ≤ 50k px, the orb redraws as one area, and nothing creates layers. One bug: showing or fading the 336×336 `lv_arc` indicators invalidates 113k px per frame. | **7.** Arclabels are static, which is fine. But the lens list calls `lv_obj_set_width` on every row on every SCROLL event, forcing a flex relayout per frame. The Mode Ring disc is a 37k px alpha fill. | **6.** Per-bar invalidation only works if the private `LV_INV_BUF_SIZE` is set to 64 for the whole build. T1/T6 ignore the 64 px ext-draw: the orb area is 304² ≈ 92k px, and with two positions per frame it exceeds 100k. |
| Beauty | **6.** Clean, but generic. The orb is the only expressive element and the rim is barely used. | **8.** The most round-native: arc titles, caps, activity ring, translator poles. | **10.** A character with moods. Clearly the strongest emotional design. |
| Consistency | **9.** 9 templates, one status slot, and PWR means "back" everywhere. | **6.** The ring opens three different ways, and the caps change meaning from screen to screen. | **6.** There are four navigation paths (swipe, bottom cap, PWR ring, header), and Settings is both a carousel page and a ring item. |
| Implementation effort | **8.** Uses the stock `lv_keyboard` with custom maps plus one custom-draw orb. | **3.** Needs a custom ring-keyboard widget, a rim catcher with exclusions, poles, caps and the lens list. | **3.** Needs 14 expression presets with eyelids and blending, petals, a carousel, a ring screen and arc toasts. |
| **Total** | **48** | **35** | **40** |

### 0.2 Decision

**Base: Calm.** It is the only concept that is geometrically correct and realistic for this bus, can be built with stock LVGL, and has no gesture that can collide with scrolling or hold-to-talk. Its weakness is beauty, and that is what the grafts fix.

| Grafted idea | From | Where in this spec |
|---|---|---|
| Minimal face on the orb (eyes + speaking mouth, 7 expressions, no eyelids) driven by the state and an LLM `mood` field; gaze follows touches and turns to the target language | Expressive | §5.3, §6 A2–A4 |
| Waveform placeholder in the user bubble before the transcript arrives | Expressive | §5.4, A17 |
| Translator bubble appears centred and slides to the speaker's side | Expressive | §5.5, A18 |
| "Не тот язык" (wrong language): re-run the stored clip with the direction forced | Expressive | §3.6, §5.5 |
| In translator mode the two side buttons become **language chips** | Expressive | §3.4 |
| Digit on each phone-keypad group (level-2 R3), giving digits in 2 taps without changing page | Expressive (flick ↓ = digit) | §4.3 |
| `lv_arclabel` titles along the top rim on sub-screens | Rim | §3.1 |
| One rim widget at r 164–168 for recording time, a busy comet, the PWR-hold ring and the scroll thumb | Rim (activity ring) | §3.1, §7.4 |
| **Hold a language chip = talk with that source language forced** | Rim (hold a pole) | §5.5 |
| "Show to the other person": 180° flip of the translation in the reader | Rim (partner view) | §3.6, A34 |

Ideas I rejected:

| Idea | From | Why |
|---|---|---|
| Rim tap/scrub Mode Ring, rim gestures | Rim | They are invisible, sit in the grip zone, and a single-touch controller is blocked by a resting thumb. |
| Ring keyboard | Rim | Sectors are 40 px at the inner edge, there is a 2 px dead band to the core, it needs a custom widget, and Russian words alternate between the alphabet halves. |
| Flick-petal keyboard | Expressive | Directional slides on 60 px keys near the rim, the "down" petal hidden under the finger, and CST816 continuous reporting is still unverified. The two-step layout is error-free and needs no learning. |
| Swipe carousel between modes, tileview | Expressive | Invisible, and a tileview means full-screen scrolling at 129.6k px per frame. |
| Per-bar invalidation, `LV_INV_BUF_SIZE` override | Expressive | Not needed: the orb redraws as one ≤ 41.6k px area. |
| Hold-to-confirm for destructive actions | Rim, Expressive | A two-button dialog is clearer; PWR = cancel. |

### 0.3 Errors found in the concepts and fixed here

1. **Full-size `lv_arc` indicators (Calm A17/A21, Rim activity ring).**
   - Problem: hiding, unhiding, restyling or fading `opa` on a 336×336 arc invalidates its whole 113k px box. Calm's 300 ms scroll-thumb fade therefore costs about 18 frames of 113k px.
   - Fix: one custom widget `ui_rim` that invalidates only the changed arc spans (`lv_draw_arc_get_area`), §7.4.
2. **Runtime control flags are lost on every keyboard map swap.**
   - Cause: in 9.5, `lv_buttonmatrix_set_ctrl_map()` copies the whole ctrl map into the widget (`memcpy`), so DISABLED or CHECKED set at runtime disappear.
   - Fix: `kb_apply()` re-applies the dynamic flags after every swap (§4.5).
3. **Calm keyboard position.** The `lv_keyboard` constructor calls `lv_obj_align(LV_ALIGN_BOTTOM_MID)`, so `lv_obj_set_pos(kb, 18, 115)` would offset it from the bottom-middle. Use `lv_obj_align(kb, LV_ALIGN_TOP_LEFT, 18, 115)`.
4. **Calm PWR-hold ring** at r 166–170 goes outside r 168. It moves to `ui_rim` with band r 162–168.
5. **Calm QR.** Calm used 5 px/module with a 22 px margin. The final spec uses 6 px/module with a true 4-module quiet zone (24 px) on a 222 px white panel (§3.10).
6. **Calm's hex keypad had no way out** to the normal keyboard. Added an "Aa" toggle (§3.8).
7. **Expressive T6/T7 area estimates** forgot the 64 px ext-draw. The final orb never uses ext-draw: its box contains every pixel it draws (§7.4).
8. **`LV_DPI_DEF 194` (Calm)** silently changes every `lv_dpx()`-based metric, for example the arc hit tolerance. Keep the default of 130; all sizes in this spec are in px.
9. **No theme is used** (`LV_USE_THEME_DEFAULT 0`), so the textarea cursor, arc parts and keyboard items have no defaults at all. Every part gets an explicit style (§7.5).
10. **Label long-mode names:** 9.5 uses `LV_LABEL_LONG_MODE_WRAP` / `_DOTS`.

### 0.4 Coordinates and fit rules

**Coordinates and angles:**
- Origin is the top-left corner. Centre C = (180,180).
- Usable radius R = 168, which leaves a 12 px rim margin.
- Angles follow LVGL: 0° = 3 o'clock, increasing clockwise (90° = 6 o'clock, 270° = 12 o'clock).
- "x a–b" means left edge a and right edge b, exclusive, so the width is b − a. With LVGL, `lv_obj_set_pos(a, …)` and `set_size(b − a, …)`.

**Fit rules:**
- Chord at vertical offset d from the centre: `w(d) = 2·sqrt(168² − d²)`.
- A rectangle fits when its farthest corner is ≤ 168 from C.
- A rounded rectangle or stadium fits when the farthest corner-circle centre plus the corner radius is ≤ 168.
- A circle fits when |centre − C| + r ≤ 168.
- Invisible hit zones may extend past the circle.

| d | 0 | 30 | 38 | 50 | 60 | 65 | 70 | 80 | 91 | 100 | 110 | 120 | 130 | 140 | 144 | 150 | 160 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| w(d) | 336.0 | 330.6 | 327.3 | 320.8 | 313.8 | 309.8 | 305.4 | 295.5 | 282.4 | 270.0 | 254.0 | 235.2 | 212.8 | 185.7 | 173.1 | 151.3 | 102.4 |

**Physical scale and touch targets:**
- The panel has about 194 px/in (7.66 px/mm), so 50 px ≈ 6.5 mm.
- Minimum visible target is 50×50 px. Neighbouring targets are ≥ 4 px apart.
- I checked every coordinate in this document numerically against these rules.

---

## 1. Visual language

### 1.1 Neutral tokens

The background is pure black: it hides the panel edge and lets the orb glow. Every "glow" colour is a **pre-mixed opaque colour**, so the renderer never alpha-blends large areas.

| Token | Hex | On panel (RGB565) | Use |
|---|---|---|---|
| `bg` | `#000000` | `#000000` | screen background (opaque on every screen) |
| `surface-1` | `#111316` | `#101010` | list rows, function keys, cards, translator bubbles, side buttons |
| `surface-2` | `#1A1D22` | `#181C20` | letter keys, user bubble, arc track, text field |
| `surface-3` | `#2C3138` | `#283038` | pressed keys, buttons and rows |
| `text-1` | `#F2F4F7` | | primary text, about 19:1 on bg |
| `text-2` | `#A6ADB8` | | secondary text, about 9.3:1 |
| `text-3` | `#6B7380` | | disabled and decorative only, about 4.4:1 |
| `ink` | `#0B0D10` | | text, icons and **eyes** on bright accent fills |
| `qr-light` / `qr-dark` | `#FFFFFF` / `#000000` | | QR panel |

### 1.2 Accents

Each accent has six steps. The core is a simple vertical gradient from core-top to core-bottom (`LV_GRAD_DIR_VER`, no complex gradients). Each step is a fixed multiple of the core-top colour:
- core-bottom = ×0.78
- mid = ×0.55
- halo1 = ×0.30
- halo2 = ×0.14
- dim fill = ×0.22

| Accent | Meaning | core-top | core-bottom | mid | halo1 | halo2 | dim |
|---|---|---|---|---|---|---|---|
| `assistant` | Assistant mode idle/thinking | `#7C9CFF` | `#617AC7` | `#44568C` | `#252F4D` | `#111624` | `#1B2238` |
| `translator` = `lang-a` | Translator mode, language A ("Мой язык") | `#FFA94D` | `#C7843C` | `#8C5D2A` | `#4D3317` | `#24180B` | `#382511` |
| `lang-b` | language B ("Собеседник") | `#6EC8FF` | `#569CC7` | `#3C6E8C` | `#213C4D` | `#0F1C24` | `#182C38` |
| `listen` | microphone open (both modes) | `#34E0C3` | `#29AF98` | `#1D7B6B` | `#10433B` | `#071F1B` | `#0B312B` |
| `speak` | device speaking (both modes) | `#B794FF` | `#8F73C7` | `#65518C` | `#372C4D` | `#1A1524` | `#282138` |
| `error` | failure | `#FF5C5C` | `#C74848` | `#8C3333` | `#4D1C1C` | `#240D0D` | `#381414` |
| `warning` | low credit, no speaker, 25 s recording | `#FFD166` | `#C7A350` | `#8C7338` | `#4D3F1F` | `#241D0E` | `#382E16` |
| `success` | saved, connected | `#3DDC84` | `#30AC67` | `#227949` | `#124228` | `#091F12` | `#0D301D` |
| `offline` | no network | `#6B7380` (outline only) | – | – | – | – | – |

- State colours (listen, speak, error) mean the same thing in both modes. Mode colours only tint the orb when it is idle or thinking.
- Amber and sky stay distinguishable under deuteranopia.

### 1.3 Typography

**Font: Inter (SIL OFL 1.1), static instances** `Inter-Medium.ttf` and `Inter-SemiBold.ttf` from the `Inter-4.1.zip` release. Check the `extras/ttf/` path after unzipping.
- Do not feed lv_font_conv a variable font.
- Commit `OFL.txt` to `main/ui/fonts/`.
- Inter covers Latin, Latin Extended and full Cyrillic, and is narrower than Montserrat, which matters on a chord.

| LVGL font | Source | px | Line height | bpp | Glyph set | Icons | Used for |
|---|---|---|---|---|---|---|---|
| `ui_16` | Inter-Medium | 16 | 20 | 4 | TEXT | I_16 | captions, subtitles, status line, user transcript, hints, "пробел/space" key, translator source line |
| `ui_20` | Inter-Medium | 20 | 26 | 4 | TEXT | I_FULL | replies, list titles, pill, buttons, arc titles, keyboard level 1 |
| `ui_28` | Inter-SemiBold | 28 | 34 | 4 | TITLE | I_28 | keyboard level 2, digits and hex keys, short translations, reader, onboarding titles, orb glyphs |
| `ui_48` | Inter-SemiBold | 48 | 56 | 4 | DIGITS | – | volume/brightness value, ambient clock |

Glyph ranges and generation (the developer runs these; nothing is installed by this spec):

```sh
R_TEXT=0x20-0x7E,0xA0-0x17F,0x218-0x21B,0x400-0x4FF,0x2010-0x2027,0x2030,0x2039-0x203A,0x20AC,0x20B4,0x20B8,0x20BD,0x2116,0x2122,0x2190-0x2193,0x2212
R_TITLE=0x20-0x7E,0xA0-0xFF,0x400-0x45F,0x490-0x491,0x2013-0x2014,0x2018-0x201E,0x2022,0x2026,0x2116
R_DIG=0x20,0x25,0x2B-0x3A,0x2212
# Font Awesome 5 Free Solid 5.15.x fa-solid-900.ttf (font OFL 1.1, icons CC BY 4.0)
I_FULL=0xF00C,0xF00D,0xF011,0xF013,0xF023,0xF028,0xF029,0xF04B,0xF04D,0xF053,0xF054,0xF05A,0xF06E,0xF070,0xF071,0xF078,0xF084,0xF0AC,0xF0E7,0xF11C,0xF130,0xF131,0xF185,0xF1AB,0xF1DA,0xF1EB,0xF1F8,0xF240-0xF244,0xF2F1,0xF362,0xF3CD,0xF4AD,0xF544,0xF55A,0xF6A9
I_16=0xF00C,0xF023,0xF053,0xF054,0xF071,0xF0E7,0xF1EB,0xF240-0xF244,0xF6A9
I_28=0xF00C,0xF00D,0xF028,0xF04D,0xF053,0xF071,0xF0AC,0xF130,0xF185,0xF1EB,0xF55A,0xF6A9

lv_font_conv --bpp 4 --size 16 --no-compress --format lvgl --font Inter-Medium.ttf -r $R_TEXT \
  --font fa-solid-900.ttf -r $I_16 --lv-font-name ui_16 -o main/ui/fonts/ui_16.c
lv_font_conv --bpp 4 --size 20 --no-compress --format lvgl --font Inter-Medium.ttf -r $R_TEXT \
  --font fa-solid-900.ttf -r $I_FULL --lv-font-name ui_20 -o main/ui/fonts/ui_20.c
lv_font_conv --bpp 4 --size 28 --no-compress --format lvgl --font Inter-SemiBold.ttf -r $R_TITLE \
  --font fa-solid-900.ttf -r $I_28 --lv-font-name ui_28 -o main/ui/fonts/ui_28.c
lv_font_conv --bpp 4 --size 48 --no-compress --format lvgl --font Inter-SemiBold.ttf -r $R_DIG \
  --lv-font-name ui_48 -o main/ui/fonts/ui_48.c
```

What the TEXT set covers:
- ASCII, Latin-1 and Latin Extended-A (de/fr/es/it/pt/pl/cs/tr/hu), Romanian ș/ț;
- the whole Cyrillic block (ru/uk/be/kk/sr/bg);
- general punctuation (– — ‘ ’ “ ” „ • …), ‹ ›, € ₴ ₸ ₽, №, ™, arrows and the minus sign.

Build notes:
- `--no-compress` avoids per-glyph decompression CPU.
- Pin lv_font_conv in `tools/package.json` and commit the generated `.c` files.
- Read the missing-glyph warnings once. Inter may lack U+20B8 or U+0218–021B; drop whatever it reports.
- Estimated flash: about 150–220 KB in total, depending on Inter's kerning table. Fonts are `const` rodata and cost no RAM. Measure after generating.

**Text sanitiser.** It runs on every model string before a label sees it:
1. Strip markdown: `#`, `*`, `_`, backticks, `>`. A leading `- ` becomes `• `.
2. Remove emoji: U+1F000–1FAFF, U+2600–27BF, U+FE0F, U+200D.
3. Map U+2028/2029 to `\n` and collapse runs of 3 or more newlines.
4. Drop any code point that `ui_20` lacks (checked with `lv_font_get_glyph_dsc`).

`LV_USE_FONT_PLACEHOLDER 1` stays on, so anything missed shows as a box.

For translator targets in scripts we do not ship (zh/ja/ko/ar/hi/th), the schema adds a `display_text` romanisation. The audio still uses the real language.

**Typed strings in the prompt:**
- The assistant system prompt asks for "plain text, no markdown, no emoji, ≤ 60 words".
- Assistant JSON: `{"transcript": str, "mood": "neutral|happy|sad|surprised|thinking", "reply": str}`, in exactly this field order so the user bubble and the face can react before the reply streams.
- Translator JSON: `{"src_lang": "<A>|<B>", "transcript": str, "translation": str, "display_text"?: str}`.

### 1.4 Icons

All icons are Font Awesome 5 Free Solid glyphs merged into the text fonts. There are no bitmaps in the UI, so there is no image decoder and no image cache. Each icon gets a macro in `ui_sym.h`, for example `#define SYM_MIC "\xEF\x84\xB0"`.

| Code | Name | Use | Code | Name | Use |
|---|---|---|---|---|---|
| F130 | microphone | mic test | F131 | microphone-slash | mic fault |
| F028 | volume-up | voice on, volume | F6A9 | volume-mute | text-only, no speaker |
| F013 | cog | settings button | F00C | check | OK key, selected row |
| F00D | times | cancel, cancel-armed orb | F04D | stop | latched-recording mark |
| F04B | play | voice preview, replay | F053 / F054 / F078 | chevron-left / right / down | title, row chevron, pill |
| F362 | exchange-alt | "⇄", wrong language | F2F1 | sync | retry, flip for the other person |
| F1AB / F0AC | language / globe | translator, UI language | F544 | robot | model row |
| F4AD | comment-dots | assistant row | F11C | keyboard | "type a question" |
| F1EB | wifi | Wi-Fi, offline orb | F084 / F023 | key / lock | API key, secured network |
| F029 / F3CD | qrcode / mobile-alt | phone setup | F185 | sun | brightness |
| F06E / F070 | eye / eye-slash | reveal password | F071 / F05A | exclamation-triangle / info-circle | errors / about |
| F55A | backspace | ⌫ key | F1DA / F1F8 | history / trash | history / clear |
| F240–F244 / F0E7 | battery levels / bolt | battery, charging | F011 | power-off | power row |

Wi-Fi signal strength is drawn as 4 bars in the row's draw event, not with glyphs.

### 1.5 Shape, spacing, states

- **Radii:** keys 10, list rows 20, bubbles 16–18, cards 20, text field 14, pills and stadiums h/2, round buttons are circles.
- **Spacing scale:** 4 / 6 / 8 / 12 / 16 / 24 px.
- **Borders and shadows:** no shadows. Borders are used only for the 3 px language stripe on translator bubbles and the 3 px border on the language chips.
- **Pressed state:** fill changes to `surface-3`, or to accent-mid for accent fills, through one shared 80 ms `lv_style_transition_dsc_t`. Nothing scales on press.
- **Disabled state:** text changes to `text-3`; the fill is unchanged.

### 1.6 Motion tokens

| Token | Value | Use |
|---|---|---|
| `t_tap` | 80 ms linear | press feedback |
| `t_fast` | 150 ms ease_in / ease_out | chrome fades |
| `t_base` | 220 ms ease_out | message appear, state entries |
| `t_slow` | 360 ms ease_in_out | orb hero ↔ compact |
| `t_morph` | 400 ms ease_in_out | colour morphs |
| `t_breath` | 3000 ms each way, ease_in_out, reverse, infinite | idle halo breathing |
| `t_blink` | close 70 ms ease_in, hold 40 ms, open 110 ms ease_out | eye blink |
| ticker | 33 ms (30 Hz) `lv_timer` | all continuous orb motion |

Motion is calm: no bounce and no overshoot, with two exceptions: a 110 ms ease_out "settle" after the press-in, and the padlock's shackle on the lock screen (§3.16), which clicks shut and springs open with an ease_out_back (about 10 % past the end).

---

## 2. Screens and navigation

### 2.1 Screen inventory

There are 10 templates: `T_HOME`, `T_LIST`, `T_TEXT`, `T_ADJUST`, `T_QR`, `T_DIALOG`, `T_STEP`, `T_AMBIENT`, `T_READ`, `T_TOUCH`.

| ID | Screen | Template | Purpose | Entered from | Exits to |
|---|---|---|---|---|---|
| S0 | Boot splash | `T_HOME` (orb only) | orb forms, eyes open (A31) | power-on | O1 (first boot) or H |
| O1–O5 | Onboarding | `T_STEP` (+ `T_QR`/`T_LIST`/`T_TEXT`) | language → network → key → speaker → first try | first boot, factory reset | H |
| H | Home, Assistant face | `T_HOME` | orb + current-session chat | boot, back from anything | – |
| H′ | Home, Translator face | `T_HOME` | same screen object, amber accent, language chips | M | – |
| HIST | History | `T_READ` | full-height reading of the stored conversation of the current mode; "Новый разговор" at the top | tap a message (assistant), M → История | back |
| RD | Translation reader | `T_READ` | large translation for the other person, flip 180°, wrong language, replay | tap a translator bubble | back |
| M | Mode sheet | `T_LIST` | mode, languages, direction, type, history, new chat, volume, voice, settings | pill | H |
| L / LP | Languages / language picker | `T_LIST` | "Мой язык", "Собеседник", "Направление" → radio list | M; tap a chip opens LP directly | caller |
| S | Settings | `T_LIST` | all settings | settings button, M | H |
| W | Wi-Fi networks | `T_LIST` | scan, select | S, O2, "Нет Wi-Fi" pill | S / O3 |
| WC | Wi-Fi connecting | `T_STEP` | progress and result | W → K | W / caller |
| K | Text entry | `T_TEXT` | round keyboard | W, PE, AI, M ("Написать текстом"), O3 | caller |
| AI | AI model picker | `T_LIST` | curated audio-capable models | S, PE, fault pill | S |
| V | TTS voice picker | `T_LIST` | TTS model + voices with preview | S | S |
| A | Volume / Brightness | `T_ADJUST` | 270° arc adjuster | S, M | caller |
| Q | Phone setup | `T_QR` | SoftAP + QR + portal progress | S, O2, O3, fault pill | caller / H |
| P / PE | Provider & keys / endpoint editor | `T_LIST` | chat / STT / TTS base URL + key + model | S, fault pill | S |
| D | Diagnostics & About | `T_LIST` | versions, network, latency, tests, licences | S | S |
| D.touch | Touch test | `T_TOUCH` | dot + coordinates at each sample | D | D (✕ circle) |
| C | Confirm dialog | `T_DIALOG` | destructive actions | S, D, HIST | caller |
| Z | Ambient | `T_AMBIENT` | clock + dot orb | idle timeout | H on wake |
| X | Screen off | – | backlight 0, LVGL paused | timeout, power off on USB | H on wake |
| LK | Lock screen | overlay on the top layer (§3.16) | clock, date, battery ring; touch and BOOT refused | PWR short, anywhere | PWR short: the screen that was locked |

Error states are not separate screens. They appear in the **pill**, the **orb** and a **card in the chat**, and they route to W, Q, P or AI (§3.14, §5.6).

### 2.2 Navigation map

```
Power ─► S0 ─┬─ first boot ─► O1 Language ─► O2 Network ─┬─► Q Phone setup (Wi-Fi + key + model) ─┐
             │                                           └─► W ─► K password ─► WC ───────────────┤
             │                O3 Key (skipped if Q sent it) ─► Q | K (hex) ──────────────────────┤
             │                O4 Speaker test ─► O5 First try ───────────────────────────────────┤
             ▼                                                                                   ▼
   ┌───────────────────────────── H  HOME  (Assistant ⇄ Translator) ─────────────────────────────┐
   │ pill ─► M ─┬► mode radio (Ассистент / Переводчик) ─► H (colour morph A21)                    │
   │            ├► Языки ─► L ─► LP          ├► Направление (Авто / A→B / B→A)                  │
   │            ├► Написать текстом ─► K ─► H (THINKING)   ├► История ─► HIST                    │
   │            ├► Новый разговор ─► H empty (A15r)         ├► Громкость ─► A                     │
   │            ├► Ответ голосом / Озвучивать перевод (toggle)  └► Настройки ─► S                 │
   │ Assistant: left button = voice toggle, right button = S                                     │
   │ Translator: left chip A, right chip B: tap = LP for that slot, hold = talk in that language │
   │ tap a message ─► HIST (assistant) / RD (translator)                                         │
   │ fault pill ─► M, whose first row ─► the fixing screen (W | Q | P | AI | D)                  │
   │ S ─► A Volume | A Brightness | W ─► K ─► WC | Q | AI ─► K | V | P ─► PE ─► K | D ─► C       │
   │ idle 30 s ─► dim 25 % (face SLEEPY) · +60 s ─► Z · +5 min ─► X                              │
   └─────────────────────────────────────────────────────────────────────────────────────────────┘
Global: PWR short = lock / unlock (LK, §3.16) · PWR hold 3 s = power off
        back = title zone, ✕ / "Отмена" / "Готово", or swipe right on T_LIST / T_READ / T_QR / T_ADJUST / O-steps
        BOOT = talk; from T_LIST/T_READ/T_QR/T_ADJUST it jumps to H/H′ and records
        BOOT double tap = Assistant ⇄ Translator (A21 morph + toast)
        (BOOT ignored in K, C, O1–O4 and while locked)
Maximum depth: H → S → P → PE → K (4 levels).
```

### 2.3 Physical buttons and touch, per context

**Roles of the two keys** (also on O5, §5.7):

| Key | Gesture | Action |
|---|---|---|
| BOOT | hold (≥ 400 ms) | push-to-talk: records from press-down, release sends |
| BOOT | tap (< 400 ms) | hands-free talk: the recording starts at press-down and latches 300 ms after the release (the double-tap window); the next press sends |
| BOOT | double tap (second press ≤ 300 ms after the first release) | Assistant ⇄ Translator: the first tap's recording is discarded silently, the mode morphs (A21), toast "Переводчик" / "Ассистент". While the app is not ready (no Wi-Fi, no key), the tap's "not ready" card waits for the window, so a double tap only switches |
| PWR | short (released < 600 ms) | **lock** when unlocked, **unlock** when locked (§3.16). PWR is no longer "back": the title zone, ✕ / "Отмена" / "Готово" and the right swipe are |
| PWR | hold 3 s | power off on battery (GPIO7 low); on USB: screen off + toast "Питание от USB". The rim POWER ring appears at 600 ms and fills until 3000 ms, also over the lock screen |

- Tap/hold threshold: 400 ms, the same as `long_press_time`. Double-tap window: 300 ms (`APP_DOUBLE_MS`) after the first release; a single tap's audio is never delayed (it records from the press), only its latched look waits.
- Locking ends whatever runs (recording discarded, request aborted, speech stopped) without a status text, and no press carries over into the locked state.

| Context | BOOT hold (≥ 400 ms) | BOOT tap (< 400 ms) | PWR short | PWR hold 3 s | Touch |
|---|---|---|---|---|---|
| H / H′ idle | records from press-down; release sends | latched recording (double tap: mode) | lock | power off on battery; on USB: screen off + "Питание от USB" | orb = talk; pill = M; side buttons per mode |
| Recording (hold) | release = send | – | lock (recording discarded) | – | slide beyond the cancel radius arms cancel |
| Recording (latched) | press = send | press = send | lock (discarded) | – | orb tap = send |
| Uploading / thinking | ≥ 400 ms: abort and start a new recording | ignored (orb pulse, hint "Держите — заново") | lock (request aborted) | – | orb: same as BOOT |
| Speaking | press stops speech; still held at 400 ms → new recording (barge-in) | stop speech | lock (speech stopped) | – | orb, voice button and chips stop speech |
| Translator chip held (≥ 400 ms) | – | – | lock (discarded) | – | release sends with the source forced to that chip |
| `T_LIST` / `T_READ` / `T_QR` / `T_ADJUST` | jump to H/H′ and record (hold semantics) | jump to H/H′, latched | lock | power off | title zone or swipe right = back |
| K | ignored (toast "Сначала завершите ввод") | ignored | lock (the draft stays) | power off | keys; ✕ = cancel |
| C | ignored | ignored | lock | – | buttons |
| O1–O5 | ignored | ignored | lock | power off | buttons; swipe right = previous step |
| Dimmed (25 %) / Z | wake + act | wake + act | wake + lock | – | the first touch only wakes and is swallowed (`lv_indev_wait_release`) |
| X (screen off) | wake to H/H′ and record at once | wake + latched | wake only | power off | wake (swallowed) |
| **LK locked** | refused: padlock shakes, hint "Нажмите PWR" | refused (same) | **unlock** (dimmed or dark too: one press) | power off | refused (same); a dark lock screen lights |

### 2.4 Why the inputs never conflict

- **Modes change only through the pill or a BOOT double tap.** Switching is two taps (pill → row) or BOOT twice. There is no swipe, no tileview, no rim menu and no CST816 gesture code (all are ignored). Swipe navigation would be invisible, would compete with chat scrolling, and a tileview would redraw 129.6k px per frame.
- **BOOT tap vs double tap.** The tap records from press-down, as before; only its latched state waits for the 300 ms window. A second press inside the window discards that recording without a word, so a double tap never sends anything. A press of the orb inside the window ends it at once (the tap was single, the orb sends it).
- **Lock vs everything.** The lock screen is a modal overlay on the top layer: it catches every touch (`lv_indev_wait_release`, so nothing below gets a release either), and the app refuses BOOT through `ui_boot_down()`. Navigation underneath (an app `UI_GOTO`) never removes it; only PWR (or the console) unlocks.
- **Hold-to-talk vs scrolling.**
  - The orb is a **sibling** of the chat container, never a child.
  - Flags: `LV_OBJ_FLAG_ADV_HITTEST` (circle radius core_r + 10). `LV_OBJ_FLAG_PRESS_LOCK` stays on (the default for children), so the orb stays pressed while the finger slides out to arm cancel.
  - Cleared flags: `SCROLLABLE`, `SCROLL_CHAIN_HOR/VER`, `GESTURE_BUBBLE` (every `lv_obj` is scrollable by default in 9.5).
  - The screen itself is not scrollable.
  - Result: a drag that starts on the orb never scrolls, and a drag that starts on the chat never talks. The orb hit circle (compact: y 250–330) never overlaps the chat viewport (y 80–244).
- **Scrolling vs tapping.** LVGL's 10 px scroll limit means a scroll never fires CLICKED. A tap that opens HIST or RD is ignored if `LV_EVENT_SCROLL_END` happened less than 300 ms earlier, which covers "tap to stop a fling".
- **Swipe-right-back** exists only on screens whose containers scroll vertically, so a horizontal drag never starts a scroll and becomes `LV_EVENT_GESTURE` (default minimum 50 px, velocity 3). The arc adjuster and the keyboard clear `GESTURE_BUBBLE`. After a gesture the handler calls `lv_indev_wait_release(lv_indev_active())`, so the row under the finger gets no CLICKED. It does not exist on H, K or C. The onboarding steps (which do not scroll) have it as their only "previous step", since PWR locks now. Every other swipe has a visible equivalent (title zone, ✕, "Готово"), so nothing is hidden.
- **Chip tap vs chip hold.**
  - Recording is decided at 400 ms, and the 0.5 s mic pre-roll is prepended, so no speech is lost.
  - A release before 400 ms opens LP and the pre-roll audio is discarded. No network request is ever made.
- **BOOT is the fallback** if the CST816T drops "pressed" during a long stationary hold (to verify, §8). On-screen latched mode keeps working, and BOOT is never affected.

---

## 3. Geometry per screen

### 3.1 Shared chrome

**Arc title (all `T_LIST`, HIST, RD, Q screens):**

| Property | Value |
|---|---|
| Widget | `lv_arclabel` |
| Position / size | `lv_obj_set_pos(80, 14)`, size 200×58 |
| Centre | `lv_arclabel_set_center_offset_y(137)`. The content centre (180,43) + 137 = (180,180). The API is unsigned, so the object must sit above the centre. |
| Radius | 148 |
| Angles | `angle_start` 238, `angle_size` 64 |
| Text options | `LV_ARCLABEL_DIR_CLOCKWISE`; horizontal and vertical align `LV_ARCLABEL_TEXT_ALIGN_CENTER`; `LV_ARCLABEL_OVERFLOW_ELLIPSIS` |
| Font / colour | `ui_20`, `text-1` |
| Text | `LV_SYMBOL_LEFT " " title` |

Notes:
- **Fit:** the arc length at r 148 over 64° is 165 px, so the title text must be ≤ 147 px after the 18 px chevron (checked by the CI fit test).
- **Glyph band:** with a full 64° span the ends sit at x 102/258, y 41–67. List viewports start at y 76 and HIST at y 70.
- **Tuning:** set the radius in the simulator so that the outer glyph edge stays at r ≤ 162.
- **Cost:** arclabels draw each glyph rotated, so they are static: the text changes only when the screen is built.
- **Q screen:** the title must be ≤ 100 px (about 40°), because its ends must clear the QR panel top at y 58.

**Title back zone:**
- Transparent clickable `lv_obj` at x 96–264, y 10–68 (168×58), created after the arclabel.
- PRESSED: title colour changes to `text-2`. CLICKED: back.
- Part of it lies outside the circle, which is harmless for an invisible zone.

**Rim widget `ui_rim`** (one global object on `lv_layer_top()`):
- Position (12,12), size 336×336, style removed, not clickable, so touches pass through.

| Channel | Radius / width | Range | Colour | Used for |
|---|---|---|---|---|
| SCROLL | r 166, w 4 (band 164–168) | thumb inside 320° → 40° (80°); length = max(12°, 80°·view/content) | `text-3`, opa param | chat, lists, HIST, RD |
| RECORD | r 166, w 4 | from 270° clockwise, 360°·t/30 s | `listen`; `warning` from 25 s | recording time |
| BUSY | r 166, w 4 | 40° comet, 1.2 s per turn | `text-2` | scans, portal wait, model list fetch |
| POWER | r 165, w 6 (band 162–168) | from 270° clockwise, 0 → 360° over 2.4 s after a 0.6 s hold | `error` | PWR hold |

Every change invalidates only the old and new spans (`lv_draw_arc_get_area` → `lv_obj_invalidate_area`), never the 113k px box (§7.4).

While the lock screen is up (§3.16) the rim sits above its overlay and draws only POWER (`ui_rim_power_only()`), so the 3 s power-off ring shows over the battery ring; the other channels of the screen below are hidden.

**Side buttons on H / H′:**
- Circles Ø52 centred at (100,292) and (260,292): |(80,112)| + 26 = 163.6, OK.
- Each spans about ±10.9° around 125.5° and 54.5°.

### 3.2 Home, empty state

Used when the current session has no messages.

| Element | Geometry | Fit | Target |
|---|---|---|---|
| Pill (mode, tap → M) | stadium x 110–250, y 36–86 (140×50), transparent; `surface-1` when pressed | 152.2 OK | 140×50 |
| Pill content | `ui_20`: "Ассистент" + chevron-down (F078) at 16 px. Translator: "RU ⇄ EN" + chevron, with RU in `lang-a` and EN in `lang-b` (`lv_label_set_recolor`). Width ≤ 124 | OK | – |
| Status line (replaces the pill when not idle) | `ui_16` `text-2`, centre (180,61), w ≤ 200 (x 80–280, y 51–71) | 163.2 OK | – |
| Orb, HERO preset | box x 78–282, y 90–294 (204×204), centre **(180,192)**, max drawn radius 99 | 111 OK | hit circle r 80 |
| Left side button | Assistant: voice toggle F028/F6A9. Translator: chip A | 163.6 OK | 52 |
| Right side button | Assistant: settings F013. Translator: chip B | 163.6 OK | 52 |
| First-use hint (only while `s_hints > 0`) | two `ui_16` `text-2` lines centred at (180,306) and (180,326), each ≤ 100 px. RU "Держите шар" / "или BOOT", EN "Hold the orb" / "or BOOT". Translator: "Говорите" / "RU или EN" | 144.9 / 163.8 OK | – |
| Battery (§3.16) | `ui_16` label x 145–215, y 16–36 (70×20), centred: level glyph F240–F244 (or a `success` bolt F0E7 while charging) + "85%"; `text-2`, `error` below 15 %, `success` when full; hidden without a reading; not clickable, created after the pill | 167.7 OK | – |

- Clearance from the orb drawing to each side button: |(80,100)| − 26 − 99 = 3 px.
- The battery sits directly above the pill (its box ends where the empty pill starts, y 36). In the conversation layout it overlaps only the pill's transparent top band (y 24–36); the pill's label line starts at y 36, and a pressed pill's `surface-1` fill is drawn under the battery, which is created after it. The rim RECORD sweep (r 164–168) passes 1–3 px above its corners while recording.
- The hint sits 6 px below halo2 (y 290) and 4 px from each button.
- The orb's bounding box overlaps the button boxes. The buttons are created after the orb (so they are on top), and the orb hit test is circular, so the buttons win.

### 3.3 Home, conversation state (≥ 1 message in the current session)

| Element | Geometry | Fit |
|---|---|---|
| Pill | x 110–250, y 24–74 | 163.5 OK |
| Status line | centre (180,49), w ≤ 180 (x 90–270, y 39–59) | 167.3 OK |
| Chat viewport | x 54–306, y 80–244 (252×164). Flex column, `LV_DIR_VER`, scrollbar off, momentum + elastic | top corners 160.9 OK |
| Top fade | x 54–306, y 80–104: `bg #000`, `bg_opa 255` → `bg_grad` stop opa 0, VER | inside |
| Bottom fade | y 228–244, opa 0 → 255 | inside |
| Orb, COMPACT preset | box x 134–226, y 244–336 (92×92), centre **(180,290)**, max drawn radius 44 | 156 OK |
| Side buttons / chips | as in §3.1 | gap to the orb: 80 − 26 − 46 = 8 px |
| Scroll thumb | `ui_rim` SCROLL channel | – |

Message objects:
- Each message = a full-width transparent wrapper (252 × content height, flex ROW, `main_place` START, END or CENTER), plus the bubble, plus its label. That is 3 objects.
- **Assistant reply:** no bubble (calm style). `ui_20` `text-1`, width 252, `LV_LABEL_LONG_MODE_WRAP`, line 26 px, so 6 lines are visible.
- **User transcript:** `surface-2` bubble, radius 16, padding 8/12, `max_width 220`, right-aligned, `ui_16` `text-2`.
  - Before the transcript arrives, the bubble is 220×44 and shows the clip's RMS envelope: 32 bars, each 3 px wide with a 3 px gap (192 px in total), heights 4–28 px, colour `text-3`.
  - The envelope is drawn once in the bubble's `LV_EVENT_DRAW_MAIN`.
- **Gaps:** 6 px between a user bubble and its reply; 14 px between turns.
- **Divider:** after 15 min idle, a `ui_16` `text-3` divider "— новый разговор · 15:10 —" is inserted and the model context resets. Waking after ≥ 15 min returns H to the empty state.
- **Limit:** ≤ 20 messages live on H. Storage keeps 40 per mode in PSRAM, shown in HIST.
- **Chat history is RAM only, on purpose.** It is never written to flash (NVS or the `storage` partition), so a reboot or power cycle empties H, HIST and the RD history. This is a privacy choice, the same as the model context (protocol §4.3): transcripts and translations of other people's speech must not outlive the session on a device that is easy to pass around, and the flash is not encrypted. It also saves flash wear. "Новый разговор" and S → История → "Очистить" clear it earlier.
- **Scrolling:** a new message scrolls the chat to the bottom. A translator bubble or a card taller than the unfaded window (y 104–228) is scrolled so that its top sits just below the top fade, and the user scrolls down for the rest. A manual drag stops auto-scroll until the next talk press.

### 3.4 Translator face (H′)

The geometry is identical to §3.2/§3.3. The orb idle accent is `translator`.

**Language chips** (they replace the side buttons):
- Ø52 circles at (100,292) for A and (260,292) for B. **A is on the left everywhere**: in the pill order, the chip, bubble alignment and the eye glance.
- Style: fill `surface-1`, 3 px border in the language colour, `ui_20` code ("RU" or "EN") in the language colour.
- Pressed: fill = language dim.
- Held and recording: fill = language dim, border 5 px.
- Pulsing target (while the device speaks): outline 1 → 3 px, pad 1 px, so the outer radius is 30. Fit: |(80,112)| + 30 = 167.6, OK.

**Turn bubble:**
- `surface-1`, radius 18, padding 10/12, width ≤ 236.
- Language A: left-aligned with a 3 px LEFT border in `lang-a`. Language B: right-aligned with a 3 px RIGHT border in `lang-b` (`border_side`).
- Line 1: `ui_16`, "RU → EN" with the target code in the target colour.
- Line 2: transcript, `ui_16` `text-2`.
- Line 3: translation in `text-1`. It uses `ui_28` if it has ≤ 48 characters and every glyph exists in `ui_28`, otherwise `ui_20`.

### 3.5 HIST, history (assistant and translator)

| Element | Geometry | Fit |
|---|---|---|
| Arc title | "‹ История", text ≤ 120 px (≤ 47°) | ends above y 62 |
| Viewport | x 54–306, y 70–290 (252×220), same message styles as H; fades y 70–94 and 270–290 | corners 167.3 OK |
| First child | stadium "Новый разговор" 200×50 (x 80–280 within the content), `surface-1`, `ui_20`; tap → C "Начать новый разговор?" → H empty | 135.4 OK |
| Scroll thumb | `ui_rim` SCROLL | – |

- On open it scrolls to the tapped message (`lv_obj_scroll_to_view(child, LV_ANIM_OFF)`).
- Back: title zone or swipe right (PWR locks).
- Translator bubbles in HIST open RD on tap.

### 3.6 RD, translation reader

| Element | Geometry | Fit | Target |
|---|---|---|---|
| Arc title | "‹ RU → EN" (it is the direction label too) | OK | back zone |
| Text box | x 54–306, y 76–260 (252×184). `ui_28` (or `ui_20` if the glyph check fails), centred, scrolls vertically; 5 lines of 34 px | 163.4 OK | – |
| "Не тот язык" | circle Ø52 at (124,296), F362 | 154.8 OK | 52 |
| "Повторить" | circle Ø52 at (236,296), F04B; disabled without a speaker | 154.8 OK | 52 |
| "Показать собеседнику" | circle Ø52 at (180,318), F2F1 | 164.0 OK | 52 |

- Buttons are 8 px apart.
- **Flip** toggles `transform_rotation` 0 ↔ 1800 on the text box (pivot 50 %/50 %).
  - Cost: one ARGB8888 layer of 252×184×4 = 185 KB from the PSRAM heap, rendered only when the box is redrawn, which happens once (A34).
  - Scrolling is disabled while the text is flipped.
- **Wrong language** re-sends the cached clip with `src` forced to the other language.
- Tapping the text box also goes back.

### 3.7 List template (`T_LIST`)

| Element | Geometry | Fit | Target |
|---|---|---|---|
| Arc title + back zone | §3.1 | OK | 168×58 |
| Viewport | x 54–306, y 76–346 (252×270), flex column, gap 6, `pad_top 4`, `pad_bottom 134`, `LV_DIR_VER`, momentum, no snap, scrollbar off | – | – |
| Row | 252×64, radius 20, `surface-1` (pitch 70) | fully inside for row-centre offset \|d_c\| ≤ 79 (159.7 OK) | 252×64 |
| Scroll thumb | `ui_rim` SCROLL | – | – |

**Curved-edge behaviour** (no layers, no `clip_corner`):
- On `LV_EVENT_SCROLL`, each row gets style `opa = 255` for |d_c| ≤ 79, falling linearly to 0 at |d_c| = 119.
- Write the opa only when it changes by 8 or more.
- A row with opa < 128 loses `CLICKABLE`.
- The first row's centre starts at y 112, so rows sit at 80–144, 150–214 and 220–284 (3 full rows).

**Row anatomy:**

| Part | Position | Font / colour |
|---|---|---|
| Icon | centre (x+28, y+32) | `ui_20`, accent or `text-2` |
| Title | x+52, y+10, w ≤ 164, DOTS | `ui_20` `text-1` |
| Subtitle | x+52, y+38, w ≤ 184, DOTS | `ui_16` `text-2` |
| Right slot | centre (x+226, y+32) | chevron (`text-3`), check (accent, selected radio), "Вкл"/"Выкл" (`ui_16`), or 4 Wi-Fi bars + lock |

- Info rows: title in `ui_16` `text-2`, value in `ui_20` `text-1`, not clickable.
- Destructive rows: `error` icon and title.

**List contents** (titles ≤ 15 characters):

- **M "Режим":**
  0. *(only while the pill shows a fault)* the fault ("Ключ не принят", "Исправить" in the fault colour ›): opens its fixing screen (§3.14); back returns to M
  1. Ассистент (radio, "Отвечает на вопросы")
  2. Переводчик (radio, "RU ⇄ EN")
  3. *(translator only)* Языки ("Русский ⇄ English" ›)
  4. *(translator only)* Направление ("Авто" ›)
  5. Написать текстом ›
  6. История ›
  7. Новый разговор ("Забыть контекст")
  8. Громкость ("70 %" ›)
  9. Ответ голосом, or Озвучивать перевод in translator mode (Вкл/Выкл)
  10. Настройки ›
- **L:** "Мой язык ›", "Собеседник ›", "Направление ›" (Авто / A → B / B → A).
- **LP:** radio list of native names: Русский, English, Deutsch, Français, Español, Italiano, Português, Polski, Türkçe, Nederlands, Čeština, Svenska, Українська, Қазақша.
  - **Firmware today** offers the protocol §4.2 codes whose scripts the fonts draw: ru, en, de, fr, es, it, pt, pl, tr, uk (names in the UI language). zh, ja, ko and ar are left out until a font or the `display_text` romanisation covers them: the sanitiser drops every glyph `ui_20` lacks, so their transcripts and translations would be empty. A stored code outside the list (from an older firmware or `set tr_lang_b zh`) falls back to en (ru when the other slot already is en), and the fallback is written back to `tr_lang_a` / `tr_lang_b`.
  - The subtitle says "только текст" when the TTS has no voice for that language. MAI-Voice has 28 locales and no uk; kk is also text-only.
  - Picking the language already in the other slot swaps the two.
  - A new pair takes effect at once: the UI writes `tr_lang_a` / `tr_lang_b` immediately (not through the 1.5 s debounce) before it tells the app, so a chip held right after the pick uses it.
- **S "Настройки":**
  1. Wi-Fi (SSID / "Не подключено")
  2. Телефон ("Настройка по QR")
  3. Модель ИИ (short name)
  4. Голос (voice name)
  5. Ответ голосом (Вкл/Выкл)
  6. Динамик (Подключён/Нет; saved as `speaker`. Нет also turns voice replies off, Подключён turns them on)
  7. Громкость
  8. Яркость
  9. Экран и сон ("Затемнение 30 с"): sub-list with dim after 15/30/60 s, ambient after +30/60/120 s, off after 2/5/10 min
  10. Лицо шара (Вкл/Выкл, default Вкл; saved as `face`)
  11. Звуки (Вкл/Выкл)
  12. Язык меню (Русский/English)
  13. Провайдер (host)
  14. История ("Очистить" → C)
  15. О системе
  16. Выключение (C; disabled on USB with "только от батареи")
- **W "Wi-Fi":**
  - "↻ Искать снова" (`ui_rim` BUSY while scanning);
  - networks sorted by RSSI: SSID in `ui_20`, subtitle "Подключено"/"Защищена"/"Открытая", 4 bars (≥ −60 dBm `success`, −60…−75 `warning`, < −75 `error`) and a lock;
  - "Другая сеть…" (K, SSID);
  - "С телефона (QR)".
- **AI "Модель ИИ":**
  - Curated rows:
    - Gemini 3.1 Flash-Lite (`google/gemini-3.1-flash-lite`, default, "≈ $0.0005 / ответ");
    - Gemini 3.5 Flash-Lite ("≈ $0.0006 / ответ").
  - "↻ Загрузить список": `GET /api/v1/models?input_modalities=audio&output_modalities=text&sort=pricing-low-to-high`. Keep only models whose `supported_parameters` include both `response_format` and `structured_outputs`. Drop `:batch`, `-preview`, `~` aliases, negative prices, and models with a non-null `expiration_date` (but keep the current model, flagged). Show at most 12 rows.
  - "Свой ID…" (K EN, pre-filled `google/`, or Q).
  - Per-turn estimate: 320 audio + 300 prompt + 150 output tokens.
  - If the current model has an `expiration_date`, its subtitle reads "истекает 20.10" in `warning`.
- **V "Голос":**
  - Section "Модель озвучки": MAI-Voice 2.1 Flash / Gemini 3.8 Flash-Lite TTS / "Свой…".
  - Section "Голос": for MAI, Маша, Лев, Грант, Харпер (`ru-RU-*:MAI-Voice-2.1-Flash`); for Gemini, Kore, Puck, Charon, ….
  - Tapping a row selects it and plays "Привет! Так звучит мой голос." This is a paid call; the subtitle says "≈ $0.0005", and the action is disabled without a speaker.
- **P "Провайдер":**
  - Чат (host · model);
  - Распознавание ("как чат (аудио в модель)");
  - Озвучка (host · model);
  - Ключ API ("sk-or-…a9f3");
  - "Поиск в сети" (globe icon, no right slot), subtitle "Авто" / "Всегда" / "Выкл" (`ai_web` 1 / 2 / 0, protocol §2.8). A tap cycles Авто → Всегда → Выкл, like "Динамик" in S, and the value is saved through the debounced writer. "Поиск в интернете" is 187 px in `ui_20`, over the 164 px row title, hence the shorter title. Implemented order: Адрес, Ключ API, Модель ИИ, Поиск в сети, Проверить ключ, С телефона;
  - "Настроить с телефона";
  - "Проверить ключ": `GET /api/v1/key` shows "Лимит: осталось $4.20" or "Лимит не задан". Never the account balance: that needs a management key.
- **PE:** Адрес (URL), Ключ, Модель, "Как у чата" (toggle).
- **D "О системе":**
  - Info rows: firmware and slot (ota_0/1), ESP-IDF/LVGL versions, panel ID (reg 0x04), touch chip, MAC, IP, RSSI, uptime, free SRAM/PSRAM, last latency ("загрузка 0.9 с · ответ 1.6 с · голос 0.7 с"), last error.
  - Actions:
    - Микрофон (live level row, "Уровень 42 %", 100 ms, falls back 50 %/s)
    - Тест динамика (two short tones, 0.3 s, played only when tapped; "Громкость 0 %" toast at volume 0)
    - Динамик (Подключён/Нет, the same switch as in S)
    - The card actions "Проверить звук" and "Диагностика" and the "Микрофон ›" fault open D centred on these rows
    - Тест касания (D.touch)
    - Показать FPS
    - Показать подсказки (resets `s_hints` to 3)
    - Лицензии: LVGL MIT, Inter OFL, Font Awesome OFL/CC BY 4.0, plus the xiaozhi-esp32 MIT notice if its code is reused
    - Перезагрузить
    - Сброс настроек (C)

### 3.8 Text entry (`T_TEXT`), layout around the keyboard

| Element | Geometry | Fit | Target |
|---|---|---|---|
| Prompt label (what is entered + counter) | `ui_16` `text-2`, x 105–255, y 30–50, DOTS, e.g. "Пароль · HomeNet", "Ключ · 37/64" | 167.7 OK | – |
| Cancel ✕ | circle Ø50 at (76,86), `surface-1`, F00D | 165.2 OK | 50 |
| Text field | `lv_textarea` one-line, x 105–255, y 61–111 (150×50), radius 14, `surface-2`, `ui_20`, pad 12/14 | 135.4 OK | 150×50 |
| Right circle | Ø50 at (284,86). Secret fields: reveal (F06E/F070). Hex key field: "Aa" (switches hex ↔ full EN keyboard). Otherwise hidden | 165.2 OK | 50 |
| Keyboard | `lv_keyboard` x 18–342, y 115–324 (324×209) | §4.4 | ≥ 50 |

- About 13 characters are visible in the field; it scrolls to the cursor.
- Password mode uses "•" and shows each new character for 1.5 s.
- The field is not clickable, so the cursor always stays at the end.

### 3.9 Volume / brightness adjuster (`T_ADJUST`)

| Element | Geometry | Fit | Target |
|---|---|---|---|
| Arc | `lv_arc` x 30–330, y 30–330 (radius 150), bg angles 135° → 45° (270° through the top), width 16, range 0–100 (brightness 5–100), knob Ø32 (`pad_all 8`), `lv_obj_set_ext_click_area(arc, 17)` | ring r 134–150, knob ≤ 158 OK | hit ring r 117–167 (50 px thick) |
| Caption | `ui_16` `text-2` "Громкость"/"Яркость", centre (180,128) | OK | – |
| Value | `ui_48` "70%", centre (180,176), w ≤ 140 | OK | – |
| − / + | circles Ø52 at (144,254) and (216,254), step 5 | 108.3 < 117 (inside the arc's dead zone) OK | 52 |
| "Готово" | stadium x 120–240, y 290–340, `surface-1` | 164.5 OK | 120×50 |

- **Why "Готово" never collides with the arc:**
  - 9.5 `lv_arc` hit test: with `LV_DPI_DEF` 130 the angle tolerance at r 117 is 7.8°, so the arc accepts angles up to 52.8° and from 127.2°.
  - "Готово" spans 61°–119° from the centre, so the arc rejects those touches.
  - "Готово" is also created after the arc, so it is on top.
- Brightness applies live through LEDC (10-bit duty = round(1023·(v/100)^2.2), minimum 5 %).
- Volume is a software gain of (v/100)². A 60 ms tick plays on release when a speaker is present.
- Keep `change_rate` 720°/s.
- The arc clears `GESTURE_BUBBLE`.
- "Готово" or a right swipe goes back (PWR locks).
- NVS save is debounced 1.5 s and only happens in IDLE.

### 3.10 Phone setup (`T_QR`)

| Element | Geometry | Fit |
|---|---|---|
| Arc title | "‹ Телефон", ≤ 100 px | ends y ≤ 54 |
| QR panel | white rounded rectangle x 69–291, y 58–280 (222×222), radius 16 | 158.3 OK |
| `lv_qrcode` | size **174** at (93,82); dark `#000000`, light `#FFFFFF`; `lv_qrcode_set_quiet_zone(qr, false)` | inside the panel |
| Caption 1 | `ui_16` `text-1`, centre (180,296), w ≤ 220 | 167.3 OK |
| Caption 2 | `ui_16` `text-2`, centre (180,318), w ≤ 150 | 165.9 OK |
| Busy | `ui_rim` BUSY while waiting | – |

- **Scale:** a version-3 code has 29 modules × 6 = 174 px. The panel margin is 24 px = 4 modules, a true quiet zone. A version-2 code (25 × 6 = 150) gets an extra 12 px of light canvas margin.
- **Step 1: join the device's network.**
  - Payload `WIFI:T:WPA;S:Orb-7568;P:k7m2q9x4;;` (34 bytes) → version 3 at ECC M.
  - The SSID suffix is the last 2 MAC bytes. The password is 8 random characters from `23456789abcdefghjkmnpqrstuvwxyz`, new each session.
  - Captions: "Сеть: Orb-7568" / "Пароль: k7m2q9x4".
- **Step 2: open the portal.** Shown on `WIFI_EVENT_AP_STACONNECTED`.
  - Payload `http://192.168.4.1/` (19 bytes, version 2).
  - Captions: "Откройте 192.168.4.1" / "Ждём данные…".
  - The captive DNS normally opens the page by itself.
- **Step 3: data received.** The QR is hidden and three `ui_20` lines are shown, centred at y 130 / 166 / 202 (w ≤ 220): "Wi-Fi ✓", "Ключ API ✓", "Модель ✓". Then "Подключаюсь…". On success: H with the toast "Готово".
- **Portal:** our own `esp_http_server` page, because esp-wifi-connect cannot take keys.
  - Fields: Wi-Fi SSID (scan list + manual) and password, an OpenRouter key (never echoed back), chat model, TTS voice, an advanced section with base URL / key / model for each endpoint, and the UI language.
  - It closes after a save, after 10 min, or when Q is left (title zone, swipe right). Locking over Q keeps it running.
- **Backlight** is forced to 100 % while a QR is shown.
- **LVGL 9.5 call order:** `lv_qrcode_set_size()`, then the colours, then `set_quiet_zone`, then `lv_qrcode_update()`. In 9.5, a size change after the data clears the code (fixed in 9.6).

### 3.11 Confirm dialog (`T_DIALOG`)

| Element | Geometry | Fit |
|---|---|---|
| Icon | `ui_28` error/warning colour, centre (180,96) | OK |
| Title | `ui_20` `text-1`, centre (180,132), w ≤ 260 | OK |
| Body | `ui_16` `text-2`, x 60–300, y 150–210, ≤ 3 lines, centred | 123.7 OK |
| "Отмена" | stadium x 56–176, y 226–276, `surface-1` | 146.8 OK |
| "Удалить" / "Сбросить" | stadium x 184–304, y 226–276, `error` fill, `ink` text | 146.8 OK |

"Отмена" is the way out; PWR locks over the dialog, which is still there after the unlock.

### 3.12 Step template (`T_STEP`): onboarding and Wi-Fi connecting

| Element | Geometry | Fit |
|---|---|---|
| Mini orb | preset MINI (core r 28, halo2 r 40, with face), centre (180,88) | 132 OK |
| Title | `ui_28` (or `ui_20` for long strings), centre (180,153), w ≤ 280 | 146.8 OK |
| Subtitle | `ui_16` `text-2`, centre (180,182), w ≤ 290 | 145.5 OK |
| Primary button | stadium x 62–298, y 204–260, accent fill, `ink` `ui_20` | 131.9 OK |
| Secondary button | stadium x 80–280, y 268–324, `surface-1` | 164.5 OK |
| Progress dots (onboarding only) | 5 × Ø6 at y 338, x 156/168/180/192/204; current = accent | 162.8 OK |

**WC (Wi-Fi connecting):**
- While connecting: mini orb THINKING, title "Подключаюсь", subtitle "к «HomeNet»", secondary "Отмена".
- Success: success toast, then back after 1.2 s.
- Failure:
  - `WIFI_REASON_AUTH_FAIL` or a handshake timeout: title "Неверный пароль", primary "Ввести снова" (K), secondary "Другая сеть".
  - Network not found: "Сеть не найдена", primary "Искать снова".

### 3.13 Ambient (`T_AMBIENT`)

| Element | Geometry | Fit |
|---|---|---|
| Clock "14:32" | `ui_48` `text-1`, centre (180,166), w ≤ 160 | 90.4 OK |
| Date "пт, 17 октября" | `ui_16` `text-2`, centre (180,212), w ≤ 220 | 117.7 OK |
| Dot orb | preset AMBIENT (core r 12, halo1 r 18, mode accent, no face), centre (180,262) | 100 OK |
| Battery | `ui_16` icon at (180,300), only if < 20 % or charging | OK |

The clock redraws once a minute. The dot breathes over an 8 s cycle (redrawing a 40×40 box).

### 3.14 Error presentation (no separate screen)

**Pill.** A persistent fault replaces the pill text with the fault in its colour, e.g. "Нет Wi-Fi ›", "Ключ не принят ›", "Нет средств ›".
- The pill grows to ≤ 160 wide (x 100–260): 166.6 OK.
- Tapping the pill always opens M, also with a fault. M then starts with a fault row that opens the fixing screen. The pill is the only way to M (mode, typed question, history), and in translator mode, where both side buttons are chips, also the only way to Settings, so a persistent fault must never block it.

**Orb.** ERROR for 2.4 s, then OFFLINE (hollow) or IDLE.

**Chat card** (system message):

| Property | Value |
|---|---|
| Size / background | width 252, radius 20, status dim fill, padding 14 |
| Row 1 | icon `ui_20` in the status colour + title `ui_20` `text-1` |
| Row 2 | body `ui_16` `text-2`, ≤ 2 lines |
| Row 3 | optional action stadium 160×50, status-colour fill, `ink` text |
| Height | 14 + 26 + 6 + 40 + 10 + 50 + 14 = 160 ≤ 164 viewport (150.4 OK) |

The first error card switches an empty H to the conversation layout.

### 3.15 Touch test (`T_TOUCH`)

- Black screen with a 12 px `text-1` dot at the last sample.
- `ui_16` "x 182 · y 177 · 23 Гц" at (180,180).
- A small `lv_obj` dot is moved, so the invalidated area is about 2 × 14² px.
- A ✕ circle Ø52 at (180,300) exits (|(0,120)| + 26 = 146); its own touches are not samples. PWR locks.

### 3.16 Lock screen (LK) and battery

PWR short locks and unlocks (§2.3). Nothing is stored: the device always boots unlocked.

**One overlay, not a screen.** `scr_lock.c` puts a single 360×360 object on `lv_layer_top()` over whatever is open; the navigation stack below stays as it is, so the unlock reveals exactly the screen that was locked (a half-typed K, a dialog, Q with its portal running). The overlay catches every touch and draws in `LV_EVENT_DRAW_MAIN`: the iris, the battery ring, the padlock and its pulse ring. Clock, date, hint and battery line are child labels. While it is up, `ui_rim` moves above it and draws only its POWER channel (the 3 s power-off ring stays visible); top-layer toasts and the sleep catcher stay above it.

| Element | Geometry | Fit |
|---|---|---|
| Backdrop / iris | black annulus centred (180,180), outer r 182 (past the glass), inner r animated; below r 3 one black rectangle | – (by design, exempt from the audit) |
| Padlock at rest | k = 0.55 of the 60 × 84 base (body 60 × 46 r 12 in the mode accent gradient, shackle arc r 21 w 7 in `text-2`, keyhole `ink`), centre (180,84): box x 162–198, y 59–109; while it opens (k → 0.95, shackle raised 16k): x 151–209, y 29–124 | 122 / 154 OK |
| Padlock while locking | k 0.6 → 1 at the centre (180,180): box x 150–210, y 122–222 (raised shackle +16) | 65 OK |
| Clock | `ui_48` `text-1` "14:32", x 100–260, y 132–188 (w ≤ 160) | 93 OK |
| Date / hint | `ui_16`, x 70–290, y 198–218 (w ≤ 220): date `text-2` "сб, 18 октября"; on a refused input "Нажмите PWR" / "Press PWR" in `text-1` crossfades in for 1.5 s | 116 OK |
| Battery line | `ui_20`, x 100–260, y 249–275: "85 %", `success` bolt + "85 %" while charging, `success` "Заряжено" / "Charged" when full, `error` below 15 % | 124 OK |
| Battery ring | rim band r 160–168: track `surface-2`, level arc from 12 o'clock clockwise 360°·pct, `success` ≥ 30 %, `warning` 15–29 %, `error` < 15 %; while charging a 36° piece mixed 43 % toward white travels along the filled part (100°/s, A38) | 168 OK |
| Pulse ring | r 40 → 92, w 3, centre (180,180), mode accent pre-mixed to black | 92 OK |

**Sleep while locked.** Whatever the screen below and `dim_s`: dimmed (25 %) after 10 s, off after 20 s, never ambient (`ui_sleep.c`), counted from the lock (the lock and the unlock restart LVGL's idle clock, so a console `lock` after a long idle does not go dark at once). The lock stays. A touch or BOOT on a dark or dim lock screen lights it and shows the hint. A PWR press on it unlocks at once (one press): a press that lit a dark screen holds the lit lock screen for 400 ms, then the unlock runs, about 1 s in all.

**PWR during an animation.** A PWR press while the lock or the unlock runs plays the running timeline back from the frame it shows (A35 backwards opens the iris again, A36 backwards closes it and brings the padlock and the content back), so the padlock never jumps and the content never pops in.

**Refused input.** Touch (the overlay calls `lv_indev_wait_release`, so nothing below sees the touch or its release) and BOOT (`ui_boot_down()` → `UI_LOCK_NUDGE`): the padlock shakes (A37) and the hint replaces the date. Locking emits `UI_IN_LOCK 1`: the app ends whatever runs without a status text (recording discarded, request aborted, speech stopped). Wi-Fi, battery and other state keep updating underneath and on the overlay.

**Battery model** (`components/board/battery.c`, `board_battery_state()`): a reading every 5 s (8 ADC samples averaged), median of the last 5, a Li-ion discharge curve (3.30 V 0 %, 3.61 V 5 %, 3.71 V 15 %, 3.77 V 30 %, 3.84 V 50 %, 3.95 V 70 %, 4.02 V 80 %, 4.11 V 90 %, 4.20 V 100 %, linear in between). The ETA6098 charger has no status pin, so **charging** = USB host present (`usb_serial_jtag_is_connected()`, start-of-frame packets) or the filtered voltage 15 mV higher than 2 min ago below 4.15 V; **full** = either condition at ≥ 4.15 V, left only below 4.11 V (a cell in the charger's CV phase hovers at the threshold, which would flip "Заряжено" ⇄ "85 %").
- **Settled readings only start the trend.** A cell's voltage also rises after a load ends (a turn's speech, mic and Wi-Fi; the backlight dimming or going off), which would read as a wall charger. So `app_io.c` passes `settled` = no turn, speaker, mic, backlight or screen-level change for 60 s; only settled readings enter the 2 min history and may start the trend. It ends on a settled 10 mV fall over 2 min or a settled reading 25 mV under the peak since, and on any reading 60 mV under the peak (unplugged while in use; a turn's sag on the charger stays well under that).
- **No cell.** Settled readings more than 250 mV apart within the median window (a charger output swinging with no cell behind it) report no reading (pct −1, indicator hidden) until they agree within 80 mV again. A charger that holds a steady 4.2 V with no cell reads as full; not observed on this board yet.
- A plain wall charger is seen only by the rising voltage, and not at all when the device boots on it with the cell already in the flat part of the curve; at full on a wall charger the model may show "discharging, 97 %". The percent follows the charge direction and moves against it only by ≥ 5 points at once (load sag). The app posts `UI_SET_BATTERY` (`UI_BAT_MAKE(pct, flags)`) when the state changes and toasts "Батарея 15 %" and "Батарея 5 %" once each per discharge (re-armed by charging or above 20 %).

---

## 4. The round keyboard

### 4.1 Decision: two-step "group → letter" keypad on the Russian phone-keypad groups

**Capacity.** With ≥ 50 px keys, the four rows that fit under a text field hold 14–19 keys in total, and 6–7 of them must be function keys. The Russian alphabet has 33 letters.

| Option | Problem on this screen |
|---|---|
| ЙЦУКЕН / QWERTY rows | 11–12 keys per row need ≥ 572 px. Split into pages, Russian needs 3 pages of ~12 letters, and the user hunts across pages. |
| Long-press variants | Hidden, conflicts with ⌫ repeat, and popovers clip at the rim (LVGL docs: popovers extend past the widget). |
| Multi-tap T9 | Timing-dependent ("аб" needs a pause); bad for passwords. |
| Rim / ring picker | COMPASS evidence comes from a physical rotating bezel. On this board, ring sectors shrink to 40 px at their inner edge. |
| Flick petals (Expressive) | Needs learned slides, the "down" petal sits under the finger, and petals near the rim must be displaced. |
| **Two-step groups (chosen)** | Every letter is exactly 2 taps, with no timing and no hidden state. All 33 letters are visible on level 1 in alphabetical order (АБВГ · ДЕЁЖЗ · …, the Russian phone keypad 2–9). Level 2 shows the group's **lower and upper case at once**, so there is no Shift and no caps state, the usual cause of wrong Wi-Fi passwords. Level 2 also shows that key's **digit** (2–9), so digits never need a page switch. |

- **Cost:** about 1.2 s per character; "HomeNet2024" takes about 15 s.
- **Role:** the keyboard is the fallback. The primary paths are the phone portal (Wi-Fi, keys, URLs, model ids) and voice (questions).
- **OpenRouter keys** get a dedicated single-tap **hex keypad**. The prefix `sk-or-v1-` is pre-filled and protected, and an "Aa" button switches to the full keyboard if the key has another format.

### 4.2 Geometry (verified against LVGL 9.5 `update_map`)

| Property | Value |
|---|---|
| Object | `lv_keyboard`, `lv_obj_align(kb, LV_ALIGN_TOP_LEFT, 18, 115)`, size 324×209 |
| Padding | `pad_all 0`, **`pad_row 3`**, **`pad_column 4`**, no background, no border |
| Rows | row height = (209 − 3·3)/4 = **50 px**. **R1 y 115–165, R2 y 168–218, R3 y 221–271, R4 y 274–324** |
| Key style | radius 10. Letter and group keys: `surface-2` / `text-1`. Function keys: `surface-1` / `text-2`. OK: accent fill with `ink` text; OK disabled: `surface-1` / `text-3`. Pressed: `surface-3`. Popovers off |
| Hit expansion | `get_button_from_point` grows each key by `(pad/2) + 1 + (pad & 1)` = **+3 px left/right and +3 px up/down**, so a visible 50×50 key is 56×56 effective. Outer edges do not grow, because `pad_all` is 0 |
| Width formula | `x1 = (max_w_no_gap · units_before)/units_row + i·pad_col`, with `max_w_no_gap = 324 − (n−1)·4`. Units are 1–15 (low 4 bits) |
| Tapering | rows taper to the circle with **hidden spacer buttons** (`LV_BUTTONMATRIX_CTRL_HIDDEN`, label `" "`; an empty string `""` would end the map). A press on a spacer selects nothing |

### 4.3 Layouts

All x ranges below come from the 9.5 formula: left edge to right edge, exclusive, then the width.

**Level 1, letters.** The geometry is the same for RU and EN.

```
R1           [ абвг ] [ деёжз ] [ ийкл ]              EN: [ abc ] [ def ] [ ghi ]
R2  [ 123 ]  [ мноп ] [ рсту  ] [ фхцч ]  [ ⌫ ]       EN: [123] [ jkl ] [ mno ] [ pqrs ] [⌫]
R3           [ шщъы ] [ ьэюя  ] [пробел]              EN: [ tuv ] [ wxyz ] [ space ]
R4                 [ EN ] [ .,?! ] [ ✓ ]              EN: [ RU ] [ .,?! ] [ ✓ ]
```

| Row | Units (h = hidden) | Visible keys: x range (width) |
|---|---|---|
| R1 | h10, 13, 13, 13, h10 | 74–142 (68) · 146–213 (67) · 217–285 (68) |
| R2 | 10, 13, 13, 13, 10 | 123: 18–70 (52) · 74–142 (68) · 146–213 (67) · 217–285 (68) · ⌫: 289–342 (53) |
| R3 | h10, 13, 13, 13, h10 | 74–142 · 146–213 · 217–285 |
| R4 | h13, 9, 9, 9, h13 | language: 97–149 (52) · .,?!: 153–206 (53) · ✓: 210–262 (52) |

- Group labels use `ui_20`: "деёжз" ≈ 59 px and "шщъы" ≈ 57 px, both inside 67–68 px keys.
- "пробел"/"space" uses `ui_16`.
- The language key shows the language it **switches to**. It is DISABLED in ASCII-only fields, which also start in EN.

**Level 2: the chosen group.** Lower case in R1, upper case in R2, then back and digit in R3. The font is `ui_28`. A letter or digit is inserted and the keyboard returns to level 1 at once.

```
R1   [ а ] [ б ] [ в ] [ г ]       lower case
R2   [ А ] [ Б ] [ В ] [ Г ]       upper case
R3        [ ‹ ] [ 2 ]              back to level 1 · the group's digit
R4   (one hidden key)
```

| Shape | R1 and R2 units | Keys: x (width) | R3 units → keys |
|---|---|---|---|
| 3 letters (EN abc, def, ghi, jkl, mno, tuv) | h6, 14, 14, 14, h6 | 56–136 (80) · 140–219 (79) · 223–303 (80) | h15, 12, 12, h15 → ‹ 108–178 (70) · digit 182–251 (69) |
| 4 letters (RU groups except деёжз; EN pqrs, wxyz) | h2, 12, 12, 12, 12, h2 | 33–103 (70) · 107–178 (71) · 182–252 (70) · 256–326 (70) | same |
| 5 (деёжз, punctuation) | h2, 11, 11, 11, 11, 11, h2 | 32–88 (56) · 92–148 (56) · 152–207 (55) · 211–267 (56) · 271–327 (56) | same |
| Symbol group | R1 as 4-letter; R2 one hidden key (h1) | as 4-letter | h15, 12, h12, h15 → ‹ 108–178 (70) |

Group contents (32 Cyrillic letters + Ё = 33; EN 26):

| Key | R1 (lower) | R2 (upper) | R3 digit |
|---|---|---|---|
| абвг | а б в г | А Б В Г | 2 |
| деёжз | д е ё ж з | Д Е Ё Ж З | 3 |
| ийкл | и й к л | И Й К Л | 4 |
| мноп | м н о п | М Н О П | 5 |
| рсту | р с т у | Р С Т У | 6 |
| фхцч | ф х ц ч | Ф Х Ц Ч | 7 |
| шщъы | ш щ ъ ы | Ш Щ Ъ Ы | 8 |
| ьэюя | ь э ю я | Ь Э Ю Я | 9 |
| abc / def / ghi / jkl / mno / tuv | 3 letters | 3 capitals | 2 / 3 / 4 / 5 / 6 / 8 |
| pqrs / wxyz | 4 letters | 4 capitals | 7 / 9 |
| .,?! (punctuation) | `. , ? ! -` | `: ; ( ) "` | 1 |

**Digits page.** Single taps, reached with "123".

```
R1           [ 1 ] [ 2 ] [ 3 ]
R2  [ абв ]  [ 4 ] [ 5 ] [ 6 ]  [ ⌫ ]       ("abc" when the letters are EN)
R3           [ 7 ] [ 8 ] [ 9 ]
R4               [ #+= ] [ 0 ] [ ✓ ]
```

Same units and x ranges as level 1. Digits use `ui_28`.

**Symbols level 1.** Two-step like letters. It covers all 32 printable ASCII symbols plus « » — …, and is reached with "#+=".

```
R1           [ !?., ] [ @#$% ] [ &*^~ ]
R2  [ абв ]  [ -_=+ ] [ ()[] ] [ {}<> ]  [ ⌫ ]
R3           [ /\|` ] [ :;'" ] [пробел]
R4               [ 123 ] [ «»—… ] [ ✓ ]
```

Each symbol group's level 2 uses the "symbol group" shape. After a symbol, the keyboard returns to symbols level 1.

**Hex keypad.** For OpenRouter keys: the field holds `sk-or-v1-` + 64 hex characters.

```
R1  [1][2][3][4][5]
R2  [6][7][8][9][0][⌫]
R3    [a][b][c][d]
R4    [e][f][✓]
```

| Row | Units | Keys: x (width) |
|---|---|---|
| R1 | h1, 12×5, h1 | 26–84 (58) · 88–146 (58) · 150–209 (59) · 213–271 (58) · 275–333 (58) |
| R2 | 1×6 | 18–68 (50) · 72–123 (51) · 127–178 (51) · 182–232 (50) · 236–287 (51) · 291–342 (51) |
| R3 | h3, 8×4, h3 | 46–110 (64) · 114–178 (64) · 182–246 (64) · 250–314 (64) |
| R4 | h13, 9, 9, 9, h13 | e: 97–149 · f: 153–206 · ✓: 210–262 |

- Font `ui_28`. Accepted characters `0-9a-f`.
- OK stays DISABLED until exactly 64 hex characters follow the prefix.
- ⌫ never deletes into the 9-character prefix.

### 4.4 Proof that every row fits

The worst corner is measured from C; the limit is 168.

| Layout / row | y | Far-edge d | Allowed half-chord | Half-span | Worst corner | Result |
|---|---|---|---|---|---|---|
| L1 / digits / symbols R1 | 115–165 | 65 | 154.9 | 106 | 124.3 | OK |
| L1 R2 | 168–218 | 38 | 163.6 | 162 | 166.4 | OK |
| L1 R3 | 221–271 | 91 | 141.2 | 106 | 139.7 | OK |
| L1 R4 | 274–324 | 144 | 86.5 | 83 | 166.2 | OK |
| L2 3-key R1 / R2 | – | 65 / 38 | 154.9 / 163.6 | 124 | 140.0 / 129.7 | OK |
| L2 4-key R1 / R2 | – | 65 / 38 | – | 147 | 160.7 / 151.8 | OK |
| L2 5-key R1 / R2 | – | 65 / 38 | – | 148 | 161.6 / 152.8 | OK |
| L2 R3 (‹ + digit) | 221–271 | 91 | 141.2 | 72 | 116.0 | OK |
| Hex R1 (tightest) | 115–165 | 65 | 154.9 | 154 | 167.2 | OK |
| Hex R2 | 168–218 | 38 | 163.6 | 162 | 166.4 | OK |
| Hex R3 | 221–271 | 91 | 141.2 | 134 | 162.0 | OK |

Target sizes:
- Smallest visible key: 50×50 (hex R2), 56×56 effective.
- Group keys: 67–68×50, 73–74×56 effective.
- Level-2 keys: 55–80×50.

### 4.5 Mapping onto `lv_keyboard` / `lv_buttonmatrix`

Facts verified in 9.5 that this code relies on:
- `lv_keyboard_set_map(kb, mode, map, ctrl)` writes into **file-static tables shared by all keyboards** and re-applies the current mode immediately. So only one keyboard may exist at a time.
- `lv_keyboard_set_mode()` returns early if the mode is unchanged. So we use a single slot, `LV_KEYBOARD_MODE_USER_1`, and swap maps into it.
- With popovers off, the ctrl map is copied, POPOVER is stripped, and the result is `memcpy`'d into the widget. **Runtime DISABLED/CHECKED flags are lost on every swap** and must be re-applied.
- The constructor adds `lv_keyboard_def_event_cb` on VALUE_CHANGED and aligns the widget BOTTOM_MID.
- VALUE_CHANGED fires on PRESSED for keys without `CLICK_TRIG`, and on RELEASED for `CLICK_TRIG` keys. The parameter is a `uint32_t *` button index. Hidden and disabled keys never fire.
- Swapping the map inside a RELEASED-triggered VALUE_CHANGED is safe: `invalidate_button_area` bounds-checks the old index.

```c
/* ui_kb.c — LVGL 9.5. Keys are classified by button index through action tables, never by UTF-8 label. */
#define SP   " "
#define HID  LV_BUTTONMATRIX_CTRL_HIDDEN
#define TAP  (LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT)
#define U(n) ((lv_buttonmatrix_ctrl_t)(n))            /* 1..15 */

typedef enum { KA_NONE, KA_CHAR, KA_GROUP, KA_BKSP, KA_SPACE, KA_OK,
               KA_LANG, KA_DIGITS, KA_SYMBOLS, KA_LETTERS, KA_BACK } kb_act_t;
typedef enum { KL_L1, KL_L2, KL_PAGE } kb_kind_t;

typedef struct kb_layout {
    const char * const *map;  const lv_buttonmatrix_ctrl_t *ctrl;
    const uint8_t *act;                              /* kb_act_t per button index ("\n" not counted) */
    const struct kb_layout * const *sub;             /* level-2 layout per button index, or NULL */
    const struct kb_layout *back;                    /* where ‹ and a typed char return; NULL = letters */
    const lv_font_t *font;  kb_kind_t kind;
    int8_t ok_id, lang_id;                           /* -1 if the layout has none */
} kb_layout_t;

static const char * const map_ru_l1[] = {
    SP, "абвг", "деёжз", "ийкл", SP, "\n",
    "123", "мноп", "рсту", "фхцч", LV_SYMBOL_BACKSPACE, "\n",
    SP, "шщъы", "ьэюя", "пробел", SP, "\n",
    SP, "EN", ".,?!", LV_SYMBOL_OK, SP, "" };          /* map_en_l1: abc def ghi / 123 jkl mno pqrs ⌫ /
                                                         tuv wxyz space / RU .,?! ✓ */
static const lv_buttonmatrix_ctrl_t ctrl_l1[20] = {
    HID|U(10), TAP|U(13), TAP|U(13), TAP|U(13), HID|U(10),
    TAP|U(10), TAP|U(13), TAP|U(13), TAP|U(13),     U(10),   /* ⌫: fires on PRESSED, auto-repeats */
    HID|U(10), TAP|U(13), TAP|U(13), TAP|U(13), HID|U(10),
    HID|U(13), TAP|U(9),  TAP|U(9),  TAP|U(9),  HID|U(13) };
static const uint8_t act_l1[20] = {
    KA_NONE, KA_GROUP, KA_GROUP, KA_GROUP, KA_NONE,
    KA_DIGITS, KA_GROUP, KA_GROUP, KA_GROUP, KA_BKSP,
    KA_NONE, KA_GROUP, KA_GROUP, KA_SPACE, KA_NONE,
    KA_NONE, KA_LANG, KA_GROUP, KA_OK, KA_NONE };      /* digits page / symbols L1: same shape */

static const char * const map_l2_abvg[] = {
    SP, "а", "б", "в", "г", SP, "\n",
    SP, "А", "Б", "В", "Г", SP, "\n",
    SP, LV_SYMBOL_LEFT, "2", SP, "\n",
    SP, "" };
static const lv_buttonmatrix_ctrl_t ctrl_l2_4[17] = {
    HID|U(2), TAP|U(12), TAP|U(12), TAP|U(12), TAP|U(12), HID|U(2),
    HID|U(2), TAP|U(12), TAP|U(12), TAP|U(12), TAP|U(12), HID|U(2),
    HID|U(15), TAP|U(12), TAP|U(12), HID|U(15),
    HID|U(1) };
static const uint8_t act_l2_4[17] = {
    KA_NONE, KA_CHAR, KA_CHAR, KA_CHAR, KA_CHAR, KA_NONE,
    KA_NONE, KA_CHAR, KA_CHAR, KA_CHAR, KA_CHAR, KA_NONE,
    KA_NONE, KA_BACK, KA_CHAR, KA_NONE,  KA_NONE };
/* ctrl_l2_3 (15 btns): h6,14,14,14,h6 ×2 rows; ctrl_l2_5 (19): h2,11×5,h2 ×2;
   ctrl_l2_sym (12): h2,12×4,h2 / h1 / h15,12,h12,h15 / h1; all R3 keys TAP.
   26 level-2 maps in flash (8 RU + 8 EN + punctuation + 9 symbol groups), about 100 B each. */

static const char * const map_hex[] = {
    SP, "1", "2", "3", "4", "5", SP, "\n",
    "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    SP, "a", "b", "c", "d", SP, "\n",
    SP, "e", "f", LV_SYMBOL_OK, SP, "" };
static const lv_buttonmatrix_ctrl_t ctrl_hex[24] = {
    HID|U(1), TAP|U(12), TAP|U(12), TAP|U(12), TAP|U(12), TAP|U(12), HID|U(1),
    TAP|U(1), TAP|U(1), TAP|U(1), TAP|U(1), TAP|U(1), U(1),
    HID|U(3), TAP|U(8), TAP|U(8), TAP|U(8), TAP|U(8), HID|U(3),
    HID|U(13), TAP|U(9), TAP|U(9), TAP|U(9), HID|U(13) };

static struct { lv_obj_t *kb, *ta; const kb_layout_t *cur, *letters; ui_field_t field; } s;

static void kb_refresh_dynamic(void)            /* ctrl map was memcpy'd: re-apply runtime flags */
{
    const kb_layout_t *L = s.cur;
    if (L->ok_id >= 0) {
        if (field_ok_allowed(s.field, lv_textarea_get_text(s.ta)))
             lv_buttonmatrix_clear_button_ctrl(s.kb, L->ok_id, LV_BUTTONMATRIX_CTRL_DISABLED);
        else lv_buttonmatrix_set_button_ctrl(s.kb, L->ok_id, LV_BUTTONMATRIX_CTRL_DISABLED);
    }
    if (L->lang_id >= 0 && field_is_ascii_only(s.field))
        lv_buttonmatrix_set_button_ctrl(s.kb, L->lang_id, LV_BUTTONMATRIX_CTRL_DISABLED);
}
static void kb_apply(const kb_layout_t *L)
{
    s.cur = L;
    lv_obj_set_style_text_font(s.kb, L->font, LV_PART_ITEMS);
    lv_keyboard_set_map(s.kb, LV_KEYBOARD_MODE_USER_1, L->map, L->ctrl);   /* applies at once */
    kb_refresh_dynamic();
}
static void kb_value_cb(lv_event_t *e)
{
    uint32_t id = *(const uint32_t *)lv_event_get_param(e);
    const kb_layout_t *L = s.cur;
    switch ((kb_act_t)L->act[id]) {
    case KA_CHAR:    field_insert(s.ta, s.field, lv_buttonmatrix_get_button_text(s.kb, id)); /* filters, byte limits */
                     if (L->kind == KL_L2) kb_apply(L->back ? L->back : s.letters);
                     else kb_refresh_dynamic();
                     break;
    case KA_GROUP:   kb_apply(L->sub[id]); break;
    case KA_BKSP:    field_backspace(s.ta, s.field); kb_refresh_dynamic(); break;  /* protects the hex prefix */
    case KA_SPACE:   field_insert(s.ta, s.field, " "); kb_refresh_dynamic(); break;
    case KA_OK:      lv_obj_send_event(s.ta, LV_EVENT_READY, NULL); break;
    case KA_LANG:    s.letters = (s.letters == &KB_RU_L1) ? &KB_EN_L1 : &KB_RU_L1; kb_apply(s.letters); break;
    case KA_DIGITS:  kb_apply(s.letters == &KB_RU_L1 ? &KB_DIG_RU : &KB_DIG_EN); break;
    case KA_SYMBOLS: kb_apply(s.letters == &KB_RU_L1 ? &KB_SYM_RU : &KB_SYM_EN); break;
    case KA_LETTERS:
    case KA_BACK:    kb_apply(L->back ? L->back : s.letters); break;
    default: break;
    }
}
static void kb_draw_cb(lv_event_t *e)           /* per-key colours and fonts without extra objects */
{
    lv_draw_task_t *t = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *b = lv_draw_task_get_draw_dsc(t);
    if (b->part != LV_PART_ITEMS) return;
    kb_act_t a = (kb_act_t)s.cur->act[b->id1];
    if (a == KA_CHAR || a == KA_GROUP || a == KA_NONE) return;          /* shared key styles */
    bool dis = lv_buttonmatrix_has_button_ctrl(s.kb, b->id1, LV_BUTTONMATRIX_CTRL_DISABLED);
    bool pr  = lv_obj_has_state(s.kb, LV_STATE_PRESSED) &&
               lv_buttonmatrix_get_selected_button(s.kb) == b->id1;
    lv_draw_fill_dsc_t  *f = lv_draw_task_get_fill_dsc(t);
    lv_draw_label_dsc_t *l = lv_draw_task_get_label_dsc(t);
    if (a == KA_OK && !dis) { if (f) f->color = pr ? ui_accent_mid() : ui_accent(); if (l) l->color = UI_INK; }
    else if (!pr)           { if (f) f->color = UI_SURFACE_1; if (l && !dis) l->color = UI_TEXT_2; }
    if (a == KA_SPACE && l) l->font = &ui_16;
}
lv_obj_t *ui_kb_create(lv_obj_t *scr, lv_obj_t *ta, ui_field_t field)
{
    s.kb = lv_keyboard_create(scr); s.ta = ta; s.field = field;
    lv_obj_remove_event_cb(s.kb, lv_keyboard_def_event_cb);
    lv_keyboard_set_popovers(s.kb, false);
    lv_keyboard_set_textarea(s.kb, ta);                      /* adds FOCUSED to ta, so the cursor shows */
    lv_keyboard_set_mode(s.kb, LV_KEYBOARD_MODE_USER_1);
    lv_obj_align(s.kb, LV_ALIGN_TOP_LEFT, 18, 115);          /* NOT set_pos: constructor aligned BOTTOM_MID */
    lv_obj_set_size(s.kb, 324, 209);
    lv_obj_add_style(s.kb, &st_kb_main, 0);                  /* pad_all 0, pad_row 3, pad_column 4, bg_opa 0 */
    lv_obj_add_style(s.kb, &st_key, LV_PART_ITEMS);
    lv_obj_add_style(s.kb, &st_key_pr, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_add_style(s.kb, &st_key_dis, LV_PART_ITEMS | LV_STATE_DISABLED);
    lv_obj_remove_flag(s.kb, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(s.kb, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(s.kb, kb_value_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s.kb, kb_draw_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    s.letters = field_starts_ru(field) ? &KB_RU_L1 : &KB_EN_L1;
    kb_apply(field == UI_FIELD_KEY_HEX ? &KB_HEX : s.letters);
    return s.kb;
}
```

Cost and memory:
- A map swap redraws only the keyboard: 67.7k px, one frame of about 14 ms, with no animation.
- Button areas cost about 10 B per button (area 8 B + ctrl 2 B).

### 4.6 Field behaviours

| Field | Start layout | Accepted / limits | Secret | OK enabled when |
|---|---|---|---|---|
| Wi-Fi password | EN (language key disabled) | printable ASCII 0x20–0x7E, 8–63 characters | yes (reveal) | len ≥ 8, or the network is open; after "Другая сеть…" (hidden SSID) also empty, which joins it as an open network |
| SSID ("Другая сеть") | EN | ≤ 32 **bytes** (checked in UTF-8 bytes) | no | len ≥ 1 |
| OpenRouter key | HEX ("Aa" → EN) | `0-9a-f`, 64 after the protected `sk-or-v1-` | no (shown so it can be checked; validated by `GET /api/v1/key` on OK) | exactly 64 hex |
| Other key / base URL / model id | EN | printable ASCII, ≤ 200 | key: yes | len ≥ 1; URL must start with `http` |
| Question | UI language | any, ≤ 500 | no | len ≥ 1 |

- **✕** sends `LV_EVENT_CANCEL` to the textarea and returns to the caller (PWR locks; the unlock comes back to K). The caller keeps the draft only for questions.
- BOOT is ignored on this screen.

---

## 5. Interaction design

### 5.1 Push-to-talk: hold and tap both work, with no setting

- While the screen is awake, the mic runs into a **0.5 s PSRAM ring buffer** (16 KB). It never leaves RAM unless the user talks.
- On press, the last 250 ms are prepended (500 ms for a chip hold), so the first syllable is never lost.
- The FSM runs in the app task. The UI only mirrors it through `s_ui_state`.

| From | Event | To | Effects |
|---|---|---|---|
| IDLE | press (BOOT or orb) | REC_PRESS | capture with pre-roll; earcon up; orb LISTENING; status "Слушаю…"; `ui_rim` RECORD |
| IDLE (H′) | chip pressed ≥ 400 ms | REC_PRESS (forced source = chip) | the chip glows; status "Слушаю… (RU)" |
| REC_PRESS | release after ≥ 400 ms | SEND | earcon down |
| REC_PRESS | release after < 400 ms | REC_LATCHED | ■ mark; status "Нажмите — отправлю". A BOOT tap latches 300 ms after its release (the recording runs on meanwhile) |
| REC_PRESS (BOOT tap) | second BOOT press ≤ 300 ms after the release | IDLE | double tap: the recording is discarded silently; the mode switches (A21) with a toast |
| REC_PRESS (orb) | finger beyond the cancel radius (hero 110, compact 70 px from the orb centre) | REC_CANCEL | orb grey with ✕; status "Отпустите — отмена" |
| REC_CANCEL | finger back inside | REC_PRESS | – |
| REC_CANCEL | release | IDLE | discard; earcon "cancel" |
| REC_LATCHED | press (BOOT or orb) | SEND | sends on press-down |
| REC_LATCHED | VAD: ≥ 400 ms of speech, then 1200 ms of silence | SEND | automatic |
| REC_LATCHED | no speech for 5 s | IDLE | neutral status "Не расслышал" for 2 s |
| REC_* | 25 s elapsed | – | status countdown "Ещё 5 с" … "1 с"; rim ring turns `warning` |
| REC_* | 30 s elapsed | SEND | automatic |
| REC_* | PWR (lock) | IDLE | discard, no status text; the lock screen closes over it |
| SEND | VAD speech < 300 ms | IDLE | "Слишком коротко" for 1.5 s; **no request** |
| SEND | otherwise | UPLOADING | the waveform bubble appears (A16); encode, POST |
| UPLOADING | body fully written | THINKING | – |
| THINKING | `transcript` (or `src_lang`) parsed from the stream | THINKING | waveform → text (A17); translator bubble slides to its side (A18) |
| THINKING | reply/translation text | SPEAKING if voice is on and a speaker is present; otherwise ANSWERED → IDLE | reply appears (§5.4) |
| SPEAKING | playback finished | IDLE | – |
| SPEAKING | press (BOOT/orb/chip) | IDLE, or REC_PRESS if still held at 400 ms | TTS flushed within 50 ms; recording starts after the flush (no pre-roll, so no TTS tail is captured) |
| UPLOADING / THINKING | tap | – | orb pulse; hint "Держите — заново" (first uses) |
| UPLOADING / THINKING | hold ≥ 400 ms | REC_PRESS | old request aborted |
| UPLOADING / THINKING / SPEAKING | PWR (lock) | IDLE | abort, speech stopped, no status text |
| IDLE (not ready) | BOOT tap | – | "not ready" card after the 300 ms window, unless a second press makes it a double tap |
| any | network/API error | ERROR for 2.4 s | then IDLE or OFFLINE (§5.6) |

### 5.2 Cancel, interrupt, text-only, no speaker

- **Cancel recording:** slide off the orb and release; PWR locks and discards it too.
- **Interrupt speech:** tap the orb, BOOT, the voice button or a chip (PWR locks and stops it). The voice button also switches to text-only.
- **Barge-in:** hold BOOT or the orb while the device speaks.
- **Text-only toggle:**
  - Controls: the left side button on H (F028 ↔ F6A9), M → "Ответ голосом", or the same row in S.
  - The NVS write is debounced 2 s and done in IDLE.
  - Toast "Голос выкл." / "Голос вкл.". The face closes its eyes for 300 ms ("shh") or widens them.
  - In text-only mode the orb goes THINKING → ANSWER-READY (A14) → IDLE, and TTS is never requested.
- **No speaker** (onboarding O4, S → Динамик or D → Динамик; the PCM5101 cannot sense one):
  - The choice is saved in the `speaker` setting and applied at boot (the UI hands a stored 0 to the app when the app registers).
  - "Нет динамика" / Нет sets text-only; "Есть динамик" / Подключён turns voice replies on (`text_only` = 0), because `text_only` defaults to 1.
  - Replies are always text and the voice button shows F6A9 with a 6 px `warning` dot.
  - Tapping it posts the card "Динамик не подключён. Отвечаю текстом" with the action "Проверить звук" → D (speaker test and the Динамик switch).
  - Translator replay is disabled. The RD view is the way to show translations.

### 5.3 What the orb shows (with face)

**Notation:**
- Hero values, with compact values in [brackets].
- L = mic level 0..1 and T = TTS level 0..1, smoothed with attack 0.6 and release 0.12 per 33 ms tick.
- Mic L = clamp((dBFS + 50)/40, 0, 1). Bar level Lᵢ = L·(0.6 + 0.4·sin(φ + i·2π/7)), with φ += 0.09 rad per tick.

**Face geometry** (relative to the core centre (cx, cy), scale k = core_r/70):

| Element | Geometry |
|---|---|
| Eyes | capsules (radius = w/2), centres (cx ± 24k + gx, cy − 8k + gy), gaze gx ∈ ±8k, gy ∈ ±6k |
| Mouth | capsule 20k × (4 + 12T)k at (cx, cy + 28k), SPEAKING only |
| Smile | arc r 12k, angles 30° → 150°, width 4k at (cx, cy + 18k) |
| Extent | all parts stay inside r 54 of the core centre (core r 70) |
| Eye colour | `ink` on 100 % accent cores. `text-1` on mid cores (UPLOADING, THINKING). The colour swaps inside a blink |
| Face hidden | when a glyph is shown (✕, ⚠, wifi), in AMBIENT, and when "Лицо шара" is off |

Expressions:

| Expression | Eyes | Use |
|---|---|---|
| NEUTRAL | 16k × 34k | idle, speaking base |
| WIDE | 18k × (38 + 4L)k | listening, "surprised" mood |
| LOOK | 16k × 30k, gaze (+6k, −6k), saccade to (−6k, −6k) every 900 ms | thinking |
| HAPPY | arcs ∩: r 11k, 200° → 340°, width 6k, rounded, + smile | answer-ready, "happy" mood, success |
| SOFT | 16k × 18k, gy + 6k | "sad"/"sorry" mood, soft errors (not heard) |
| SLEEPY | 16k × 12k, gy + 4k | dimmed |
| BLINK | 16k × 4k (transient) | blink; also the shape change between any two expressions (A4) |

Orb states:

| State | Core | Halos | Strokes | Face / glyph | Status slot |
|---|---|---|---|---|---|
| IDLE · Assistant | r 70 [30], `#7C9CFF` → `#617AC7` | h1 `#252F4D` r 80↔84 [36]; h2 `#111624` r 92↔98 [42↔44]; breathing | – | NEUTRAL; blink every 2.5–6 s (20 % double); glance every 6–12 s | pill "Ассистент ⌄" |
| IDLE · Translator | `#FFA94D` → `#C7843C` | `#4D3317` / `#24180B` | – | NEUTRAL; glances alternate toward chip A (gx −8, gy +5) and chip B (gx +8) | pill "RU ⇄ EN ⌄" |
| LISTENING | `#34E0C3` → `#29AF98`, r 70 + 4L [30 + 2L] | h1 `#10433B` r 80; h2 hidden | 24 bars [16], width 6 [4], round caps, `#34E0C3`; r_in 78 [34]; length 4 + 16·Lᵢ [3 + 7·Lᵢ], so r_out ≤ 98 [44] | WIDE. Latched: ■ (F04D, `ui_20`, `ink`) at (cx, cy + 30); compact: ■ replaces the eyes | "Слушаю…" / "Нажмите — отправлю" / "Ещё 5 с"; rim RECORD |
| CANCEL-ARMED | `#6B7380` flat | hidden | bars `#6B7380` | ✕ `ui_28` `ink` | "Отпустите — отмена" |
| UPLOADING | mode mid (`#44568C` / `#8C5D2A`) | h1 = mode dim | progress arc r 82 [36], width 4 [3], mode accent, from 270° clockwise to `s_upload_pct`; track `surface-2` | NEUTRAL, gaze (0, −6k), eyes `text-1` | "Отправляю 62 %" |
| THINKING | mode mid, r 66↔70 over 1.2 s | h1 mode dim | arc A r 82 [36] w 4, 70°, +300°/s; arc B r 90 [41] w 3 [2], 40°, −200°/s; opa 255 / 140 | LOOK | "Думаю…" → 6 s "Ещё немного…" → 20 s "Долго… держите шар" (a hold aborts and records anew; PWR locks). While the assistant searches the web (`UI_WEB` 1, protocol §2.8): "Ищу в интернете…" (149 px in `ui_16`), then "Долго… держите шар" 20 s after the search began; `UI_WEB` 0 (or leaving THINKING) restarts the normal stages |
| SPEAKING | `#B794FF` → `#8F73C7`, r 70 + 4T [30 + 2T] | h1 `#372C4D` | echo rings r 76 + 12T (w 3, opa 160) and r 84 + 14T (w 2, opa 90) [32 + 6T / 37 + 7T] | mood expression for 1.5 s, then NEUTRAL + mouth; translator: gaze toward the target chip (gy + 6k), target chip pulses (A19) | "Нажмите — стоп" (first uses) |
| ANSWER-READY (text-only) | mode accent | h1 flashes to mode mid 300 ms, back in 600 ms | – | HAPPY for 600 ms, gaze down (gy + 6k) | – |
| ERROR | `#FF5C5C` → `#C74848`, shake | h1 `#4D1C1C` | – | ⚠ `ui_28` `ink` | short error in coral |
| OFFLINE | hollow: 3 px ring `#6B7380`, fill `#000` | none | – | wifi glyph `text-3` `ui_28` | pill "Нет Wi-Fi ›" in coral |
| DIMMED | as IDLE | as IDLE | – | SLEEPY | – |
| AMBIENT | r 12, mode accent | h1 r 18, mode halo1, 8 s breathing | – | none | (clock face) |

Interaction and mood:
- **Gaze follows touches.** Pressing a side button, chip, message or the pill on H sets the gaze toward the touch point (normalised (p − c)·8k) for 900 ms.
- **New messages.** When a bubble appears, gaze_y = +6k for 1 s.
- **LLM mood:** happy → HAPPY, sad → SOFT, surprised → WIDE, neutral and thinking → NEUTRAL.

**Presets:**

| Preset | Box | Centre | Core r | Hit radius |
|---|---|---|---|---|
| HERO | 204 | (180,192) | 70 | 80 |
| COMPACT | 92 | (180,290) | 30 | 40 |
| MINI | 96 | (180,88) | 28 | 38 |
| AMBIENT | 40 | – | 12 | – |

- Hit test: a custom `LV_EVENT_HIT_TEST` circle of core_r + 10.
- OFFLINE orb tap opens W. ERROR orb tap dismisses the error.

### 5.4 Chat behaviour

1. **SEND:** the user bubble (220×44 waveform) appears at once (A16), in the right-aligned wrapper in assistant mode or the centred wrapper in translator mode.
2. **Transcript parsed** from the streamed JSON: the waveform crossfades into the text (A17), and the bubble relayouts to its text height.
3. **Reply, voice on:** sentences appear **as their TTS chunk starts playing**, because TTS is pipelined per sentence. If TTS fails, all text appears at once, with a `ui_16` `text-3` note "голос недоступен".
4. **Reply, voice off:** text appears as it streams. The label updates at most every 100 ms (`lv_label_ins_text(label, LV_LABEL_POS_LAST, chunk)`), because a relayout scales with the whole text.
5. **Auto-scroll:** `lv_obj_scroll_to_y(chat, LV_COORD_MAX, LV_ANIM_ON)` after each update that grows the content. A manual scroll suspends auto-scroll until the next turn, and the next talk press jumps back to the bottom.
6. **Separate histories per mode.** Switching mode rebuilds the chat children (≤ 20, < 10 ms) and picks the empty or conversation layout.
7. **Typed question** (M → "Написать текстом" → K): the FSM goes straight to THINKING with a text request, and the bubble shows the typed text with no waveform. In translator mode, typed text is auto-detected between A and B.
8. **Tapping a message** opens HIST (assistant) or RD (translator), never during or within 300 ms of a scroll.
9. **Web replies** (protocol §2.8): `UI_BOT_SOURCE` right after `UI_BOT_MSG` gives the host of the first source. The reply's `ui_16` `text-3` note line then reads "Источник: gismeteo.ru" (with "голос недоступен · " in front when TTS failed too) and wraps on a long host. It is shown on H and in HIST and never spoken. When the search failed (W1), the AI client's reply already ends with "Не получилось найти это в интернете, попробуйте ещё раз." after the holding phrase; the app shows and speaks it as it is, with no separate note and no source.

### 5.5 Translator turn-taking: one auto-detect control, plus a forced override on the chips

- **Default.** The orb and BOOT are one talk control for both people.
  - The request carries both languages, and the JSON schema constrains `src_lang` to `enum [A, B]`.
  - The model translates into the other language. TTS uses the voice mapped to the target locale (for MAI the voice locale selects the language).
- **Override (from Rim's "hold a pole").** Hold chip A or chip B for ≥ 400 ms to record with that source forced. This covers names, one-word replies and close language pairs (ru/uk, es/pt) where detection fails.
  - Tapping a chip opens LP for that slot instead. During SPEAKING a chip tap stops speech.
  - Persistent override: M → Направление (Авто / A → B / B → A). While it is forced, the pill shows "RU → EN" instead of "RU ⇄ EN", so it is never hidden.
- **Feedback.**
  - The waveform bubble appears centred, then slides to the speaker's side when `src_lang` is parsed (A18).
  - The pill shows the direction ("EN → RU") for 2 s.
  - While speaking, the face looks toward the target chip and that chip pulses, so the listener knows the answer is for them.
- **Correction.** RD → "Не тот язык" re-sends the cached clip with the source forced to the other language. The clip is kept in PSRAM until the next turn.
- **Why not two "me / them" buttons:**
  - the board has one BOOT button;
  - two on-screen talk targets would halve the orb;
  - each speaker would have to pick the right button under social pressure;
  - the requirement asks the device to work out the spoken language.

  The chip override keeps the "me / them" power without making it the default.
- **No speaker:** short translations render in `ui_28`, and RD (with the flip) shows them to the other person.

### 5.6 Error states

| Fault | Detection | Pill | Card title / body (RU · EN) | Action → screen |
|---|---|---|---|---|
| No Wi-Fi | not configured, or `WIFI_EVENT_STA_DISCONNECTED` (auto-reconnect, backoff 2 → 60 s) | "Нет Wi-Fi ›" coral; orb OFFLINE | "Нет подключения к Wi-Fi" / "Подключаюсь снова…" · "Not connected to Wi-Fi" / "Reconnecting…" | "Настроить" → W |
| Bad key | HTTP 401 (or 403 with an invalid-key message) | "Ключ не принят ›" coral | "Ключ API не подошёл (401)" · "API key rejected (401)" | "Заменить ключ" → Q (or K hex) |
| No credit | HTTP 402 | "Нет средств ›" yellow | "Закончились средства" / "Пополните баланс на openrouter.ai" · "Out of credit" / "Top up at openrouter.ai" | "Подробнее" → P |
| Low credit | `/api/v1/key` `limit_remaining` < $0.50, checked daily (needs a per-key limit; null → never shown) | "Мало средств" yellow, once a day | – | – |
| Region / account blocked | HTTP 403 "Not available in your region" | "Недоступно ›" coral | "Сервис недоступен для этого региона или аккаунта" / "Укажите другого провайдера" · "Not available in your region" | "Провайдер" → P |
| Model gone / expiring | 404, 400 "model not found", or `expiration_date` set at the boot check | "Модель недоступна ›" (or "Модель скоро отключат ›" yellow) | "Модель недоступна" · "Model unavailable" | "Выбрать" → AI |
| Rate limit | 429 (+ Retry-After, default 10 s) | – | "Слишком часто — подождите 10 с" (countdown in the card) | auto-retry once |
| Server / timeout | 5xx, 524, 529, connect > 10 s, response > 30 s, SSE `finish_reason: "error"` | – | "Сервер не ответил" · "Server didn't respond" | "Повторить" (re-sends the cached clip) |
| TLS / DNS | esp-tls error | – | "Не удаётся связаться с сервером" | "Повторить", "Диагностика" → D |
| Not heard | empty transcript, or VAD < 300 ms | – | neutral status "Не расслышал"; face SOFT; no card | – |
| Mic dead | flat or zero RMS for the whole clip | "Микрофон ›" coral | "Микрофон молчит" · "Microphone is silent" | "Диагностика" → D |
| No speaker | configuration flag | voice button F6A9 + yellow dot | "Динамик не подключён" / "Отвечаю текстом" | "Проверить звук" → D |
| TTS failed | `/audio/speech` error | – | text shown, with the note "голос недоступен" | – |
| Low battery | ≤ 15 % and ≤ 5 % while discharging (§3.16) | toast "Батарея 15 %", then "Батарея 5 %", once each per discharge; Home battery and lock ring turn `error` below 15 % | – | – |

The SSE parser must ignore `: OPENROUTER PROCESSING` comment lines and treat `finish_reason: "error"` inside a 200 response as a failure.

### 5.7 Onboarding (first boot)

1. **O1, Language.** The mini orb opens its eyes and glances left and right. Title "Привет! · Hello!". Primary "Русский", secondary "English".
2. **O2, Network.** "Подключим Wi-Fi". Primary "С телефона (QR)" → Q (Wi-Fi + key + model on one page). Secondary "Выбрать сеть здесь" → W → K → WC.
3. **O3, Key.** Skipped if Q delivered it. Primary "С телефона (QR)", secondary "Ввести вручную" (K hex). Subtitle "Ключ: openrouter.ai/keys".
4. **O4, Speaker.** "Проверим звук" plays three chimes. Primary "Слышу", secondary "Нет динамика"; the latter sets `s_speaker = 0` and text-only, the primary turns voice replies on (`text_only` = 0). Both are saved (`speaker`).
5. **O5, First try.** Hero orb with the hint "Скажите «Привет»". This is a real round trip; any fault is shown with its fix (§5.6). On success: face HAPPY, "Готово" → H.
   - Firmware today: title "Готово!", subtitle "Держите шар или BOOT и говорите" (281 px of 290), primary "Начать", and in the secondary button's place a two-line key guide in `ui_16` `text-2`: "BOOT ×2 — смена режима" (212 px; x 60–300, y 276–296, 166.9 OK) and "PWR — блокировка" (155 px; x 95–265, y 298–318, 162.1 OK).
   - Every step goes one step back with a right swipe (O1 leaves the flow to H); PWR locks.

### 5.8 Sleep, ambient, power

| Event | Result |
|---|---|
| 30 s idle | backlight → 25 % (LEDC hardware fade, 800 ms); face SLEEPY |
| +60 s | Z ambient, backlight 15 % |
| +5 min | screen off: backlight 0, mic off, LVGL timers paused, the LVGL task sleeps |
| Wake | BOOT wakes to H/H′ and **records at once**. PWR on a dark screen or a touch wakes; the waking touch is swallowed. After ≥ 15 min, a new session starts (empty hero) |
| PWR short | locks (from a dimmed or ambient screen too; it lights first). Locked: dimmed after 10 s, off after 20 s, never ambient; PWR unlocks in one press from any of these (§3.16) |
| PWR held | at 0.6 s the rim POWER ring starts and fills over 2.4 s; release retracts it in 200 ms. At 100 %: on battery GPIO7 goes low (power off); on USB the screen goes off and the toast "Питание от USB" appears |

### 5.9 Earcons and first-use hints

**Earcons** play only with a speaker and "Звуки" on, at 30 % of master volume. They are sine tones with an envelope, so there are no assets:
- Listening starts: 660 → 880 Hz, 60 ms.
- Sent: 880 → 660 Hz, 60 ms.
- Cancel: 440 Hz, 40 ms.
- Error: 330 Hz, 120 ms.

**First-use hints.**
- `s_hints` starts at 3 and drops by 1 after each successful turn.
- While it is above 0, the UI shows the H bottom hint and the instructive status strings.
- On the first translator turn, a one-time card explains: "Держите RU или EN — язык выбирается вручную".
- D → "Показать подсказки" resets the counter.

---

## 6. Animations and the frame budget

### 6.1 Budget model

**Bus.** RGB565 is 2 B/px, so 40 MB/s means **0.05 µs per pixel**; it doubles at 40 MHz.

Reference areas:

| Area | Pixels | Flush time |
|---|---|---|
| Hero orb | 204² = 41.6k | 2.1 ms |
| Compact orb | 8.5k | – |
| Chat viewport | 41.3k | – |
| List viewport | 68k | – |
| Keyboard | 67.7k | – |
| Full screen | 129.6k | 6.5 ms |

The two partial buffers let DMA of one chunk overlap rendering of the next. A 204 px-wide area renders in 70-row strips, which is 3 chunks.

**Render cost.** These are planning estimates. Calibrate them in milestone UI-0 with `lv_demo_benchmark` and `LV_USE_SYSMON`. The 9.5 software renderer runs without S3 SIMD: esp_lvgl_port builds it only for LVGL 9.1.x.

| Operation | Estimated cost |
|---|---|
| Opaque fill | 10 ns/px |
| Vertical gradient | 15 ns/px |
| Alpha fill | 25 ns/px |
| Anti-aliased edge | 100 ns per edge pixel |
| 4-bpp text | 50 ns per glyph-box pixel |
| Overhead | 0.3 ms per invalidated area + 0.1 ms per chunk |

- Example, hero orb (24 bars, face): about 0.4 (background) + 0.8 (halo/core fills with overdraw) + 0.2 (edges) + 0.7 (bars) + 0.1 (face) + 0.6 (overhead) ≈ **2.8 ms**. Budget it at ≤ 6 ms.

**Rules:**

| Rule | Limit |
|---|---|
| Continuous (steady) animation | ≤ 50k px and ≤ 8 ms per frame |
| Transitions | ≤ 90k px per frame, for ≤ 500 ms |
| Full-screen redraw | only on a screen change or keyboard map swap, as one frame, never animated |
| Continuous orb motion | one **orb ticker** `lv_timer` at 33 ms (30 fps), parameters from `lv_tick_get()`; it runs only in LISTENING, UPLOADING, THINKING and SPEAKING |
| One-shot transitions | `lv_anim` / `lv_anim_timeline` at `LV_DEF_REFR_PERIOD 16` |
| Orb invalidation | in continuous states the whole orb box (one area). Idle micro-motion (blink, glance) invalidates only the eye/mouth union box: hero 84×76 ≈ 6.4k px |
| Invalidated areas per frame | at most about 8 (orb, `ui_rim` deltas, labels), well under the default `LV_INV_BUF_SIZE` of 32, so no override is needed |

**Acceptance.**
- With lv_sysmon running on the device, every row in §6.2 must hold ≥ 30 fps with Wi-Fi and TLS active.
- LVGL CPU on core 1 must stay ≤ 70 % during A6/A12 at 80 MHz QSPI.
- Fallbacks, applied in order:
  1. hero bars 24 → 16;
  2. drop halo2 during SPEAKING;
  3. ticker 33 → 40 ms;
  4. draw buffers 360×40 → 360×60 (2 × 43.2 KB) if internal RAM allows.

### 6.2 Animation table

| # | Animation | LVGL mechanism | Animated properties | Duration · easing | Invalidated / frame | Est. render / frame |
|---|---|---|---|---|---|---|
| A1 | Idle breathing | 2 × `lv_anim` on orb params; exec invalidates only when an integer radius changes | h1 r 80↔84, h2 r 92↔98 (phase +1500 ms) [compact h2 42↔44] | 3000 ms each way, ease_in_out, reverse, infinite | 41.6k [8.5k], about 3–5 redraws/s | ≈ 3 ms each; < 3 % CPU |
| A2 | Blink | `lv_timer` with random period 2.5–6 s (`lv_rand`) starts an `lv_anim` on `blink` 0 → 1024 → 0 | eye height × (1 − blink) | close 70 ms ease_in, hold 40, open 110 ms ease_out; 20 % double | eye box 84×56 = 4.7k | < 0.5 ms |
| A3 | Glance / gaze | `lv_anim` on gx, gy | gaze target (random ±6k / ±4k, a touch point, or a chip) | 260 ms ease_in_out, hold 0.8–2 s, return 260 ms | eye box 4.7k (whole orb box if the ticker runs) | < 0.5 ms |
| A4 | Expression change | `lv_anim` 0 → 1024; parameters interpolated; a shape change (capsule ↔ arc) goes through BLINK at mid-point | eye w/h/offset, smile, eye colour | 160 ms ease_in_out | eye box ≤ 6.4k | < 0.7 ms |
| A5 | Press-in → listening | `lv_anim_timeline` | core r 70 → 64 (90 ms ease_out) → 70 (110 ms ease_out); colour mode → listen (`lv_color_mix`, 200 ms); bars 0 → 1 (180 ms ease_out); NEUTRAL → WIDE (A4); pill + settings button opa 255 → 0 (150 ms ease_in, then HIDDEN) | 200 ms | 41.6k + 7k + 2.7k = 51.3k | ≈ 4 ms |
| A6 | Listening level | orb ticker reads `atomic_int mic_level` | bar lengths, core r, eye height | continuous, 30 Hz | 41.6k [8.5k] | ≈ 3 ms [0.8] |
| A7 | Recording ring | `ui_rim` RECORD from a 100 ms `lv_timer` | sweep angle | linear over 30 s | delta span ≈ 12×12 | negligible |
| A8 | Cancel-armed in/out | `lv_anim` | colour → `#6B7380`, face → ✕ glyph | 120 ms ease_out | 41.6k | ≈ 3 ms |
| A9 | Release → uploading | `lv_anim` + ticker | bars 1 → 0 (160 ms ease_in); colour listen → mode mid (200 ms ease_out); eye colour swap inside a blink; progress arc from `s_upload_pct` | 200 ms, then steady | 41.6k | ≈ 3 ms |
| A10 | Thinking | orb ticker + saccade `lv_anim` | arc phases (+300°/s, −200°/s), core r 66↔70 (1.2 s sine), gaze saccade 900 ms ease_in_out | continuous | 41.6k [8.5k] | ≈ 3 ms |
| A11 | Thinking → speaking | `lv_anim_timeline` | arcs opa 255 → 0 (150 ms ease_in); colour → speak (250 ms ease_out); mood expression (A4) | 250 ms | 41.6k | ≈ 3 ms |
| A12 | Speaking level | orb ticker reads `atomic_int tts_level` (RMS per 20 ms I2S block, delayed 60 ms for I2S latency) | core r, 2 echo radii, mouth height | continuous, 30 Hz | 41.6k [8.5k] | ≈ 3 ms |
| A13 | Back to idle | `lv_anim_timeline` | rings → 0 (200 ms ease_in); colour → mode (300 ms ease_in_out); pill / settings opa 0 → 255 (200 ms ease_out) | 300 ms | 51.3k | ≈ 4 ms |
| A14 | Answer-ready (text-only) | `lv_anim` (reverse) | h1 halo1 → mid → halo1; HAPPY | 300 ms ease_out + 600 ms ease_in_out | 41.6k | ≈ 3 ms |
| A15 | Hero → compact (first message) | `lv_anim_timeline` | orb `lv_obj_set_pos/size` y 90 → 244 and box 204 → 92, parameters scaled by k (0–320 ms ease_in_out); hint opa → 0 (0–100 ms); chat container opa 0 → 255 (280–460 ms ease_out) | 460 ms | ≤ 83k (old + new orb box) early, ≤ 50k later | ≤ 9 ms |
| A15r | Compact → hero ("Новый разговор") | same timeline, `lv_anim_timeline_set_reverse()` | – | 460 ms | ≤ 83k | ≤ 9 ms |
| A16 | Message appears | `lv_anim`, exec sets style `opa` + `translate_y` on the wrapper (neither creates a layer) | opa 0 → 255, translate_y 12 → 0 | 220 ms ease_out; reply 80 ms later | ≤ 252×120 = 30k | ≈ 3 ms |
| A17 | Waveform → transcript | 2 × `lv_anim` | waveform child opa 255 → 0, label opa 0 → 255 | 160 ms ease_in_out | ≤ 220×80 = 17.6k | ≈ 2 ms |
| A18 | Translator bubble to its side | `lv_anim` on the bubble's translate_x, then set the wrapper `main_place` START/END and translate_x 0 in the completed callback | translate_x 0 → ±(252 − w)/2 | 220 ms ease_out | ≤ 30k | ≈ 3 ms |
| A19 | Target chip pulse | `lv_anim` on style `outline_width` | 1 → 3 px | 600 ms ease_in_out, reverse, infinite while speaking | 60×60 = 3.6k | < 0.3 ms |
| A20 | Auto-scroll | `lv_obj_scroll_to_y(…, LV_ANIM_ON)` | scroll_y | LVGL-computed, about 200–400 ms | 41.3k + `ui_rim` thumb | ≈ 5 ms (text-dense) |
| A21 | Mode colour morph | `lv_anim` 0 → 255, exec mixes colours | core gradient, halos, pill text colours, side buttons ↔ chips content swap at 50 % | 400 ms ease_in_out | 41.6k + 7k + 2 × 2.7k | ≈ 4 ms |
| A22 | Pill / status text swap, toasts | 2 × `lv_anim` on label opa | old 255 → 0 (100 ms ease_in), swap, new 0 → 255 (160 ms ease_out) | 260 ms | ≤ 9k | < 1 ms |
| A23 | Error shake | `lv_anim`, custom path | core x offset = 6·sin(6πt)·(1 − t) inside the box; colour → error (120 ms) | 360 ms | 41.6k | ≈ 3 ms |
| A24 | List screen entrance | `lv_anim` on the list container (opa multiplies children; no layer) | translate_y 16 → 0, opa 128 → 255 | 180 ms ease_out, after an instant `lv_screen_load` | 68k | ≈ 10 ms |
| A25 | Scroll thumb | `ui_rim` SCROLL on `LV_EVENT_SCROLL`; fade with `lv_anim` on the channel opa | thumb angles; opa → 0 after 600 ms idle (300 ms ease_in) | – | thumb bbox ≤ 60×190 = 11k | ≈ 1 ms |
| A26 | Press feedback | shared `lv_style_transition_dsc_t` | bg_color | 80 ms linear | the target only (≤ 16k) | < 2 ms |
| A27 | Keyboard map swap | instant | – | – | 67.7k once | ≈ 14 ms, single frame |
| A28 | Arc adjuster drag | native `lv_arc` (invalidates the changed span) + value label | indicator angle | finger-driven | ≤ 15k | ≈ 2 ms |
| A29 | PWR hold ring | `lv_anim` on `ui_rim` POWER (delta span only) | 0 → 1000 ‰ | 2400 ms linear after 600 ms; retract 200 ms ease_in | ≤ 4k | < 0.5 ms |
| A30 | Busy comet | 33 ms `lv_timer` on `ui_rim` BUSY | comet angle | 1.2 s per turn | old + new comet bbox ≤ 5k | < 0.5 ms |
| A31 | Boot splash | `lv_anim_timeline` | core r 0 → 70 (500 ms ease_out); halos black → halo (600 ms); eyes open from BLINK at 450 ms, glance left/right | 900 ms | 41.6k | ≈ 3 ms |
| A32 | Backlight dim / wake | LEDC `ledc_set_fade_with_time` | duty | 800 ms / 150 ms | 0 | 0 |
| A33 | Screen change | `lv_screen_load(scr)`, **no load animation**, then `lv_obj_delete(old)` | – | – | 129.6k once | ≈ 20 ms, single frame |
| A34 | Reader flip | style `transform_rotation` 0 ↔ 1800 | – | instant | 46k once through a 185 KB ARGB8888 layer | ≈ 30–60 ms, single frame (measure) |
| A35 | Lock (§3.16) | one linear `lv_anim` 0 → 700 on the overlay; each part eases itself | iris inner r 182 → 0 (0–300 ms ease_in_out); padlock appears at the centre, k 0.6 → 1, pre-mixed 0 → 255 (180–340 ease_out); shackle drops 16 → 0 with ease_out_back, the click (320–450); pulse ring r 40 → 92, fading (380–560 ease_out, over before the ring band fades in, so a frame never queues more than about 18 of the 32 invalidated areas); padlock glides to (180,84), k 1 → 0.55 (450–640 ease_in_out); clock, date, battery ring and line 0 → 255 once the padlock has left the clock (560–700 ease_out) | 700 ms | creating the overlay: 129.6k once; iris band in 12 × 30° pieces ≤ 25k; at 560–700: ring band 12 pieces ≈ 36k + labels ≈ 18k + padlock old/new ≈ 6k (≤ 90k) | ≈ 4–7 ms |
| A36 | Unlock | the same `lv_anim` 0 → 560 | shackle springs 0 → 16 (0–180 ease_out_back); padlock k 0.55 → 0.95 and 255 → 0 (140–340 ease_in); content 255 → 0 (60–240 ease_in); iris inner r 0 → 182 (300–560 ease_in_out) on the screen that was locked | 560 ms (+ up to 400 ms lit first when PWR woke a dark lock screen) | ≤ 66k at 140–240; iris band ≤ 25k; deleting the overlay: 129.6k once | ≈ 4–7 ms |
| A37 | Refused input on the lock screen | `lv_anim` on the padlock's x offset; two `lv_anim` on the hint mix | dx = 6·sin(1080°·u)·(1 − u); date → "Нажмите PWR" (150 ms ease_out), back after 1.4 s (200 ms ease_in) | 360 ms / 1.75 s | padlock box ≈ 3k + 2 labels ≈ 9k | < 1 ms |
| A38 | Charging highlight | 40 ms `lv_timer` while locked, charging and lit | a 36° piece, 43 % toward white, travels along the filled ring, 100°/s, then starts over | continuous | old + new span ≈ 2 × 3k | < 0.5 ms |

### 6.3 Patterns not used, and why

| Pattern | Why |
|---|---|
| `clip_corner` on the root screen or on lists | In 9.5 it renders children into ARGB8888 layers for the rounded bands on every redraw. With a 360 px circle that covers the whole screen. The bezel already hides the corners. |
| Showing, hiding or restyling full-size (336×336) arcs or labels | Each one invalidates 113k px. Rim indicators live in `ui_rim` with span invalidation. |
| `transform_scale` / rotation / skew on animated objects | Creates one unchunked layer per transformed area. The only transform is the static RD flip. |
| Large `box_shadow` and glow shadows | Slow and memory-hungry. Glow = two opaque pre-mixed halos. |
| Lottie / ThorVG / vector graphics | ARGB8888 w·h·4 buffers rendered in software. `LV_USE_LOTTIE 0`. |
| Screen FADE/MOVE loads, tileview, swipe carousels | They redraw 129.6k px every frame and block input. |
| `opa_layered`, blend modes, bitmap masks | They create layers. Style `opa` (no layer) is used instead. |
| Radial and conical gradients | Need `LV_USE_DRAW_SW_COMPLEX_GRADIENTS`. |
| `lv_roller` INFINITE | Copies the option list across pages. Rollers are not used at all. |
| `lv_list`, `lv_menu`, `lv_win`, `lv_obj_bind_flag_if_*` / `lv_obj_bind_state_if_*` | Deprecated in 9.6. |
| Per-part invalidation with dozens of areas | Risk of overflowing `LV_INV_BUF_SIZE` (32), which triggers a full-screen redraw. |
| NVS writes during animation | Flash writes disable the cache, so PSRAM and rodata stall. Saves are debounced and run only in IDLE. |

---

## 7. LVGL 9.5 implementation notes

### 7.1 `lv_conf` / Kconfig

| Setting | Value | Note |
|---|---|---|
| `LV_COLOR_DEPTH` | 16 | RGB565; the port does `swap_bytes` |
| `LV_DEF_REFR_PERIOD` | 16 | one-shot anims up to 60 fps; continuous motion uses the 33 ms ticker |
| `LV_DPI_DEF` | 130 (default) | all sizes are in px; changing it alters `lv_dpx()`-based hit tolerances |
| `LV_USE_OS` | `LV_OS_NONE` | esp_lvgl_port's recursive mutex is the only lock; rendering stays in the LVGL task |
| `LV_USE_DRAW_SW` / `LV_DRAW_SW_COMPLEX` / `LV_DRAW_SW_DRAW_UNIT_CNT` | 1 / 1 / 1 | – |
| `LV_USE_DRAW_SW_ASM` | `LV_DRAW_SW_ASM_NONE` | no S3 SIMD on 9.5 |
| `LV_USE_DRAW_SW_COMPLEX_GRADIENTS` | 0 | – |
| `LV_DRAW_SW_SHADOW_CACHE_SIZE` / `LV_DRAW_SW_CIRCLE_CACHE_SIZE` | 0 / 16 | breathing, listening and thinking use about 14 distinct radii |
| `LV_USE_STDLIB_MALLOC` | `LV_STDLIB_CUSTOM` | `lv_malloc_core` → `heap_caps_malloc_prefer(sz, 2, MALLOC_CAP_SPIRAM\|MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL\|MALLOC_CAP_8BIT)`, and the same for realloc/free |
| `LV_USE_OBSERVER`, `LV_USE_ARCLABEL`, `LV_USE_TRANSLATION`, `LV_USE_QRCODE`, `LV_USE_CANVAS` | 1 | qrcode needs canvas |
| `LV_USE_LABEL`, `LV_USE_ARC`, `LV_USE_BUTTONMATRIX`, `LV_USE_KEYBOARD`, `LV_USE_TEXTAREA` | 1 | xiaozhi sets `LV_USE_KEYBOARD=n`; do not copy that |
| `LV_USE_LIST`, `LV_USE_MENU`, `LV_USE_WIN`, `LV_USE_TILEVIEW`, `LV_USE_ROLLER`, `LV_USE_SWITCH`, `LV_USE_SPINNER`, `LV_USE_MSGBOX`, `LV_USE_LOTTIE`, `LV_USE_THORVG_INTERNAL`, `LV_USE_VECTOR_GRAPHIC` | 0 | – |
| `LV_FONT_MONTSERRAT_*` | all 0 | – |
| `LV_FONT_CUSTOM_DECLARE` / `LV_FONT_DEFAULT` | `LV_FONT_DECLARE(ui_16) LV_FONT_DECLARE(ui_20) LV_FONT_DECLARE(ui_28) LV_FONT_DECLARE(ui_48)` / `&ui_20` | – |
| `LV_USE_FONT_PLACEHOLDER`, `LV_TXT_ENC` | 1, `LV_TXT_ENC_UTF8` | – |
| `LV_USE_THEME_DEFAULT`, `LV_USE_THEME_SIMPLE` | 0, 0 | every part is styled explicitly (§7.5) |
| `LV_OBJ_STYLE_CACHE` | 1 | – |
| `LV_LABEL_TEXT_SELECTION` | 0 | – |
| `LV_USE_SNAPSHOT` | 1 | dev-console screenshots |
| `LV_USE_SYSMON`, `LV_USE_PERF_MONITOR`, `LV_USE_MEM_MONITOR` | 1 in debug builds | – |
| `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM`, `CONFIG_COMPILER_OPTIMIZATION_PERF` | y | – |
| Indev | `long_press_time` 400 ms, `long_press_repeat_time` 100 ms, scroll limit 10 px, scroll throw 10 % | set with `lv_indev_set_*` after creation |

### 7.2 esp_lvgl_port 2.9 and touch

```c
const lvgl_port_cfg_t port_cfg = { .task_priority = 4, .task_stack = 10240, .task_affinity = 1,
                                   .task_max_sleep_ms = 500, .timer_period_ms = 5 };
const lvgl_port_display_cfg_t disp_cfg = {
    .io_handle = lcd_io, .panel_handle = lcd_panel,
    .buffer_size = 360 * 40, .double_buffer = true,            /* 2 × 28.8 KB internal DMA */
    .hres = 360, .vres = 360, .monochrome = false, .color_format = LV_COLOR_FORMAT_RGB565,
    .flags = { .buff_dma = 1, .buff_spiram = 0, .swap_bytes = 1 } };
/* touch: esp_lcd_touch_cst816s (Waveshare documents CST816T: bring-up test) on I2C_NUM_1 (SDA1/SCL3, internal
   pull-ups on), INT GPIO4 -> lvgl_port_add_touch(); the LVGL task wakes on the IRQ. */
```

- **ST77916 alignment.** If the panel shows odd-alignment artefacts, add a `rounder_cb` (esp_lvgl_port ≥ 2.7.1) that sets `x1 &= ~1; x2 |= 1;` and the same for y.
- **Tearing.** No TE: neither init table sends TEON, and QSPI tearing mode is NONE. The animated regions are small, so TE is a research spike, not a dependency.
- **CST816T settings to verify on its datasheet:**
  - DisAutoSleep (0xFE) = 1 while the screen is awake;
  - IrqCtl (0xFA) so that INT pulses repeatedly while touched;
  - LongPressTime (0xFC) = 0, because we detect long presses ourselves.

### 7.3 Widget choices

| Need | Implementation |
|---|---|
| Orb + face | `ui_orb`: one `lv_obj` (style removed, transparent) with `LV_EVENT_DRAW_MAIN`, a custom `LV_EVENT_HIT_TEST` (`LV_OBJ_FLAG_ADV_HITTEST`), `PRESS_LOCK`, and SCROLLABLE / SCROLL_CHAIN / GESTURE_BUBBLE cleared. No child objects; the ■ ✕ ⚠ wifi glyphs are drawn with `lv_draw_label` |
| Rim indicators | `ui_rim`: one custom `lv_obj` on `lv_layer_top()` |
| Titles | `lv_arclabel` + transparent back zone |
| Pill, side buttons, chips, rows, cards, dialog buttons | `lv_obj` + `lv_label` with shared styles and `LV_OBJ_FLAG_CLICKABLE` (`lv_button` adds nothing without a theme) |
| Chat, lists, HIST | `lv_obj` flex column, `LV_DIR_VER`, `LV_SCROLLBAR_MODE_OFF`; messages = wrapper + bubble + label |
| Waveform | `LV_EVENT_DRAW_MAIN` on the bubble, drawing 32 `lv_draw_rect` bars from a uint8 envelope |
| Keyboard | `lv_keyboard` + `lv_textarea` (§4.5) |
| Adjuster | `lv_arc` |
| QR | `lv_qrcode` (I1 canvas, ECC M) on a white `lv_obj` panel |
| Toggles | text "Вкл"/"Выкл" in the row's right slot; the whole 252×64 row is the target |

### 7.4 Custom widget APIs

```c
/* ui_orb.h */
typedef enum { ORB_HERO, ORB_COMPACT, ORB_MINI, ORB_AMBIENT } orb_preset_t;
typedef enum { EXPR_NEUTRAL, EXPR_WIDE, EXPR_LOOK, EXPR_HAPPY, EXPR_SOFT, EXPR_SLEEPY } orb_expr_t;
lv_obj_t *ui_orb_create(lv_obj_t *parent, orb_preset_t p);
void ui_orb_set_preset(lv_obj_t *orb, orb_preset_t p, bool anim);      /* A15 / A15r */
void ui_orb_set_state(lv_obj_t *orb, int32_t ui_state);                /* starts the timelines and the ticker */
void ui_orb_set_expr(lv_obj_t *orb, orb_expr_t e, uint32_t hold_ms);   /* 0 = until the next call */
void ui_orb_look_at(lv_obj_t *orb, lv_point_t p, uint32_t hold_ms);
void ui_orb_set_face(lv_obj_t *orb, bool on);
/* All parameters live in orb_params_t. Draw order: halo2, halo1, core (VER gradient via
   lv_draw_rect + LV_RADIUS_CIRCLE), strokes (lv_draw_line round caps / lv_draw_arc), face or glyph.
   Ticker states: lv_obj_invalidate(orb). Idle blink/glance: lv_obj_invalidate_area(orb, &face_box).
   The box always contains every drawn pixel, so no REFR_EXT_DRAW_SIZE is needed. */

/* ui_rim.h */
void ui_rim_init(void);                          /* creates the object on lv_layer_top() */
void ui_rim_attach_scroll(lv_obj_t *scroller);   /* SCROLL/SCROLL_END events drive the thumb */
void ui_rim_record(int32_t ms, bool warn);       /* ms < 0 hides */
void ui_rim_busy(bool on);
void ui_rim_power(int32_t permille);             /* < 0 hides */
/* Each setter: lv_draw_arc_get_area(cx, cy, r, a0, a1, w, true, &area) for the old and the new span
   (or only the changed delta for sweeps), then lv_obj_invalidate_area(rim, &area). Never lv_obj_invalidate(rim). */
```

### 7.5 Styles

All styles are static `lv_style_t` objects initialised once in `ui_theme_init()` and added with `lv_obj_add_style(obj, &st, selector)`.
- Local `lv_obj_set_style_*` is used only for animated properties (`opa`, `translate_x/y`, `outline_width`) and for per-instance values (the bubble stripe side and colour).
- Accent styles are re-initialised on a mode change, followed by `lv_obj_report_style_change(&st_accent)`.

| Group | Styles |
|---|---|
| Screen | `st_screen` (bg `#000`, opa cover, pad 0, border 0) |
| Chrome | `st_pill` / `st_pill_pr`, `st_btn_round` / `_pr`, `st_chip_a`, `st_chip_b` (+ `_pr`, `_hold`) |
| Lists | `st_row` / `_pr`, `st_row_info` |
| Text | `st_title20`, `st_body20`, `st_caption16`, `st_hint16` |
| Chat | `st_bubble_user`, `st_reply`, `st_turn`, `st_turn_a`, `st_turn_b`, `st_card[status]`, `st_divider` |
| Keyboard | `st_kb_main`, `st_key`, `st_key_pr`, `st_key_dis` (`LV_PART_ITEMS`) |
| Text field | `st_field` (main), `st_field_cursor` (`LV_PART_CURSOR`: `border_side` LEFT, width 2, accent colour, `anim_duration` 500 for the blink) |
| Arc | `st_arc_main` (`arc_width` 16, `surface-2`), `st_arc_ind` (16, accent, rounded), `st_arc_knob` (`text-1`, pad 8) |
| Buttons | `st_stadium_primary`, `st_stadium_secondary` |
| Fades | `st_fade_top`, `st_fade_bottom` |
| Transition | one `lv_style_transition_dsc_t trans_tap` (BG_COLOR, 80 ms), used by every `_pr` style |

### 7.6 Subjects and the 9.6-safe wrapper

| Subject (int) | Values | Written by | Observed by |
|---|---|---|---|
| `s_ui_state` | IDLE, REC, REC_LATCHED, REC_CANCEL, UPLOAD, THINK, SPEAK, ANSWERED, ERROR, OFFLINE | app FSM (via `ui_post`) | orb, status/pill, settings button, `ui_rim` RECORD |
| `s_mode` | 0 assistant, 1 translator | UI (M) | orb colours, pill, side buttons ↔ chips, chat model |
| `s_voice_out`, `s_speaker`, `s_face_on`, `s_sounds` | 0/1 | UI | voice button, FSM, orb, earcons |
| `s_fault` | NONE, NO_WIFI, BAD_KEY, NO_CREDIT, LOW_CREDIT, REGION, MODEL, MODEL_EXPIRING, MIC | net / app | pill, orb OFFLINE, H routing |
| `s_net`, `s_rssi` | OFF / CONNECTING / OK / FAIL; dBm | Wi-Fi task | W, D, faults |
| `s_upload_pct` | 0–100 | net task | orb progress, status |
| `s_volume`, `s_brightness` | 0–100 / 5–100 | UI | A, LEDC observer, audio (atomic copy) |
| `s_ui_lang` | 0 ru / 1 en | UI | `lv_translation_set_language("ru"/"en")`, rebuild of H + the current screen |
| `s_lang_a`, `s_lang_b`, `s_tr_dir` | language index; 0 auto / 1 A→B / 2 B→A | UI | pill, chips, prompt builder |
| `s_battery` (`g_s_battery`) | `UI_BAT_MAKE(pct, flags)`: percent 0–100 or `UI_BAT_NONE` (127), `UI_BAT_CHARGING` / `_FULL` / `_USB` bits | app (`UI_SET_BATTERY`, on change) | Home battery, lock screen ring and line |
| `s_portal` | OFF, AP_UP, PHONE_JOINED, DATA_RX, DONE, FAIL | portal task | Q |
| `s_clock_min` | 0–1439 | RTC/SNTP task, once a minute | Z |
| `s_hints` | 0–3 | app | H hint, hint strings |

Mic and TTS levels and upload bytes are **not** subjects: they are `atomic_int` values read by the ticker. Bindings use only `lv_subject_add_observer_obj()` (present in 9.5 and 9.6), `lv_label_bind_text()` and `lv_arc_bind_value()`.

```c
/* ui_subj.h — the only file that knows which observer API LVGL has */
#pragma once
#include "lvgl.h"
typedef lv_subject_t *ui_subj_t;
ui_subj_t ui_subj_int(int32_t init, int32_t min, int32_t max);
static inline int32_t ui_get(ui_subj_t s) { return lv_subject_get_int(s); }
static inline void ui_set(ui_subj_t s, int32_t v) { if (lv_subject_get_int(s) != v) lv_subject_set_int(s, v); }
static inline lv_observer_t *ui_watch(ui_subj_t s, lv_observer_cb_t cb, lv_obj_t *obj, void *ud)
{ return lv_subject_add_observer_obj(s, cb, obj, ud); }  /* removed automatically with obj */

/* ui_subj.c */
ui_subj_t ui_subj_int(int32_t init, int32_t min, int32_t max)
{
#if LV_VERSION_CHECK(9, 6, 0)
    lv_subject_t *s = lv_subject_create(LV_SUBJECT_TYPE_INT);   /* 9.6: LVGL-owned, starts at 0 */
    lv_subject_set_min_value_int(s, min);                       /* limits before the first value */
    lv_subject_set_max_value_int(s, max);
    lv_subject_set_int(s, init);
#else
    lv_subject_t *s = lv_malloc_zeroed(sizeof(lv_subject_t));   /* 9.5: caller-owned, on the heap */
    LV_ASSERT_MALLOC(s);
    lv_subject_init_int(s, init);
    lv_subject_set_min_value_int(s, min);
    lv_subject_set_max_value_int(s, max);
#endif
    return s;
}
```

- Observer callbacks use the 9.5 signature `void cb(lv_observer_t *o, lv_subject_t *s)`, with `lv_observer_get_target_obj(o)` and `lv_observer_get_user_data(o)`.
- The 9.6 signature of `lv_subject_create` is inferred from the migration page. Check it against the 9.6 header on upgrade; only `ui_subj.c` changes.
- Other 9.6 upgrade notes: headers move to `include/lvgl`, and `LV_COLOR_DEPTH` becomes `LV_COLOR_FORMAT_DEFAULT`.

### 7.7 Thread-safety rules

1. **Only the LVGL task** (esp_lvgl_port, core 1) touches `lv_*` objects, styles, animations, timers and subjects.
2. **Other tasks call `ui_post(&msg)`:**
   - a FreeRTOS queue of 32 × `ui_msg_t`, sent with `xQueueSend(q, &m, 0)`;
   - if the queue is full, non-critical messages are dropped and counted;
   - text payloads are `heap_caps_malloc(MALLOC_CAP_SPIRAM)` buffers whose ownership passes to the UI;
   - an `lv_timer` pump runs every 20 ms and handles up to 16 messages per tick.
3. **High-rate values** (mic RMS, TTS RMS, upload bytes) are `atomic_int` written by their producers and read by the ticker.
4. **The exception path:** `lvgl_port_lock(100)` / `lvgl_port_unlock()` only for rare direct calls from other tasks (dev-console snapshot, touch injection), and the timeout must be handled. Never hold it across I/O or other mutexes, and never take it in an ISR. It is not needed inside the LVGL task.
5. **Event handlers never block.** They post commands to the app FSM (`app_post(CMD_TALK_DOWN)`, …).
6. **Physical buttons:** a GPIO ISR (any edge) sends a task notification to the input task. That task debounces (20 ms), measures the 400 ms tap/hold, and handles the 600 ms / 3 s PWR thresholds, then calls `app_post()` and `ui_post()`. (Firmware today: a 10 ms `esp_timer` poll with 30 ms debounce in `buttons.c`; the app task owns the 300 ms BOOT double-tap window and asks the UI to lock / unlock with `ui_pwr_short()`, which the UI decides in queue order.)

```c
typedef struct { uint16_t type; int32_t a, b; char *text; } ui_msg_t;   /* text owned by the UI after posting */
static void ui_pump_cb(lv_timer_t *t)
{
    ui_msg_t m; int n = 0;
    while (n++ < 16 && xQueueReceive(ui_q, &m, 0) == pdTRUE) { ui_dispatch(&m); if (m.text) heap_caps_free(m.text); }
}
```

### 7.8 Screen lifecycle and memory per screen

- **H is permanent.** Every other screen is built on entry with `ui_nav_push(builder)` and deleted on exit with `ui_nav_back()`, which runs `lv_screen_load(prev)` and then `lv_obj_delete(cur)`.
- The stack depth is at most 4.
- Observers bound with `ui_watch` are removed when their object is deleted.
- Estimates assume about 100 B per object including its style list, plus text.

| Screen | Objects | LVGL heap (PSRAM) | Other |
|---|---|---|---|
| H (permanent) | 16 + 3 per message (≤ 76) | ≈ 30 KB with 20 messages | chat models for both modes ≤ 80 KB PSRAM; last clip ≤ 1 MB PSRAM |
| HIST | 6 + 3 × ≤ 40 | ≈ 40 KB | – |
| RD | 8 | ≈ 3 KB | flip layer 185 KB, transient |
| M / L / LP / S | ≈ 32 / 12 / 45 / 66 | 4–12 KB | – |
| W (≤ 20 networks) | ≈ 90 | ≈ 16 KB | scan records 20 × 80 B |
| AI / V | ≤ 60 | ≤ 11 KB | `/models` parse buffer ≤ 100 KB PSRAM, freed once the curated list is built |
| K | 9 + keyboard (≤ 24 buttons × 10 B) | ≈ 3 KB | textarea ≤ 512 B |
| A | 8 | ≈ 2 KB | – |
| Q | 10 + qrcode | ≈ 3 KB + I1 buffer (22 B × 174 rows ≈ 3.8 KB) + encoder temp ≈ 1 KB | portal HTTP server in its own task |
| D | ≈ 75 | ≈ 14 KB | – |
| C / T_STEP / Z / T_TOUCH | 7 / 10 / 4 / 3 | ≤ 2 KB each | – |
| `ui_rim`, arc titles | 1 / 2 per screen | < 1 KB | – |

**Fixed costs:**
- internal SRAM: draw buffers 57.6 KB (DMA) + LVGL task stack 10 KB;
- flash: fonts ≈ 150–220 KB, keyboard maps ≈ 3 KB.

Peak LVGL heap (H + HIST + transient) stays under 120 KB, plus the 185 KB flip layer only while RD is flipped.

### 7.9 Files, simulator and test hooks

```
main/ui/
  ui.c/.h            init, nav stack, ui_post queue + pump, dim/ambient timers, swipe-back handler
  ui_theme.c/.h      tokens, shared styles, motion tokens
  ui_subj.c/.h       subjects + the 9.5/9.6 wrapper
  ui_orb.c/.h        orb, face, presets, ticker, state timelines
  ui_rim.c/.h        rim channels with span invalidation
  ui_list.c/.h       curved list (row fade), row builders, arc title + back zone
  ui_kb.c/.h         keyboard layouts, action tables, handler, draw-task styling, field rules
  scr_home.c scr_hist.c scr_reader.c scr_mode.c scr_lang.c scr_settings.c scr_wifi.c scr_text.c
  scr_adjust.c scr_qr.c scr_models.c scr_voices.c scr_provider.c scr_diag.c scr_touch.c
  scr_dialog.c scr_step.c scr_ambient.c scr_lock.c (lock overlay, §3.16)
  i18n/ui_tr_ru.c ui_tr_en.c   (lv_translation_add_static packs; labels use lv_label_set_translation_tag)
  fonts/ui_16.c ui_20.c ui_28.c ui_48.c OFL.txt
```

**Simulator.** `lv_port_pc_visual_studio` at 360×360. The UI code is hardware-free. A simulator-only overlay on `lv_layer_sys()` paints black outside r 180 and an optional r 168 guide. Debug builds can flag any clickable object whose visible box goes past r 168.

**Dev console** (USB serial):

| Command | What it does |
|---|---|
| `ui shot` | `lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565)` → PNG on the PC |
| `ui tap x y [ms]` / `ui drag x1 y1 x2 y2 ms` | a second, virtual pointer indev |
| `ui btn boot\|pwr down\|up` | the same input-task path as the real buttons |
| `ui state <n>` / `ui expr <e>` | forces orb states and expressions for screenshots |
| `ui fps on\|off` | toggles lv_sysmon |
| `ui lang ru\|en` | switches the UI language |
| `lock` / `unlock` / `ui goto lock` | lock screen (§3.16), as PWR does |
| `ui lockanim lock\|unlock <ms>` / `ui lockanim off` | holds frame ms of the lock / unlock animation for screenshots; `off` finishes it |
| `bat` / `bat sim <pct> [charging 0\|1]` / `bat sim off` | the battery model's state; the override for UI tests reaches the UI at once |
| `btn boot double` | two simulated 80 ms BOOT taps 120 ms apart (the mode switch) |

**CI string-fit test.** Every RU and EN string is measured with `lv_text_get_size()` against its slot, and the build fails on any overflow:

| Slot | Max width (px) |
|---|---|
| pill | 124 |
| status line, empty / conversation | 200 / 180 |
| arc title | 147 (Q title: 100) |
| row title / subtitle | 164 / 184 |
| prompt label | 150 |
| H hint line | 100 |
| Q caption 1 / 2 | 220 / 150 |
| key labels | key width − 8 |
| Home battery (glyph + percent, `ui_16`) | 70 |
| lock screen date / hint (`ui_16`) / battery line after the bolt (`ui_20`) | 220 / 140 |
| O5 key guide lines (`ui_16`) | 240 / 170 |

---

## 8. To verify on the hardware before freezing

1. **Render throughput** of 9.5 without SIMD at 360×360. Measure every §6.2 row with lv_sysmon at 80 MHz and at 40 MHz QSPI, and apply the §6.1 fallbacks if a row drops below 30 fps.
2. **CST816T reporting.** Does it report continuously while dragging (scrolling) and keep reporting "pressed" during a stationary hold over 3 s (on-screen hold-to-talk, chip hold)? Check the auto-sleep, IrqCtl and long-press registers.
3. **Physical position and labels of BOOT and PWR** on the enclosure. Hints say "BOOT" until this is known. Also check whether a long PWR press cuts power in hardware.
4. **ST77916 panel batch** (reg 0x04), whether the even-alignment rounder is needed, and visible tearing during chat or list scrolling.
5. **Inter glyph coverage** of every requested range (watch U+20B8 and U+0218–021B) and the real font flash size.
6. **QR scanning** at 6 px/module from 10–20 cm on iOS and Android, and captive-portal auto-open with our DNS hijack.
7. **Streamed structured output.** Do `transcript`, `mood` and `src_lang` arrive early enough to drive A17, A18 and the face during THINKING?
8. **Arclabel quality** at 20 px (rotated-glyph anti-aliasing) and the radius tuning, so that glyphs stay at r ≤ 162.
9. **Backlight PWM** at 15 % duty: no visible flicker (LEDC ≥ 20 kHz).
10. **RD flip layer:** time and memory of one 185 KB rotated render.


## Open questions

- Physical position and labels of the BOOT and PWR buttons on the enclosure, and whether a long PWR press also cuts power in hardware (affects hint copy and the 3 s power-off design).
- Does the CST816T on this rev 2.1 board report continuously while a finger drags, and keep reporting 'pressed' during a stationary hold of more than 3 s? On-screen hold-to-talk, the translator chip hold and scrolling depend on this; BOOT and latched mode are the fallbacks.
- Should the orb face be on by default? The spec defaults to 'Лицо шара: Вкл' with a Settings toggle; this is a product-taste decision for the user.
- Is the user's OpenRouter account usable from their region? Research reports that Russia/Belarus-region accounts lost all access from 2026-06-27. This decides whether the 403 'region' card is an edge case or the first-run experience, and whether a non-OpenRouter provider must be configured during onboarding.
- Which translator languages are required beyond the 14 curated ones? CJK, Arabic and other scripts would show only a romanised display_text, because the fonts do not include those scripts.
- Real LVGL 9.5 frame rate on this board without S3 SIMD, and whether internal RAM next to Wi-Fi, TLS and I2S allows growing the draw buffers from 360x40 to 360x60.
- ST77916 panel batch (register 0x04 result) and whether partial flushes need an even-coordinate rounder; visible tearing while scrolling without TE.
- QR scan reliability at 6 px/module on the user's phone, and whether captive-portal auto-open works with the DNS hijack (otherwise step 2 with the URL QR is always needed).
- Exact OpenRouter key format: the hex keypad assumes 'sk-or-v1-' plus 64 lowercase hex characters; the 'Aa' toggle covers other formats, but the OK-enable rule should be confirmed against a real key.