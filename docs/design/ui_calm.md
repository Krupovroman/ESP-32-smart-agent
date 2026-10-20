# Round Assistant: UI/UX Specification, "Calm Watch" concept

**Version:** 1.0, 2026-10-17.

**Target hardware and software:**
- Waveshare ESP32-S3-Touch-LCD-1.85 rev 2.1.
- ST77916 round IPS, 360×360, RGB565, on QSPI. At 80 MHz the bus runs at about 40 MB/s. At 40 MHz during bring-up it is about 20 MB/s, and every bus time below doubles.
- CST816T single touch.
- ESP-IDF 6.1, LVGL 9.5.x through esp_lvgl_port 2.9.
- PARTIAL render mode with two 360×40 RGB565 draw buffers in internal DMA SRAM (28.8 KB each). Everything else goes to the 8 MB PSRAM.

**Sources:** `docs/requirements.md`, `docs/hardware.md` and `docs/research/stack_2026-10-17.json`. The `verify:` corrections in the research file take priority over the research entries. I also checked these LVGL 9.5 sources directly: `lv_buttonmatrix.c` (hit-area expansion), `lv_arc.c` (ring hit test), `lv_qrcode.c/.h`, `lv_keyboard.c/.h`, `lv_arclabel.h`, and the v9.6 migration notes.

---

## 0. Concept and ground rules

**Calm Watch** is a voice-first object. You glance at it, press, talk and listen. It is not a phone UI.

1. **One hero.** The orb is the only large element. It shows the device state and it is the on-screen talk button.
2. **The physical buttons run the core loop.** **BOOT (GPIO0) = talk.** **PWR (GPIO6) = back / stop / cancel.** A full voice turn never needs the touchscreen.
3. **Two modes, one face.** Assistant and Translator share the Home screen. The orb colour and the label in the top pill show which mode is active.
4. **No hidden gestures.** There are no swipes, no long-press menus and no edge or rim menus. Every function has a visible, labelled target. The only drag in the whole UI is vertical scrolling of lists and the chat.
5. **Status lives in one place.** The top "pill slot" shows the mode when idle, the progress text while busy, and the fault when something is wrong. Tapping a fault takes you to the screen that fixes it.
6. **Everything important sits inside r = 168 px.** That leaves a 12 px rim margin. Each row is sized from the chord at its height. Nothing relies on `clip_corner`; the bezel already hides the corners.
7. **Animated areas stay small.** Steady animation regions are at most about 42k px (32 % of the screen). Full-screen redraws happen only as single frames when the screen changes.
8. **Few screens.** The UI uses 9 templates. Error states are not separate screens.
9. **First-use hints retire themselves.** Inline hints disappear after 3 successful voice turns, so the steady-state UI has almost no text.

### 0.1 Coordinates and fit rules

- Origin is the top-left corner. Centre C = (180, 180). Usable radius R = 168.
- Half-chord at vertical offset d from the centre: `h(d) = sqrt(168² − d²)`. Full chord width: `w(d) = 2·h(d)`.
- **Rectangle** fits if the farthest corner satisfies `sqrt(dx² + dy²) ≤ 168`. A row of keys fits if the row's half-width ≤ h(d) at the row edge farthest from the centre.
- **Stadium** (pill) of size w×h with end radius r = h/2 fits if `sqrt((w/2 − r)² + dy_c²) + r ≤ 168`, where dy_c is the offset of its centre line.
- **Circle** fits if `|centre − C| + r ≤ 168`.
- Angles follow the LVGL convention: 0° = 3 o'clock, increasing clockwise.

Chord reference table (R = 168):

| d | 0 | 38 | 40 | 50 | 60 | 65 | 70 | 80 | 91 | 100 | 110 | 120 | 130 | 140 | 144 | 150 | 160 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| w(d) px | 336.0 | 327.3 | 326.3 | 320.8 | 313.8 | 309.8 | 305.4 | 295.4 | 282.4 | 270.0 | 254.0 | 235.1 | 212.8 | 185.7 | 173.1 | 151.3 | 102.4 |

**Touch targets.** The minimum visible target is 50×50 px. Most targets are 52–80 px. On this panel 1.85" ≈ 194 px/in, so 50 px ≈ 6.5 mm. Neighbouring targets are at least 4 px apart.

---

## 1. Visual language

### 1.1 Colour tokens

The screen background is pure black: it hides the panel edge and makes the orb glow. All "glow" colours below are **precomputed opaque mixes with black**. The renderer never alpha-blends large areas. Fading UI elements use `opa` on small objects only.

| Token | Hex | Use | Contrast on #000 |
|---|---|---|---|
| `bg` | `#000000` | screen background | – |
| `surface-1` | `#111316` | list rows, function keys, cards, translator bubbles | – |
| `surface-2` | `#1A1D22` | letter keys, user bubble, arc track, pressed rows | – |
| `surface-3` | `#2C3138` | pressed keys/buttons, outlines, dividers | – |
| `text-1` | `#F2F4F7` | primary text | ≈19:1 |
| `text-2` | `#A6ADB8` | secondary text, hints, subtitles | ≈9.3:1 |
| `text-3` | `#6B7380` | disabled and decorative only (never body text) | ≈4.4:1 |
| `ink` | `#0B0D10` | text/icons on accent fills | 7.5:1 on assistant, 10:1 on translator |
| `qr-light` / `qr-dark` | `#FFFFFF` / `#000000` | QR panel | – |

### 1.2 Accent colours (modes and states)

Mode colours tint the orb when idle or thinking. State colours mean the same thing in both modes.

| Accent | Meaning | core-top (100 %) | core-bottom (×0.78) | mid (×0.55) | halo1 (×0.30) | halo2 (×0.14) | dim fill (×0.22) |
|---|---|---|---|---|---|---|---|
| assistant | Assistant mode, idle/thinking | `#7C9CFF` | `#617AC7` | `#44568C` | `#252F4D` | `#111624` | `#1B2238` |
| translator | Translator mode, idle/thinking | `#FFA94D` | `#C7843C` | `#8C5D2A` | `#4D3317` | `#24180B` | `#382511` |
| listen | mic open (both modes) | `#34E0C3` | `#29AF98` | `#1D7B6B` | `#10433B` | `#071F1B` | `#0B312B` |
| speak | device speaking | `#B794FF` | `#8F73C7` | `#65518C` | `#372C4D` | `#1A1524` | `#282138` |
| error | failure | `#FF5C5C` | `#C74848` | `#8C3333` | `#4D1C1C` | `#240D0D` | `#381414` |
| warning | low credit, no speaker | `#FFD166` | `#C7A350` | `#8C7338` | `#4D3F1F` | `#241D0E` | `#382E16` |
| success | saved, connected | `#3DDC84` | `#30AC67` | `#227949` | `#124228` | `#091F12` | `#0D301D` |
| offline | no network | `#6B7380` (outline only) | – | – | – | – | – |

- The core uses a **simple vertical gradient** (`bg_grad_dir = LV_GRAD_DIR_VER`) from core-top to core-bottom. That does not need `LV_USE_DRAW_SW_COMPLEX_GRADIENTS`.
- Halos are flat opaque circles.
- RGB565 quantises these dark colours slightly. Flat halos avoid visible banding. The core gradient spans only a 22 % range, so banding stays minimal.

### 1.3 Typography

**Font:** **Inter** (SIL OFL 1.1), static TTF instances from the Inter 4.x release (`Inter-Medium.ttf`, `Inter-SemiBold.ttf`). Inter has full Cyrillic, clear shapes at 16 px on dark backgrounds, and narrower widths than Montserrat, which matters on a round chord. lv_font_conv cannot apply variable-font axes or OpenType features, so use the static instances. Digits are proportional, which is acceptable because all numbers are centred.

| LVGL font | Source | Weight | px | Line height | bpp | Ranges | Icons merged | Est. flash | Used for |
|---|---|---|---|---|---|---|---|---|---|
| `ui_16` | Inter-Medium | 500 | 16 | 20 | 4 | TEXT | I_16 (14) | ≈38 KB | captions, subtitles, status line, user transcript, hints, "пробел/space" key, translator source line |
| `ui_20` | Inter-Medium | 500 | 20 | 26 | 4 | TEXT | I_FULL (40) | ≈55 KB | replies, list titles, pill, buttons, keyboard level 1, orb label |
| `ui_28` | Inter-SemiBold | 600 | 28 | 34 | 4 | TITLE | I_28 (12) | ≈45 KB | keyboard level 2 and hex keys, short translations, reader view, onboarding headlines, orb icons |
| `ui_48` | Inter-SemiBold | 600 | 48 | 56 | 4 | DIGITS | – | ≈7 KB | volume/brightness value, ambient clock |

Total ≈145 KB flash, read in place from rodata, with no RAM cost. The estimates scale from the measured Montserrat figures in the research: ~42 B/glyph at 16 px, ~88 B at 24 px, ~313 B at 48 px at 4 bpp, plus descriptors and kerning.

Glyph ranges:

```sh
# TEXT: ASCII, Latin-1 + Latin Ext-A (de/fr/es/it/pt/pl/cs/tr…), Romanian ș ț,
# the whole Cyrillic block (ru/uk/be/kk/sr/bg incl. Ґґ Ққ…), general punctuation
# (– — ‘ ’ “ ” „ • …), ‰ ‹ ›, € ₴ ₸ ₽, №, ™, arrows, minus
R_TEXT=0x20-0x7E,0xA0-0x17F,0x218-0x21B,0x400-0x4FF,0x2010-0x2027,0x2030,0x2039-0x203A,0x20AC,0x20B4,0x20B8,0x20BD,0x2116,0x2122,0x2190-0x2193,0x2212
# TITLE: UI strings + translations in ru/en/Latin-1 languages
R_TITLE=0x20-0x7E,0xA0-0xFF,0x400-0x45F,0x490-0x491,0x2013-0x2014,0x2018-0x201E,0x2022,0x2026,0x2116
# DIGITS
R_DIG=0x20,0x25,0x2B-0x3A,0x2212
# Font Awesome 5 Free Solid 5.15.x (fa-solid-900.ttf; font OFL 1.1, icons CC BY 4.0)
I_FULL=0xF00C,0xF00D,0xF011,0xF013,0xF023,0xF028,0xF029,0xF04B,0xF04D,0xF053,0xF054,0xF05A,0xF06E,0xF070,0xF071,0xF078,0xF084,0xF0AC,0xF0E7,0xF11C,0xF130,0xF131,0xF185,0xF1AB,0xF1DA,0xF1EB,0xF1F8,0xF240-0xF244,0xF2F1,0xF362,0xF3CD,0xF4AD,0xF544,0xF55A,0xF6A9
I_16=0xF00C,0xF023,0xF053,0xF054,0xF071,0xF0E7,0xF1EB,0xF240-0xF244,0xF6A9
I_28=0xF00C,0xF00D,0xF028,0xF04D,0xF053,0xF071,0xF0AC,0xF130,0xF185,0xF1EB,0xF55A,0xF6A9

lv_font_conv --bpp 4 --size 16 --no-compress --format lvgl \
  --font Inter-Medium.ttf -r $R_TEXT --font fa-solid-900.ttf -r $I_16 \
  --lv-font-name ui_16 -o main/ui/fonts/ui_16.c
lv_font_conv --bpp 4 --size 20 --no-compress --format lvgl \
  --font Inter-Medium.ttf -r $R_TEXT --font fa-solid-900.ttf -r $I_FULL \
  --lv-font-name ui_20 -o main/ui/fonts/ui_20.c
lv_font_conv --bpp 4 --size 28 --no-compress --format lvgl \
  --font Inter-SemiBold.ttf -r $R_TITLE --font fa-solid-900.ttf -r $I_28 \
  --lv-font-name ui_28 -o main/ui/fonts/ui_28.c
lv_font_conv --bpp 4 --size 48 --no-compress --format lvgl \
  --font Inter-SemiBold.ttf -r $R_DIG --lv-font-name ui_48 -o main/ui/fonts/ui_48.c
```

- Use `--no-compress`: compressed fonts cost CPU on every glyph draw.
- Pin the lv_font_conv version in a `tools/package.json` and commit the generated `.c` files, so a rollback never depends on the tool.
- Code points that Inter lacks are skipped by lv_font_conv. Review its warnings once.

**Missing glyphs.** Set `LV_USE_FONT_PLACEHOLDER 1`, so an unknown glyph draws as a box instead of silently vanishing. Run a text sanitiser on every model string before it reaches a label:
- strip markdown (`#`, `*`, `_`, backticks, `>`) and turn a leading "- " into "• ";
- remove emoji and pictographs (U+1F000–1FAFF, U+2600–27BF, U+FE0F, U+200D);
- turn U+2028/2029 into "\n" and collapse runs of 3 or more newlines;
- drop any remaining code point that `ui_20` does not contain.

The system prompt also asks for "plain text, no markdown, no emoji, ≤ 60 words".

For translator targets in scripts we do not ship (CJK, Arabic…), the JSON schema adds a `display_text` field with a romanisation (pinyin, romaji, …). The audio is still spoken in the real language.

### 1.4 Icons

All icons are Font Awesome 5 Free Solid glyphs merged into the text fonts. There are no bitmap images in the UI, so no image cache and no decoder are needed.

| Code point | UTF-8 | FA name | Use |
|---|---|---|---|
| U+F130 | `EF 84 B0` | microphone | onboarding, diagnostics mic test |
| U+F131 | `EF 84 B1` | microphone-slash | mic fault |
| U+F028 | `EF 80 A8` | volume-up | voice button (voice on), volume row |
| U+F6A9 | `EF 9A A9` | volume-mute | voice button (text only), no speaker |
| U+F013 | `EF 80 93` | cog | settings button |
| U+F00C | `EF 80 8C` | check | OK key, selected rows |
| U+F00D | `EF 80 8D` | times | cancel, cancel-armed orb |
| U+F04D | `EF 81 8D` | stop | latched-listening orb core |
| U+F04B | `EF 81 8B` | play | voice preview |
| U+F053 / F054 / F078 | `EF 81 93` / `EF 81 94` / `EF 81 B8` | chevron-left / right / down | header back, row chevron, pill |
| U+F362 | `EF 8D A2` | exchange-alt | "RU ⇄ EN" |
| U+F1AB / F0AC | `EF 86 AB` / `EF 82 AC` | language / globe | translator, UI language |
| U+F544 | `EF 95 84` | robot | model row |
| U+F4AD | `EF 92 AD` | comment-dots | assistant mode row |
| U+F11C | `EF 84 9C` | keyboard | "type a question" |
| U+F1EB | `EF 87 AB` | wifi | Wi-Fi, offline orb |
| U+F084 / F023 | `EF 82 84` / `EF 80 A3` | key / lock | API key, secured networks |
| U+F029 / F3CD | `EF 80 A9` / `EF 8F 8D` | qrcode / mobile-alt | phone setup |
| U+F185 | `EF 86 85` | sun | brightness |
| U+F06E / F070 | `EF 81 AE` / `EF 81 B0` | eye / eye-slash | reveal password |
| U+F071 / F05A | `EF 81 B1` / `EF 81 9A` | exclamation-triangle / info-circle | errors / about |
| U+F55A | `EF 95 9A` | backspace | ⌫ key (= `LV_SYMBOL_BACKSPACE`) |
| U+F2F1 / F1F8 / F1DA | `EF 8B B1` / `EF 87 B8` / `EF 87 9A` | sync / trash / history | retry / clear / history |
| U+F240–F244, U+F0E7 | `EF 89 80`–`84`, `EF 83 A7` | battery levels, bolt | low battery / charging |
| U+F011 | `EF 80 91` | power-off | power row |

Wi-Fi signal strength is drawn as 4 small bars in the row's draw event, not with glyphs.

### 1.5 Shape, spacing, touch

- **Radii:** keys 10, list rows 20, bubbles 16–18, cards 20, pills = h/2, round buttons = circle.
- **Spacing scale:** 4 / 6 / 8 / 12 / 16 / 24 px.
- **No borders, no shadows.** Surfaces are separated by fill colour.
- **Pressed state:** fill changes to `surface-3`, or to accent-mid for accent fills, through an 80 ms style transition. Nothing scales.
- **Disabled state:** text changes to `text-3`; the fill stays the same.

### 1.6 Motion tokens

| Token | Value | Use |
|---|---|---|
| `t_tap` | 80 ms, linear | press feedback |
| `t_fast` | 150 ms, ease_in / ease_out | fades of chrome |
| `t_base` | 220 ms, ease_out | message appear, state entries |
| `t_slow` | 360 ms, ease_in_out | orb hero ↔ compact |
| `t_morph` | 400 ms, ease_in_out | colour morphs |
| `t_breath` | 3000 ms each way, ease_in_out, reverse, infinite | idle breathing |
| ticker | 33 ms (30 Hz) | all continuous orb motion (level rings, arcs) |

Calm means **no bounce or overshoot**, except one 110 ms ease_out "settle" after the orb press-in.

---

## 2. Screens and navigation

### 2.1 Screen inventory

Templates: `T_HOME`, `T_LIST`, `T_TEXT`, `T_ADJUST`, `T_QR`, `T_DIALOG`, `T_STEP`, `T_AMBIENT`, `T_READ`.

| ID | Screen | Template | Purpose | Entered from | Exits to |
|---|---|---|---|---|---|
| S0 | Boot splash | `T_HOME` (orb only) | orb forms (600 ms) | power-on | O1 (first boot) or H |
| O1–O5 | Onboarding | `T_STEP` + `T_QR`/`T_LIST`/`T_TEXT` | language → network → key → speaker test → first try | first boot, factory reset | H |
| H | Home, Assistant face | `T_HOME` | orb + chat | boot; back from anything | – |
| H′ | Home, Translator face | `T_HOME` | the same, amber, language pair | Mode sheet | – |
| H.hist | Chat history (state of H) | – | scroll up the chat; "Новый разговор" | drag chat | – |
| H.read | Reader (translator) | `T_READ` | large text to show the other person | tap a translation bubble | back |
| M | Mode sheet | `T_LIST` | mode choice, languages, direction, volume, type a question, new chat | pill | H |
| L | Languages | `T_LIST` | "Мой язык", "Собеседник", "Направление" → language list | M | M |
| S | Settings | `T_LIST` | all settings | ⚙ button | H |
| W | Wi-Fi networks | `T_LIST` | scan, select, connect | S, O2, "Нет Wi-Fi" pill | S / O3 |
| K | Text entry | `T_TEXT` | round keyboard | W, PE, AI, M ("Написать"), O3 | caller |
| AI | AI model picker | `T_LIST` | curated audio-capable models | S, PE, fault pill | S |
| V | TTS voice picker | `T_LIST` | voices with preview | S | S |
| A | Volume / Brightness | `T_ADJUST` | 270° arc adjuster | S, M | caller |
| Q | Phone setup | `T_QR` | SoftAP + QR + portal progress | S, O2, O3, fault pill | caller / H |
| P | Provider & keys | `T_LIST` | chat / STT / TTS endpoints | S, fault pill | S |
| PE | Endpoint editor | `T_LIST` | base URL, key, model | P | P |
| D | Diagnostics / About | `T_LIST` | versions, network, latency, tests | S | S |
| C | Confirm dialog | `T_DIALOG` | destructive actions | S, D | caller |
| Z | Ambient | `T_AMBIENT` | clock + dot orb | idle timeout | H on wake |
| X | Screen off | – | backlight 0, LVGL idle | PWR at idle, timeout | H on wake |

Error states are not separate screens. They appear in the **pill**, the **orb** and a **card in the chat**, and they route to W, Q, P or AI (§3.11, §5.6).

### 2.2 Navigation map

```
Power ─► S0 ─┬─ first boot ─► O1 Language ─► O2 Network ─┬─► Q Phone setup (Wi-Fi + key in one go) ─┐
             │                                           └─► W Wi-Fi list ─► K password ───────────┤
             │                O3 Key (skipped if Q delivered it) ─► Q  |  K (hex keypad) ───────────┤
             │                O4 Speaker test ─► O5 First try ─────────────────────────────────────┤
             ▼                                                                                      ▼
   ┌──────────────────────────── H  HOME (Assistant ⇄ Translator) ─────────────────────────────────┐
   │ pill ──► M Mode sheet ─► L Languages ─► language list                                         │
   │                       ├► A Volume                                                             │
   │                       ├► K "Написать текстом" ─► sends the question ─► H                       │
   │                       └► "Новый разговор" ─► H (empty)                                        │
   │ ⚙ ─────► S Settings ─► A Volume │ A Brightness │ W Wi-Fi ─► K │ Q Phone │ AI Model ─► K       │
   │                    ├► V Voice │ UI language list │ P Provider ─► PE Endpoint ─► K │ Q │ AI     │
   │                    └► C Clear history │ D Diagnostics ─► C Factory reset                     │
   │ 🔊 button ─ toggles voice/text-only (no navigation)                                            │
   │ fault pill ─► the screen that fixes it (W │ Q │ P │ AI)                                        │
   │ idle 30 s ─► dim 25 %  · +60 s ─► Z Ambient (15 %) · +5 min ─► X screen off                    │
   └───────────────────────────────────────────────────────────────────────────────────────────────┘
Global: PWR short = back one level (at H idle: screen off) · PWR hold 3 s = power off
        BOOT = talk; from any list screen BOOT jumps to H and starts listening (not in K / C / O1–O4)
Maximum depth: H → S → P → PE → K (4 levels).
```

### 2.3 Physical buttons and touch, per context

The tap/hold threshold is **400 ms**, identical to LVGL `long_press_time`.

| Context | BOOT press-and-hold | BOOT tap (< 400 ms) | PWR short | PWR hold 3 s | Touch |
|---|---|---|---|---|---|
| H idle | record; release = send | start **latched** recording | screen off | power off on battery (GPIO7 low); on USB: screen off + "Питание от USB" | orb = talk (same as BOOT); pill = Mode sheet; ⚙ = Settings; 🔊 = voice toggle |
| Recording (hold) | release = send | – | cancel (discard) | – | slide beyond cancel radius = arm cancel |
| Recording (latched) | press = send | press = send | cancel | – | orb tap = send |
| Uploading / thinking | hold ≥ 400 ms = abort + new recording | ignored (orb pulse, hint) | abort request | – | orb: same as BOOT |
| Speaking | press stops speech; held to 400 ms → new recording (barge-in) | stop speech | stop speech | – | orb: same as BOOT |
| Any `T_LIST` / `T_ADJUST` / `T_QR` / `T_READ` | jump to H and record (hold semantics) | jump to H, latched | back | power off | header = back |
| K text entry | ignored (pill toast "Сначала завершите ввод") | ignored | cancel (= ✕) | power off | keys |
| C dialog | ignored | ignored | = "Отмена" | – | buttons |
| O1–O4 | ignored | ignored | previous step | power off | buttons |
| Dimmed (25 %) | wake + act | wake + act | wake | – | the first touch only wakes and is swallowed |
| Z ambient / X off | wake to H + record immediately | wake to H + latched | wake to H | power off | wake (swallowed) |

### 2.4 Why there are no input conflicts

- **No horizontal gestures and no tileview.** Swipe navigation would be invisible (it fails zero-learning). It would compete with scrolling. A tileview is also a full-screen scroll: 129.6k px redrawn every frame. Mode switching is an explicit two-tap action (pill → choose), so it cannot happen by accident. The CST816's on-chip gesture codes are ignored.
- **No rim menu.** Targets near the bezel are partly hidden under the finger and the rim. The only published rotary-keyboard evidence (COMPASS) used a physical rotating bezel, which this board does not have.
- **Hold-to-talk vs scrolling.** The orb is a **sibling** of the chat container, not a child, and it is not scrollable. Its flags:
  - `LV_OBJ_FLAG_PRESS_LOCK`: it stays pressed even when the finger slides off;
  - `LV_OBJ_FLAG_ADV_HITTEST` with a circular hit test;
  - `LV_OBJ_FLAG_SCROLL_CHAIN` and `LV_OBJ_FLAG_GESTURE_BUBBLE` cleared.

  A drag that starts on the orb never scrolls, and a drag that starts on the chat never talks. The screen object itself is not scrollable (`lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE)`).
- **Scrolling vs tapping.** Lists use LVGL's 10 px scroll limit, so a scroll never fires a click. The translator reader opens on `LV_EVENT_CLICKED`, and is ignored if `LV_EVENT_SCROLL_END` happened less than 300 ms earlier. That covers the "tap to stop the fling" case.
- **BOOT covers the case where on-screen hold is unreliable.** If the CST816T drops "pressed" during a long stationary hold (to be verified, §8), the on-screen orb still works in latched mode, and BOOT is never affected.

---

## 3. Geometry per screen (360×360 coordinates)

### 3.1 Home, empty state (no messages in the current mode)

| Element | Type | Geometry | Fit check | Target |
|---|---|---|---|---|
| Pill | stadium, transparent; `surface-1` when pressed | x 110, y 36, 140×50 | (45, 119) → 127.2 + 25 = **152.2 ✓** | 140×50 |
| Pill content | `ui_20` label + chevron-down 16 px | Assistant: "Ассистент ⌄"; Translator: "RU ⇄ EN ⌄" (codes in amber) | ≤ 124 px wide ✓ | – |
| Status line (non-idle; the pill is hidden) | `ui_16` label, `text-2` | centre (180, 61), w ≤ 200, y 51–71 | (100, 129) → **163.2 ✓** | – |
| Orb (hero) | custom-draw `lv_obj` | box x 78, y 90, 204×204; centre **(180, 192)** | max drawn radius 101 + offset 12 = **113 ✓** | circle r 80 |
| Orb label (first-use) | `ui_20`, colour `ink`, 2 lines | centre (180, 192), w ≤ 120: "Нажмите" / "и говорите" (translator: "Говорите" / "RU или EN") | inside core r 70 ✓ | – |
| Voice button | circle Ø52, `surface-1`, icon `ui_20` | centre (100, 292) | (80, 112) → 137.6 + 26 = **163.6 ✓** | 52 |
| Settings button | circle Ø52 | centre (260, 292) | symmetric ✓ | 52 |

- Clearance from the pill bottom (86) to the top of halo2 (192 − 98 = 94) is 8 px.
- Clearance from the orb drawing to each button: |(80, 100)| = 128.1 − 26 = 102.1 > 101 ✓. The orb's bounding box overlaps the button boxes, but the orb hit test is circular and the buttons are created after the orb (on top), so the buttons receive their touches.

### 3.2 Home, conversation state and history (≥ 1 message)

| Element | Geometry | Fit check |
|---|---|---|
| Pill | x 110, y 24, 140×50 | (45, 131) → 138.5 + 25 = **163.5 ✓** |
| Status line (non-idle) | centre (180, 49), w ≤ 180, y 39–59 | (90, 141) → **167.3 ✓** |
| Chat viewport | x 54, y 80, **252×164** (y 80–244), flex column, scroll VER, scrollbar off, momentum + elastic | top corners (126, 100) → **160.9 ✓**; bottom (126, 64) → 141.3 ✓ |
| Top fade | x 54, y 80, 252×24; `bg #000`, `bg_main_opa 255` → `bg_grad_opa 0`, VER | inside viewport |
| Bottom fade | x 54, y 228, 252×16; opa 0 → 255 | inside viewport |
| Rim scroll indicator | `lv_arc` 336×336 at (12, 12), bg angles 320° → 40°, width 4, not clickable; track invisible; thumb `text-3` | ring r 164–168 ✓ |
| Orb (compact) | box x 134, y 244, 92×92; centre **(180, 290)** | 110 + 46 = **156 ✓**; orb top 244 = viewport bottom |
| Voice / Settings buttons | same as §3.1 | to orb: 80 − 26 = 54 > 46 ✓ |

Chat content:
- **Assistant reply:** `ui_20` `text-1`, width 252, no background (calm), `LV_LABEL_LONG_WRAP`. Line 26 px, so 6 lines are visible.
- **User transcript:** `ui_16` `text-2` on a `surface-2` bubble, radius 16, padding 8/12, `max_width 220`, right-aligned.
- **Gaps:** 6 px from a user bubble to its reply, 14 px between turns.
- **System cards:** see §3.11.
- **History:** the first child (shown when ≥ 2 turns) is the stadium button "Новый разговор / New chat", 200×50, `surface-1`, centred, ≥ 50 px tall ✓. There are no dates or extra chrome.
- **Idle divider:** after 15 min idle a `ui_16` `text-3` divider "— новый разговор —" is inserted and the model context resets.
- **Limits:** at most **20 messages** per mode. The oldest are deleted. The text model is kept in PSRAM per mode and persisted to the `storage` partition (last 20).

### 3.3 Translator variants

- **Faces.** The empty and conversation faces of H′ have the same geometry as §3.1/§3.2. The orb and pill accent are `translator`.
- **Turn bubble:** `surface-1`, radius 18, padding 10/12, width ≤ 236.
  - Utterances in "my language" (A) are right-aligned; the other person's (B) are left-aligned.
  - Line 1 (`ui_16`): direction "RU → EN", with the target code in amber.
  - Line 2 (`ui_16` `text-2`): transcript.
  - Line 3: the translation in `text-1`. It uses `ui_28` if it has ≤ 48 characters and every glyph exists in `ui_28` (checked with `lv_font_get_glyph_dsc` per code point), otherwise `ui_20`.
- **Reader view** (`T_READ`, opened by tapping a translation bubble):
  - text box x 54, y 80, 252×200: corners (126, 100) → **160.9 ✓**;
  - `ui_28` centred, scroll VER;
  - small direction label `ui_16` at centre (180, 300), w ≤ 200: (100, 130) → 164 ✓;
  - tap anywhere or PWR → back.

### 3.4 Ambient (Z)

| Element | Geometry | Check |
|---|---|---|
| Clock "14:32" | `ui_48` `text-1` at 70 % brightness level; centre (180, 166), w ≤ 160, y 138–194 | corners (80, 42) ✓ |
| Date "пт, 17 октября" | `ui_16` `text-2`, centre (180, 212), w ≤ 220 | (110, 42) ✓ |
| Dot orb | orb preset AMBIENT: core r 12, halo1 r 18, centre (180, 262) | 82 + 18 = 100 ✓ |
| Battery (only if < 20 % or charging) | `ui_16` icon, centre (180, 300) | ✓ |

The ambient face redraws once a minute (clock observer) and breathes the dot at 8 s cycles (redrawing a 36×36 area).

### 3.5 List template (`T_LIST`)

Used by M, L, S, W, AI, V, P, PE, D, the UI-language list and onboarding pickers.

| Element | Geometry | Check | Target |
|---|---|---|---|
| Header (= back) | stadium x 110, y 20, 140×50; content "‹ Title" `ui_20`; optional busy arc r 9 at the right inside the pill | (45, 135) → 142.3 + 25 = **167.3 ✓** | 140×50 |
| List viewport | x 54, y 76, 252×270 (y 76–346), flex column, gap 6, `pad_top 4`, `pad_bottom 134` (so the last row can reach the centre), scroll VER, momentum on, no snap | – | – |
| Row | 252×64, radius 20, `surface-1` | fully inside R for row-centre offset \|d_c\| ≤ 79 (corner (126, d_c+32) ≤ 168) | 252×64 |
| Rim scroll indicator | same as §3.2 | ✓ | – |

**Curved-edge behaviour (no layers, no `clip_corner`).** On `LV_EVENT_SCROLL`, each visible row gets `opa = 255` for \|d_c\| ≤ 79, falling linearly to 0 at \|d_c\| = 119. A row whose opa is below 128 loses `LV_OBJ_FLAG_CLICKABLE`. Faded rows may reach into the 12 px rim margin. At d_c = 100 the rounded corner is at 174 px, still inside the physical r = 180 and at 47 % opacity. That is acceptable for non-important, transitional content.

**Initial position.** The first row is centred at y = 112 (rows 80–144, 150–214, 220–284; the fourth row is faded).

**Row anatomy:**

| Part | Position | Font / colour |
|---|---|---|
| Icon | centre (x+28, y+32) | `ui_20`, accent or `text-2` |
| Title | x+52, y+10, w ≤ 164, `LONG_DOT` | `ui_20`, `text-1` (≤ 15 chars) |
| Subtitle | x+52, y+38, w ≤ 184, `LONG_DOT` | `ui_16`, `text-2` (≤ 21 chars) |
| Right slot | centre (x+226, y+32) | chevron (`text-3`), ✓ check (accent) for the selected radio, "Вкл/Выкл" `ui_16` for toggles, or Wi-Fi bars |

Info rows (diagnostics) put the title in `ui_16` `text-2` and the value in `ui_20` `text-1`, and are not clickable. Destructive rows use an `error` icon and title.

**Content of each list:**

- **M, Mode sheet:**
  - Ассистент (radio, "Отвечает на вопросы");
  - Переводчик (radio, "RU ⇄ EN");
  - *(translator only)* Языки ("Русский ⇄ English" ›) and Направление ("Авто" ›);
  - Громкость ("70 %" ›);
  - Написать текстом (›);
  - Новый разговор ("Забыть контекст").

  Selecting a mode returns to H with the colour morph (A13).
- **L, Languages:** Мой язык ›, Собеседник ›, Направление (Авто / A→B / B→A) ›. Each opens a radio list of the curated languages, shown by native name: Русский, English, Deutsch, Français, Español, Italiano, Português, Polski, Türkçe, Nederlands, Čeština, Svenska, Українська, Қазақша. The subtitle says "только текст" when the current TTS has no voice for that language. Picking the language already in the other slot swaps the two.
- **S, Settings** (titles ≤ 15 chars):
  - Громкость, Яркость;
  - Wi-Fi (SSID);
  - Телефон (QR);
  - Модель ИИ (model name);
  - Голос (voice name);
  - Ответ голосом (Вкл/Выкл; same state as 🔊);
  - Динамик (Подключён/Нет);
  - Язык меню (Русский);
  - Провайдер (openrouter.ai);
  - История (Очистить);
  - О системе;
  - Выключение.
- **W, Wi-Fi:**
  - "↻ Искать снова";
  - networks sorted by RSSI: SSID `ui_20`, subtitle "Подключено"/"Защищена"/"Открытая", 4-bar meter and lock icon at the right;
  - "Другая сеть…" (opens K for the SSID);
  - "С телефона (QR)".
- **AI, Model picker:** a curated list built from `GET /api/v1/models?input_modalities=audio&output_modalities=text&sort=pricing-low-to-high`, filtered as follows:
  - keep only models whose `supported_parameters` include `response_format` and `structured_outputs`;
  - drop `:batch`, `-preview`, `~` aliases, negative prices and non-null `expiration_date`.

  The list shows at most 12 models. The subtitle is the estimated cost per turn, computed as 320 audio + 300 prompt + 150 output tokens, e.g. "≈ $0.0005 / ответ". Default: Gemini 3.1 Flash-Lite. Extra rows: "↻ Обновить список" and "Свой ID…" (K, or Q).
- **V, Voice picker:**
  - for MAI: Маша, Лев, Грант, Харпер (`ru-RU-*:MAI-Voice-2.1-Flash`);
  - for Gemini TTS: Kore, Puck, Charon, …;
  - subtitle: provider and price;
  - tapping a row selects it and plays a 40-character preview ("Привет! Так звучит мой голос.").
- **P, Provider & keys:**
  - Чат (host · model);
  - Распознавание ("Как чат (аудио в модель)");
  - Голос (host · TTS model);
  - Ключ API ("sk-or-…a9f3 ✓");
  - Настроить с телефона;
  - Проверить ключ: `GET /api/v1/key` shows "Лимит: осталось $4.20" or "Без лимита".
- **PE, Endpoint editor:** Адрес (URL), Ключ, Модель, and "Как у чата" (toggle).
- **D, Diagnostics / About:**
  - info rows: firmware version and slot (ota_0/1), ESP-IDF/LVGL versions, panel ID (reg 0x04), MAC, IP, RSSI, uptime, free internal/PSRAM heap, last latency ("загрузка 0.9 с · ответ 1.6 с · голос 0.7 с");
  - actions: Тест микрофона (live level row), Тест динамика, Показать FPS (lv_sysmon toggle), Показать подсказки (resets the hint counter), Перезагрузить, Сброс настроек (C).

### 3.6 Text entry (`T_TEXT`), layout around the keyboard (keyboard itself in §4)

| Element | Geometry | Check | Target |
|---|---|---|---|
| Prompt label (what is being entered, + counter) | `ui_16` `text-2`, x 105, y 30, 150×20, `LONG_DOT`, e.g. "Пароль · HomeNet", "Ключ · 37/64" | (75, 150) → **167.7 ✓** | – |
| Cancel ✕ | circle Ø50, `surface-1`, centre (76, 86) | (104, 94) → 140.2 + 25 = **165.2 ✓** | 50 |
| Text field | `lv_textarea` one-line, x 105, y 61, 150×50, radius 14, `surface-2`, `ui_20`, pad 12/14 | corners (75, 119) → **140.7 ✓** | 150×50 |
| Reveal 👁 (secret fields only; otherwise hidden) | circle Ø50, centre (284, 86) | symmetric ✓ | 50 |
| Keyboard | `lv_keyboard` x 18, y 115, 324×209 | see §4.4 | ≥ 50 |

There is a 4 px gap between ✕ (x 51–101) and the field (x 105). About 13 characters are visible in the field, and it scrolls to the cursor. Password mode uses the bullet "•" (U+2022) and shows each new character for 1.5 s (`LV_TEXTAREA_DEF_PWD_SHOW_TIME`).

### 3.7 Arc adjuster (`T_ADJUST`), volume and brightness

| Element | Geometry | Check | Target |
|---|---|---|---|
| Arc | `lv_arc` x 30, y 30, 300×300 (R 150); bg angles 135° → 45° (270° through the top); range 0–100 (brightness 5–100); width 16; track `surface-2`, indicator accent; knob Ø32 (`pad 8`) | ring r 134–150, knob ≤ 158 ✓ | `ext_click_area 17` → hit ring r 117–167 = **50 px** thick |
| Caption | `ui_16` `text-2`, "Громкость" / "Яркость", centre (180, 128) | ✓ | – |
| Value | `ui_48` "70%", centre (180, 176), w ≤ 140 | ✓ | – |
| − / + | circles Ø52, centres (144, 254) and (216, 254), step 5 | (36, 74) → 82.3 + 26 = 108.3 < 117: inside the arc's inner hit radius ✓ | 52 |
| "Готово" | stadium 120×50 in the bottom gap, x 120, y 290 | (35, 135) → 139.5 + 25 = **164.5 ✓**; its corners lie at 61°/119°, more than 16° inside the 45°–135° gap, so the arc's angle check rejects touches there (checked in `lv_arc.c`) | 120×50 |

- Brightness applies live through LEDC.
- Volume plays a 60 ms tick on release, if a speaker is present.
- PWR or "Готово" goes back.
- Keep the default `change_rate` of 720°/s; it prevents value jumps.

### 3.8 Phone setup (`T_QR`)

| Element | Geometry | Check |
|---|---|---|
| Header | stadium as in §3.5: "‹ Телефон" | ✓ |
| QR panel | white rounded rect x 85, y 78, 190×190, radius 16 | far corner (95, 102) → **139.4 ✓** |
| QR | `lv_qrcode`, dark `#000`, light `#FFF`, centred in the panel; **size = modules × scale** (no LVGL quiet zone; the white panel margin of ≥ 4 modules is the quiet zone) | – |
| Caption line 1 | `ui_16` `text-1`, centre (180, 286), w ≤ 220 | (110, 116) → 159.9 ✓ |
| Caption line 2 | `ui_16` `text-2`, centre (180, 308), w ≤ 180 | (90, 138) → 164.8 ✓ |

Steps:

1. **Join the device's network.**
   - QR payload: `WIFI:T:WPA;S:Orb-A1B2;P:k7m2q9x4;;` (34 bytes → version 3 at ECC M, which LVGL uses → 29 modules × **scale 5 = 145 px**; margin 22 px ≈ 4.5 modules).
   - The SSID suffix is the last 2 MAC bytes. The password is 8 random characters from `23456789abcdefghjkmnpqrstuvwxyz`, new each session.
   - Captions: "Сеть: Orb-A1B2" / "Пароль: k7m2q9x4".
2. **Open the portal** (shown automatically when `WIFI_EVENT_AP_STACONNECTED` fires).
   - QR payload: `http://192.168.4.1/` (19 bytes → version 2 → 25 modules × **scale 6 = 150 px**).
   - Captions: "Откройте 192.168.4.1" / "Ждём данные…" with a busy arc.
   - The captive-portal DNS usually opens the page by itself.
3. **Data received.** The QR is replaced by checklist rows "Wi-Fi ✓ / Ключ API ✓ / Модель ✓", then "Подключаюсь…". On success: back to H with the success toast "Готово".

Further rules:
- The portal is our own `esp_http_server` page. esp-wifi-connect cannot take an API key or model id.
- It closes after a save, after 10 min, or on PWR.
- Backlight is forced to 100 % while a QR is shown.
- **LVGL 9.5 call order:** `lv_qrcode_set_size()`, then the colours, then `lv_qrcode_update(qr, data, len)`. In 9.5, size or quiet-zone changes made after the data are silently dropped (fixed in 9.6).

### 3.9 Confirm dialog (`T_DIALOG`)

| Element | Geometry | Check |
|---|---|---|
| Icon | `ui_28`, error or warning colour, centre (180, 96) | ✓ |
| Title | `ui_20` `text-1`, centre (180, 132), w ≤ 260 | (130, 48) ✓ |
| Body | `ui_16` `text-2`, x 60, y 150, 240×60, up to 3 lines, centred | (120, 60) → 134 ✓ |
| "Отмена" | stadium x 56, y 226, 120×50, `surface-1` | (35, 71) → 79 + 25 ✓ |
| "Удалить"/"Сбросить" | stadium x 184, y 226, 120×50, `error` fill, `ink` text | symmetric ✓ |

PWR means "Отмена".

### 3.10 Onboarding step (`T_STEP`)

| Element | Geometry | Check |
|---|---|---|
| Mini orb | preset MINI (core r 28, halo2 r 40), centre (180, 88) | 92 + 40 = 132 ✓ |
| Title | `ui_28` (or `ui_20` for long strings), centre (180, 153), w ≤ 280 | ✓ |
| Subtitle | `ui_16` `text-2`, centre (180, 182), w ≤ 290 | ✓ |
| Primary button | stadium x 62, y 204, 236×56, accent fill, `ink` `ui_20` | (90, 52) → 103.9 + 28 = 131.9 ✓ |
| Secondary button | stadium x 80, y 268, 200×56, `surface-1`, `text-1` | (72, 116) → 136.5 + 28 = **164.5 ✓** |
| Progress dots | 5 × Ø6 at y 338, x 156/168/180/192/204; current = accent, others `text-3` | (24, 158) → 159.8 + 3 ✓ |

### 3.11 Error presentation (no separate screen)

**Pill.** Persistent faults replace the pill text with the fault in its colour, e.g. "Нет Wi-Fi ›", "Ключ не принят ›", "Нет средств ›". The pill is up to 160 wide: (55, 131) → 141.6 + 25 = 166.6 ✓. While the fault is active, tapping the pill opens the fixing screen instead of M.

**Orb.** The orb shows ERROR for 2.4 s (§5.3). If the fault persists, it then shows OFFLINE (hollow) or IDLE.

**Chat card** (system message):

| Property | Value |
|---|---|
| Size | width 252, radius 20 |
| Background / padding | status "dim fill" colour, padding 14 |
| Row 1 | icon `ui_20` in the status colour + title `ui_20` `text-1` |
| Row 2 | body `ui_16` `text-2`, ≤ 2 lines |
| Row 3 | optional action stadium 160×50, status-colour fill, `ink` text |
| Height | ≈ 14 + 26 + 6 + 40 + 10 + 50 + 14 = **160 ≤ 164 viewport ✓** |

In the empty state, the first error card switches H to the conversation layout.

---

## 4. The round keyboard

### 4.1 Decision: two-step "group → letter" keypad (phone-keypad groups)

Measured capacity: with keys of at least 50×50 px, the four rows that fit under a text field hold **≈ 14–19 keys in total** (§4.4). Six or seven of those must be function keys (⌫, OK, space, language, 123, punctuation). That leaves about 12 letter slots per page. The Russian alphabet has 33 letters.

| Option | Problem on this screen |
|---|---|
| ЙЦУКЕН / QWERTY rows | 11–12 keys per row need ≥ 572 px. Split into pages, RU needs **3 pages** of about 12 letters, and the user has to hunt across pages. |
| Long-press for ё/ъ or variants | Hidden, so it fails zero-learning. It conflicts with ⌫ auto-repeat. Popovers would clip at the rim (LVGL docs say they extend past the widget). |
| Multi-tap T9 | Depends on timing; "аб" needs a pause. High error rate for passwords. |
| Rim / rotary picker | No evidence for a touch-only bezel-less rim; COMPASS used a physical bezel. Fingers slip off the edge. |
| **Two-step groups (chosen)** | Every letter is exactly **2 taps**, with no timing and no hidden states. All 33 letters are visible on level 1, in alphabetical order, exactly like the Russian phone keypad (АБВГ · ДЕЁЖЗ · …). Level 2 shows the group's letters as **huge keys with lower case and upper case both visible**, so there is **no Shift key and no caps-lock state**, the most common cause of a wrong Wi-Fi password. |

- **Cost:** about 1.2 s per character, so "HomeNet2024" takes about 18 s. This keyboard is the fallback. The primary entry paths are the phone portal (Wi-Fi, keys, URLs, model ids) and voice (questions).
- **API keys:** OpenRouter keys get a dedicated **single-tap hex keypad**: the prefix `sk-or-v1-` is pre-filled and locked, then 64 hex characters take 64 taps.

### 4.2 Keyboard geometry

- `lv_keyboard`: x 18, y 115, **324×209**; `pad_all 0`, **`pad_row 3`**, **`pad_column 4`**; no background or border.
- Row height = (209 − 3·3)/4 = **50 px**.
- Row y ranges: **R1 115–165**, **R2 168–218**, **R3 221–271**, **R4 274–324**.
- Key style: radius 10.
  - Letter and group keys: `surface-2` / `text-1`.
  - Function keys: `surface-1` / `text-2`.
  - OK: mode accent fill with `ink` text; disabled OK: `surface-1` / `text-3`.
  - Pressed: `surface-3`.
- Popovers are off.
- **Hit expansion (verified in `lv_buttonmatrix.c` v9.5).** Each key's hit area grows by `(pad/2) + 1 + (pad & 1)`, capped at `LV_DPI_DEF/10`: **+3 px left/right** (pad_column 4) and **+3 px up/down** (pad_row 3). Visible 50×50 becomes an effective 56×56.
- **Button width formula** (`update_map`): `x1 = (max_w_no_gap · units_before)/units_row + i·pad_col`, where `max_w_no_gap = 324 − (n−1)·4`. All pixel values below come from that formula (±1 px rounding). Rows taper to the circle through **hidden spacer buttons** (`LV_BUTTONMATRIX_CTRL_HIDDEN`, label `" "`; an empty string would end the map).

Row limits: half-chord at each row's far edge.

| Row | y | far-edge d | chord w(d) | half |
|---|---|---|---|---|
| R1 | 115–165 | 65 | 309.8 | 154.9 |
| R2 | 168–218 | 38 | 327.3 | 163.6 |
| R3 | 221–271 | 91 | 282.4 | 141.2 |
| R4 | 274–324 | 144 | 173.1 | 86.5 |

### 4.3 Layouts

#### Level 1, letters (RU shown; EN in brackets). Same geometry for both languages.

```
R1          [ абвг ] [ деёжз ] [ ийкл ]             ([abc] [def] [ghi])
R2  [ 123 ] [ мноп ] [ рсту  ] [ фхцч ] [ ⌫ ]       ([123][jkl] [mno] [pqrs] [⌫])
R3          [ шщъы ] [ ьэюя  ] [пробел]             ([tuv] [wxyz] [space])
R4               [ EN ] [ .,?! ] [ ✓ ]              ([RU] [.,?!] [✓])
```

| Row | Units (hidden = h) | Keys: screen x range (width) |
|---|---|---|
| R1 | h10, 13, 13, 13, h10 | абвг 74–141 (68) · деёжз 146–212 (67) · ийкл 217–284 (68) |
| R2 | 10, 13, 13, 13, 10 | 123 18–69 (52) · мноп 74–141 (68) · рсту 146–212 (67) · фхцч 217–284 (68) · ⌫ 289–341 (53) |
| R3 | h10, 13, 13, 13, h10 | шщъы 74–141 · ьэюя 146–212 · пробел 217–284 |
| R4 | h13, 9, 9, 9, h13 | EN 97–148 (52) · .,?! 153–205 (53) · ✓ 210–261 (52) |

- Group labels use `ui_20`. "деёжз" ≈ 59 px and "шщъы" ≈ 57 px, both inside 67–68 px keys.
- "пробел"/"space" uses `ui_16`, ≈ 50 px.
- The language key shows the language it **switches to**. It is `DISABLED` in ASCII-only fields (passwords, keys, URLs, model ids), which also start in EN.

#### Level 2, the chosen group: lower row + upper row, then back

```
R1   [ а ] [ б ] [ в ] [ г ]          lower case
R2   [ А ] [ Б ] [ В ] [ Г ]          upper case
R3            [ ‹ ]                   back to level 1 (also PWR)
R4   (one hidden key)
```

Level 2 uses font `ui_28`. Choosing a letter inserts it and returns to level 1 immediately.

| Group size | Units R1/R2 | Keys: screen x (width) |
|---|---|---|
| 3 (EN abc…tuv) | h6, 14, 14, 14, h6 | 56–135 (80) · 140–218 (79) · 223–302 (80) |
| 4 (RU, EN pqrs/wxyz, symbol groups) | h2, 12, 12, 12, 12, h2 | 33–102 (70) · 107–177 (71) · 182–251 (70) · 256–325 (70) |
| 5 (деёжз, punctuation) | h2, 11, 11, 11, 11, 11, h2 | 32–87 (56) · 92–147 (56) · 152–206 (55) · 211–266 (56) · 271–326 (56) |
| R3 back | h10, h13, 13, h13, h10 | ‹ 146–212 (67) |

Group contents:

| Key | Lower (R1) | Upper (R2) |
|---|---|---|
| абвг | а б в г | А Б В Г |
| деёжз | д е ё ж з | Д Е Ё Ж З |
| ийкл | и й к л | И Й К Л |
| мноп | м н о п | М Н О П |
| рсту | р с т у | Р С Т У |
| фхцч | ф х ц ч | Ф Х Ц Ч |
| шщъы | ш щ ъ ы | Ш Щ Ъ Ы |
| ьэюя | ь э ю я | Ь Э Ю Я |
| abc / def / ghi / jkl / mno / tuv | 3 letters | 3 capitals |
| pqrs / wxyz | 4 letters | 4 capitals |
| .,?! (punctuation, size 5) | `. , ? ! -` | `: ; ( ) "` |

That covers all 33 Russian letters (8 groups: 7×4 + 1×5) and all 26 English letters.

#### Digits (single tap), reached with "123"

```
R1          [ 1 ] [ 2 ] [ 3 ]
R2  [ абв ] [ 4 ] [ 5 ] [ 6 ] [ ⌫ ]
R3          [ 7 ] [ 8 ] [ 9 ]
R4       [ #+= ] [ 0 ] [ ✓ ]
```

Same units and x ranges as level-1 letters. Digits use `ui_28`. "абв"/"abc" returns to the current-language letters.

#### Symbols level 1 (two-step like letters; covers all 32 printable ASCII symbols), reached with "#+="

```
R1          [ !?., ] [ @#$% ] [ &*^~ ]
R2  [ абв ] [ -_=+ ] [ ()[] ] [ {}<> ] [ ⌫ ]
R3          [ /\|` ] [ :;'" ] [ пробел ]
R4       [ 123 ] [ «»—… ] [ ✓ ]
```

The symbol groups' level 2 uses the 4-key shape in R1 only. R2 is a single hidden key, R3 is back.

#### Hex keypad (OpenRouter key; the field holds `sk-or-v1-` + 64 hex)

```
R1  [1][2][3][4][5]
R2  [6][7][8][9][0][⌫]
R3    [a][b][c][d]
R4    [e][f][✓]
```

| Row | Units | Keys: screen x (width) |
|---|---|---|
| R1 | h1, 12×5, h1 | 26–83 (58) · 88–145 (58) · 150–208 (59) · 213–270 (58) · 275–332 (58) |
| R2 | 1×6 | 18–67 (50) · 72–122 (51) · 127–177 (51) · 182–231 (50) · 236–286 (51) · 291–341 (51) |
| R3 | h3, 8×4, h3 | 46–109 (64) · 114–177 (64) · 182–245 (64) · 250–313 (64) |
| R4 | h13, 9, 9, 9, h13 | e 97–148 · f 153–205 · ✓ 210–261 |

Font `ui_28`. OK stays `DISABLED` until exactly 64 hex characters are entered, and the prompt shows "Ключ · n/64". If the key does not match `^sk-or-v1-[0-9a-f]{64}$`, or the provider is not OpenRouter, the normal EN keyboard is used.

### 4.4 Proof that every row fits

| Layout / row | Row span (screen x) | Half-span | Allowed half-chord | Result |
|---|---|---|---|---|
| L1 R1 / R3 (also digits, symbols) | 74–284 | 106 | 154.9 / 141.2 | ✓ / ✓ |
| L1 R2 | 18–341 | 162 | 163.6 | ✓ |
| L1 R4 (3×52) | 97–261 | 83 | 86.5 | ✓ |
| L2 3-key R1, R2 | 56–302 | 124 | 154.9, 163.6 | ✓ |
| L2 4-key R1, R2 | 33–325 | 147 | 154.9, 163.6 | ✓ |
| L2 5-key R1, R2 | 32–326 | 148 | 154.9, 163.6 | ✓ |
| L2 R3 | 146–212 | 34 | 141.2 | ✓ |
| Hex R1 (tightest) | 26–332 | 154 | 154.9 | ✓ (corner 167.2 px from centre) |
| Hex R2 | 18–341 | 162 | 163.6 | ✓ |
| Hex R3 | 46–313 | 134 | 141.2 | ✓ |

The smallest visible key is 50×50 (hex R2); the effective target is 56×56. Group keys are 68×50 (74×56 effective). Level-2 letters are 55–80 × 50 (61–86 × 56 effective).

### 4.5 Mapping onto `lv_keyboard` / `lv_buttonmatrix`

`lv_keyboard` is a buttonmatrix with an attached textarea. Facts checked in v9.5:
- `lv_keyboard_set_map(kb, mode, map, ctrl)` writes into **file-static tables shared by all keyboards** and re-applies the current mode at once.
- `lv_keyboard_set_mode()` **returns early if the mode is unchanged**.
- The constructor adds `lv_keyboard_def_event_cb` on `VALUE_CHANGED`.

So: use **one slot, `LV_KEYBOARD_MODE_USER_1`**, swap maps into it, and replace the default handler with our own. Our handler classifies keys by **button index** through a per-layout action table, not by comparing UTF-8 labels.

```c
/* ui_kb.c — LVGL 9.5 */
#define SP   " "                                   /* hidden spacer; "" would end the map */
#define HID  LV_BUTTONMATRIX_CTRL_HIDDEN
#define TAP  (LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT)
#define U(n) ((lv_buttonmatrix_ctrl_t)(n))          /* relative width 1..15 */

static const char * const kb_ru_l1[] = {
    SP, "абвг", "деёжз", "ийкл", SP, "\n",
    "123", "мноп", "рсту", "фхцч", LV_SYMBOL_BACKSPACE, "\n",
    SP, "шщъы", "ьэюя", "пробел", SP, "\n",
    SP, "EN", ".,?!", LV_SYMBOL_OK, SP, "" };
static const char * const kb_en_l1[] = {
    SP, "abc", "def", "ghi", SP, "\n",
    "123", "jkl", "mno", "pqrs", LV_SYMBOL_BACKSPACE, "\n",
    SP, "tuv", "wxyz", "space", SP, "\n",
    SP, "RU", ".,?!", LV_SYMBOL_OK, SP, "" };
static const char * const kb_digits_ru[] = {
    SP, "1", "2", "3", SP, "\n",
    "абв", "4", "5", "6", LV_SYMBOL_BACKSPACE, "\n",
    SP, "7", "8", "9", SP, "\n",
    SP, "#+=", "0", LV_SYMBOL_OK, SP, "" };          /* kb_digits_en: "abc" */
static const char * const kb_sym_l1_ru[] = {
    SP, "!?.,", "@#$%", "&*^~", SP, "\n",
    "абв", "-_=+", "()[]", "{}<>", LV_SYMBOL_BACKSPACE, "\n",
    SP, "/\\|`", ":;'\"", "пробел", SP, "\n",
    SP, "123", "«»—…", LV_SYMBOL_OK, SP, "" };
/* ⌫ has no TAP flag: it fires on PRESSED and auto-repeats via LONG_PRESSED_REPEAT (100 ms) */
static const lv_buttonmatrix_ctrl_t kb_l1_ctrl[] = {
    HID|U(10), TAP|U(13), TAP|U(13), TAP|U(13), HID|U(10),
    TAP|U(10), TAP|U(13), TAP|U(13), TAP|U(13),     U(10),
    HID|U(10), TAP|U(13), TAP|U(13), TAP|U(13), HID|U(10),
    HID|U(13), TAP|U(9),  TAP|U(9),  TAP|U(9),  HID|U(13) };

/* Level 2, 4-letter shape (example: абвг) */
static const char * const kb_l2_abvg[] = {
    SP, "а", "б", "в", "г", SP, "\n",
    SP, "А", "Б", "В", "Г", SP, "\n",
    SP, SP, LV_SYMBOL_LEFT, SP, SP, "\n",
    SP, "" };
static const lv_buttonmatrix_ctrl_t kb_l2_4_ctrl[] = {
    HID|U(2), TAP|U(12), TAP|U(12), TAP|U(12), TAP|U(12), HID|U(2),
    HID|U(2), TAP|U(12), TAP|U(12), TAP|U(12), TAP|U(12), HID|U(2),
    HID|U(10), HID|U(13), TAP|U(13), HID|U(13), HID|U(10),
    HID|U(1) };
/* kb_l2_3_ctrl: h6,14,14,14,h6 ; kb_l2_5_ctrl: h2,11×5,h2 ; kb_l2_sym_ctrl: R1 as 4, R2 = HID|1 */

static const char * const kb_hex[] = {
    SP, "1", "2", "3", "4", "5", SP, "\n",
    "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    SP, "a", "b", "c", "d", SP, "\n",
    SP, "e", "f", LV_SYMBOL_OK, SP, "" };
static const lv_buttonmatrix_ctrl_t kb_hex_ctrl[] = {
    HID|U(1), TAP|U(12), TAP|U(12), TAP|U(12), TAP|U(12), TAP|U(12), HID|U(1),
    TAP|U(1), TAP|U(1), TAP|U(1), TAP|U(1), TAP|U(1), U(1),
    HID|U(3), TAP|U(8), TAP|U(8), TAP|U(8), TAP|U(8), HID|U(3),
    HID|U(13), TAP|U(9), TAP|U(9), TAP|U(9), HID|U(13) };

typedef enum { KA_NONE, KA_CHAR, KA_GROUP, KA_BKSP, KA_SPACE, KA_OK,
               KA_LANG, KA_DIGITS, KA_SYMBOLS, KA_LETTERS, KA_BACK } kb_act_t;
typedef struct {
    const char * const *map; const lv_buttonmatrix_ctrl_t *ctrl;
    const uint8_t *act;   /* kb_act_t per button index (no "\n" entries) */
    const uint8_t *arg;   /* group id for KA_GROUP */
    const lv_font_t *font;
} kb_layout_t;

static const kb_layout_t *cur, *l1;    /* l1 = current-language level 1 */
static lv_obj_t *kb, *ta;

static void kb_apply(const kb_layout_t *L) {
    cur = L;
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_1, L->map, L->ctrl); /* applies immediately */
    lv_obj_set_style_text_font(kb, L->font, LV_PART_ITEMS);
}

static void kb_value_cb(lv_event_t *e) {
    uint32_t id = lv_buttonmatrix_get_selected_button(kb);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    switch ((kb_act_t)cur->act[id]) {
    case KA_CHAR:   lv_textarea_add_text(ta, lv_buttonmatrix_get_button_text(kb, id));
                    if (cur != l1 && cur->font == &ui_28 && cur->act == act_l2) kb_apply(l1);
                    break;                          /* level 2 returns to level 1 */
    case KA_GROUP:  kb_apply(&kb_l2_layouts[cur->arg[id]]); break;
    case KA_BKSP:   lv_textarea_delete_char(ta); break;
    case KA_SPACE:  lv_textarea_add_char(ta, ' '); break;
    case KA_OK:     lv_obj_send_event(ta, LV_EVENT_READY, NULL); break;
    case KA_LANG:   l1 = (l1 == &kb_ru) ? &kb_en : &kb_ru; kb_apply(l1); break;
    case KA_DIGITS: kb_apply(l1 == &kb_ru ? &kb_digits_ru_l : &kb_digits_en_l); break;
    case KA_SYMBOLS:kb_apply(l1 == &kb_ru ? &kb_sym_ru_l : &kb_sym_en_l); break;
    case KA_LETTERS:
    case KA_BACK:   kb_apply(l1); break;
    default: break;
    }
}

/* Per-key colours/fonts without extra objects (LVGL 9 draw-task API) */
static void kb_draw_cb(lv_event_t *e) {
    lv_draw_task_t *t = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *b = lv_draw_task_get_draw_dsc(t);
    if (b->part != LV_PART_ITEMS) return;
    kb_act_t a = (kb_act_t)cur->act[b->id1];
    bool pressed = lv_obj_has_state(kb, LV_STATE_PRESSED) &&
                   lv_buttonmatrix_get_selected_button(kb) == b->id1;
    lv_draw_fill_dsc_t  *f = lv_draw_task_get_fill_dsc(t);
    lv_draw_label_dsc_t *l = lv_draw_task_get_label_dsc(t);
    if (a == KA_OK && !lv_buttonmatrix_has_button_ctrl(kb, b->id1, LV_BUTTONMATRIX_CTRL_DISABLED)) {
        if (f) f->color = pressed ? ui_accent_mid() : ui_accent();
        if (l) l->color = UI_INK;
    } else if (a >= KA_BKSP && !pressed) {
        if (f) f->color = UI_SURFACE_1;
        if (l) l->color = UI_TEXT_2;
    }
    if (a == KA_SPACE && l) l->font = &ui_16;
}

lv_obj_t *ui_kb_create(lv_obj_t *parent, lv_obj_t *textarea, ui_kb_kind_t kind) {
    kb = lv_keyboard_create(parent); ta = textarea;
    lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);   /* own handler only */
    lv_obj_add_event_cb(kb, kb_value_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(kb, kb_draw_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_keyboard_set_popovers(kb, false);
    lv_keyboard_set_textarea(kb, ta);                     /* keeps cursor/focus handling */
    lv_obj_set_pos(kb, 18, 115); lv_obj_set_size(kb, 324, 209);
    lv_obj_set_style_pad_all(kb, 0, 0);
    lv_obj_set_style_pad_row(kb, 3, 0); lv_obj_set_style_pad_column(kb, 4, 0);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_1);
    l1 = (kind == KB_TEXT_RU) ? &kb_ru : &kb_en;
    kb_apply(kind == KB_HEX ? &kb_hex_l : l1);
    return kb;
}
```

- Each L2 map (8 RU, 8 EN, punctuation, 8 symbol groups, «»—…: 26 in total) is a `static const` array in flash, about 100 B each. Ctrl maps are shared by shape (3 / 4 / 5 / symbol).
- `lv_buttonmatrix_set_map` stores the map pointer, so maps must be static. It reallocates the button areas, at about 8 B per button.
- A map swap redraws only the keyboard: 324×209 = 67.7k px, one frame of about 14 ms. No animation.

### 4.6 Field behaviours

| Field | Start layout | Accepted / limits | Secret | OK enabled when |
|---|---|---|---|---|
| Wi-Fi password | EN, language key disabled | printable ASCII 0x20–0x7E, 8–63 chars (WPA2) | yes | len ≥ 8 (or open network) |
| SSID ("Другая сеть") | EN | ≤ 32 **bytes** (checked in UTF-8 bytes, not characters) | no | len ≥ 1 |
| OpenRouter key | HEX | `0-9a-f`, 64 after the locked prefix (`lv_textarea_set_accepted_chars`) | yes | exactly 64 |
| Other API key / base URL / model id | EN | printable ASCII, ≤ 200 | key yes | len ≥ 1; URL must start `http` |
| Question to the assistant | UI language (RU) | any, ≤ 500 | no | len ≥ 1 |

- **✕ or PWR** sends `LV_EVENT_CANCEL` to the textarea and returns to the caller; the caller keeps the draft only for questions.
- **👁** toggles `lv_textarea_set_password_mode`.
- Tapping the field does nothing (the cursor always stays at the end), which avoids precise cursor placement.
- **BOOT** is ignored on this screen; see §2.3.

---

## 5. Interaction design

### 5.1 Push-to-talk: hold and tap both work, with no setting

The mic runs into a **0.5 s PSRAM ring buffer** whenever the screen is awake (16 KB; never leaves RAM unless the user presses). On press, the last 250 ms are prepended so the first syllable is not lost.

| From | Event | To | Effects |
|---|---|---|---|
| IDLE | press (BOOT or orb) | REC_PRESS | capture starts with pre-roll; earcon ↑; orb → LISTENING; status "Слушаю…" |
| REC_PRESS | release after ≥ 400 ms | SEND | earcon ↓ |
| REC_PRESS | release after < 400 ms | REC_LATCHED | core shows ■; status "Нажмите — отправлю" |
| REC_PRESS (orb) | finger > cancel radius (hero 110 px, compact 70 px from the orb centre) | REC_CANCEL | orb grey, ✕ in core; status "Отпустите — отмена" |
| REC_CANCEL | finger back inside | REC_PRESS | – |
| REC_CANCEL | release | IDLE | audio discarded; earcon "cancel" |
| REC_LATCHED | press (BOOT or orb) | SEND | sends on press-down (snappy) |
| REC_LATCHED | VAD: ≥ 400 ms speech, then 1200 ms silence | SEND | automatic |
| REC_LATCHED | no speech for 5 s | IDLE | neutral status "Не расслышал" 2 s |
| REC_* | 25 s elapsed | – | status countdown "Ещё 5 с" … "1 с" |
| REC_* | 30 s elapsed | SEND | automatic |
| REC_* | PWR | IDLE | discard, "Отменено" |
| SEND | VAD speech < 300 ms | IDLE | "Слишком коротко" 1.5 s, **no request** (saves money) |
| SEND | otherwise | UPLOADING | encode, POST |
| UPLOADING | body fully written | THINKING | – |
| THINKING | `transcript` field arrives (streamed JSON) | THINKING | the user bubble appears early |
| THINKING | reply text | SPEAKING (voice on + speaker present), else ANSWERED → IDLE | reply bubble |
| SPEAKING | playback finished | IDLE | – |
| SPEAKING | press (BOOT/orb) | IDLE, or REC_PRESS if still held at 400 ms | TTS flushed within 50 ms; recording starts after the flush (no pre-roll, so no TTS tail is captured) |
| UPLOADING / THINKING | tap | – | orb pulse; hint "PWR — отмена" (first uses) |
| UPLOADING / THINKING | hold ≥ 400 ms | REC_PRESS | old request aborted |
| UPLOADING / THINKING | PWR | IDLE | abort, "Отменено" |
| any | network/API error | ERROR (2.4 s) | then IDLE or OFFLINE (§5.6) |

The FSM runs in the **app task**. The UI only mirrors it through `s_ui_state` (§7.5).

### 5.2 Cancel, interrupt, text-only

- **Cancel recording:** PWR, or slide off the orb and release.
- **Interrupt speech:** tap the orb, BOOT, PWR, or the 🔊 button (which also switches to text-only).
- **Barge-in:** hold BOOT or the orb while the device speaks; it stops and listens.
- **Text-only toggle:** the 🔊/🔇 button at (100, 292) and the matching row in Settings.
  - The state persists (NVS write debounced by 2 s, done while no animation runs).
  - Toggling shows a 1.5 s pill toast "Голос выкл." / "Голос вкл.".
  - In text-only mode the orb goes THINKING → ANSWER-READY pulse (A9) → IDLE.
- **No speaker** (configured in onboarding or Settings): replies are always text. The 🔇 icon has a 6 px `warning` dot. Tapping it posts a card "Динамик не подключён. Отвечаю текстом" with the action "Проверить звук".

### 5.3 What the orb shows

Hero values (compact values in brackets). L = mic level 0..1 and T = TTS level 0..1, both smoothed (attack 0.6, release 0.12 per 33 ms tick). Mic L = clamp((dBFS + 50)/40, 0, 1).

| State | Core | Halos | Ring / arcs | Centre content | Status (pill slot) |
|---|---|---|---|---|---|
| IDLE · Assistant | r 70 [30], gradient `#7C9CFF`→`#617AC7` | h1 `#252F4D` r 80↔84 [36]; h2 `#111624` r 92↔98 [42↔44], breathing 3 s | – | first-use label (`ink`) | pill "Ассистент ⌄" |
| IDLE · Translator | `#FFA94D`→`#C7843C` | `#4D3317` / `#24180B` | – | "Говорите / RU или EN" | "RU ⇄ EN ⌄" |
| LISTENING | `#34E0C3`→`#29AF98`, r 70+4L [30+2L] | h1 `#10433B` r 80 fixed; h2 hidden | 24 bars [16], width 6 [4], round caps, `#34E0C3`; r_in 78 [34], length 4+16·Lᵢ [3+7·Lᵢ] → r_out ≤ 98 [44]; Lᵢ = L·(0.6+0.4·sin(φ+i·2π/7)), φ += 0.09 rad/tick | latched: ■ (`ink`, `ui_28`) | "Слушаю…" / "Нажмите — отправлю" / "Ещё 5 с" |
| CANCEL-ARMED | `#6B7380` flat | hidden | bars `#6B7380` | ✕ | "Отпустите — отмена" |
| UPLOADING | mode mid (`#44568C` / `#8C5D2A`) | h1 = mode dim | progress arc r 82 [36], width 4 [3], mode accent from −90° clockwise to `s_upload_pct`; track `surface-2` | – | "Отправляю 62%" |
| THINKING | mode mid, r 66↔70 every 1.2 s | h1 mode dim | arc A r 82 [36] w 4, 70°, +300°/s; arc B r 90 [41] w 3 [2], 40°, −200°/s; mode accent, opa 255 / 140 | – | "Думаю…" → after 6 s "Ещё немного…" |
| SPEAKING | `#B794FF`→`#8F73C7`, r 70+4T [30+2T] | h1 `#372C4D` | echo rings: r 76+12T (w 3, opa 160) and r 84+14T (w 2, opa 90) [32+6T / 37+7T] | translator: target code "EN" (`ink`, `ui_20`) | "Нажмите — стоп" (first uses) |
| ANSWER-READY (text-only) | mode accent | h1 flashes to mode mid 300 ms, back in 600 ms | – | – | – |
| ERROR | `#FF5C5C`→`#C74848`, shake | h1 `#4D1C1C` | – | ⚠ (`ink`, `ui_28`) | short error, coral |
| OFFLINE | hollow: 3 px ring `#6B7380`, fill `#000` | none | – | wifi icon `#6B7380` | pill "Нет Wi-Fi ›" (coral) |
| AMBIENT | r 12, mode accent | h1 r 18, mode halo1, 8 s breathing | – | – | (clock face) |

Orb presets: HERO (box 204, centre (180, 192)), COMPACT (box 92, centre (180, 290)), MINI (box 96, core 28) and AMBIENT (box 40, core 12). Hit test (`LV_EVENT_HIT_TEST`): circle of radius core_r + 10, i.e. 80 / 40 / 38 px. Orb taps in OFFLINE open W; in ERROR they dismiss the error.

### 5.4 Chat behaviour

- **User bubble:** appears as soon as the transcript is known (A11). With a non-streamed JSON reply, the user bubble and the reply appear together, the reply 80 ms later.
- **Reply, voice on:** sentences appear **as their TTS chunk starts playing**, because TTS is pipelined per sentence. If TTS fails, all text appears at once and a `ui_16` `text-3` note "🔇 голос недоступен" goes under the reply.
- **Reply, voice off:** text appears as it streams. The label updates at most every 100 ms, because a label relayout costs scale with the whole text.
- **Auto-scroll:** `lv_obj_scroll_to_y(chat, LV_COORD_MAX, LV_ANIM_ON)` on every new sentence. A manual scroll by the user suspends auto-scroll until the next turn, and the next press of talk jumps back to the bottom.
- **Mode histories:** each mode has its own history. Switching mode rebuilds the chat children (≤ 20, < 10 ms) and picks the empty or conversation layout.
- **Typed question** (M → "Написать текстом"): K returns the text and the FSM goes straight to THINKING with a text-only request. The bubble shows the typed text.

### 5.5 Translator turn-taking: one control with auto-detection

- **One talk control for both people** (the orb or BOOT). The request carries both languages. The JSON schema constrains `src_lang` to `enum [A, B]` and returns `{src_lang, transcript, translation, display_text?}`. The model translates into the other language, and TTS uses the voice mapped to the target language.
- **Why not two buttons ("me"/"them"):**
  - the board has a single BOOT button;
  - two on-screen buttons would halve the hero target;
  - each speaker would have to pick the right button, which is extra learning and a common error under social pressure;
  - the requirement explicitly asks the device to work out the spoken language.
- **Override for edge cases** (names, one-word utterances): M → Направление (Авто / RU→EN / EN→RU). When forced, the pill shows "RU → EN" instead of "RU ⇄ EN", so the override is never hidden.
- **Direction feedback:** for 2 s after detection the pill shows the direction ("EN → RU"). During speaking the core shows the target language code, so the listener knows it is for them.
- **No speaker:** translations display at `ui_28` when short, and a tap opens the Reader view to show the other person.

### 5.6 Error states

| Fault | Detection | Pill | Card title / body (RU · EN) | Action → screen |
|---|---|---|---|---|
| No Wi-Fi | not configured, or `WIFI_EVENT_STA_DISCONNECTED` (auto-reconnect 2→60 s backoff) | "Нет Wi-Fi ›" coral; orb OFFLINE | "Нет подключения к Wi-Fi" / "Подключаюсь снova…" · "Not connected to Wi-Fi" / "Reconnecting…" | "Настроить" → W |
| Bad key | HTTP 401 (or 403 with an invalid-key message) | "Ключ не принят ›" coral | "Ключ API не подошёл (401)" · "API key rejected (401)" | "Заменить ключ" → Q (or K hex) |
| No credit | HTTP 402 | "Нет средств ›" yellow | "Закончились средства" / "Пополните баланс на openrouter.ai" · "Out of credit" / "Top up at openrouter.ai" | "Подробнее" → P (shows the `/api/v1/key` limit) |
| Low credit | `/api/v1/key` limit_remaining < $0.50 (checked daily; a per-key limit must be set) | "Мало средств" yellow (once a day) | – | – |
| Region blocked | 403 "Not available in your region" | "Недоступно ›" coral | "Сервис недоступен в регионе" · "Service not available in your region" | "Провайдер" → P |
| Model gone | 404, or 400 "model not found" | "Модель недоступна ›" | "Модель недоступна" · "Model unavailable" | "Выбрать" → AI |
| Rate limit | 429 (+ Retry-After, default 10 s) | – | "Слишком часто — подождите 10 с" (countdown arc in the card) | – |
| Server / timeout | 5xx, 524, 529, connect > 10 s, response > 30 s, SSE `finish_reason:"error"` | – | "Сервер не ответил" · "Server didn't respond" | "Повторить" (re-sends the cached clip in PSRAM) |
| TLS / DNS | esp-tls error | – | "Не удаётся связаться с сервером" | "Повторить", "Диагностика" → D |
| Not heard | empty transcript or VAD < 300 ms | – | neutral `text-2` status "Не расслышал" (no card, no red) | – |
| Mic dead | all-zero / flat RMS for the whole clip | "Микрофон ›" coral | "Микрофон молчит" · "Microphone is silent" | "Диагностика" → D |
| No speaker | configuration flag (the PCM5101 cannot sense a speaker) | 🔇 + yellow dot | "Динамик не подключён" / "Отвечаю текстом" | "Проверить звук" → D |
| Low battery | ≤ 10 % | toast "Батарея 10 %" | – | – |

### 5.7 Onboarding (first boot)

1. **O1, Language.** Title "Привет! · Hello!". Primary "Русский", secondary "English".
2. **O2, Network.** "Подключим Wi-Fi". Primary "С телефона (QR)" → Q, which takes Wi-Fi + API key + optional model on one page. Secondary "Выбрать сеть здесь" → W → K.
3. **O3, Key.** Skipped if Q delivered the key. Primary "С телефона (QR)", secondary "Ввести вручную" (K hex). Subtitle "Ключ: openrouter.ai/keys".
4. **O4, Speaker.** "Проверим звук" plays three chimes. Primary "Слышу", secondary "Нет динамика"; the latter sets `s_speaker = 0` and voice to text-only.
5. **O5, First try.** Hero orb with the label "Скажите «Привет»". This is a real round trip, and any fault is shown with its fix (§5.6). On success: "Готово" → H.

### 5.8 Sleep, ambient, power

| Event | Result |
|---|---|
| 30 s idle | backlight fades to 25 % (LEDC hardware fade, 800 ms) |
| +60 s | Ambient face, backlight 15 % |
| +5 min | screen off: backlight 0, mic off, LVGL timers paused, the LVGL task sleeps |
| Wake | BOOT wakes to H and **starts listening at once** (voice-first, like raising a watch). PWR or touch wakes to H; the waking touch is swallowed. |
| PWR at H idle | screen off immediately |
| PWR held 3 s | rim progress ring (A21), then power off: GPIO7 low on battery. On USB the screen goes off and the toast says "Питание от USB" (the latch cannot cut USB power). |

### 5.9 Earcons and first-use hints

**Earcons** play only when a speaker is present, at 30 % of the master volume, and are generated as sine tones with an envelope (no assets):

| Event | Tone |
|---|---|
| Listening starts | 660 → 880 Hz, 60 ms |
| Sent | 880 → 660 Hz, 60 ms |
| Cancel | 440 Hz, 40 ms |
| Error | 330 Hz, 120 ms |

**First-use hints.** `s_hints` starts at 3 and drops by 1 after each successful turn. While it is above 0, the UI shows the orb label and the instructive status strings. Diagnostics → "Показать подсказки" restores it.

---

## 6. Animations and the frame budget

### 6.1 Budget model

**Bus cost.** RGB565 is 2 B/px, so the bus costs **0.05 µs per pixel** at 40 MB/s (0.1 µs at 20 MB/s). Reference areas:
- hero orb 204² = **41.6k px**, 2.1 ms;
- compact orb 8.5k px;
- chat viewport 41.3k px;
- list viewport 68k px;
- keyboard 67.7k px;
- full screen 129.6k px, 6.5 ms.

**Render cost.** These are assumptions to verify with lv_sysmon. The LVGL 9.5 software renderer on the S3 runs without the SIMD blend (esp_lvgl_port only compiles it for LVGL 9.1.x). Assume about 8–10 Mpx/s for anti-aliased shapes and about 4–5 Mpx/s for text-dense areas. The two partial buffers let DMA of one chunk overlap rendering of the next. A 204-px-wide area is rendered in strips of 28.8 KB / (204·2) = 70 rows, i.e. 3 chunks.

**Rules:**

| Rule | Limit |
|---|---|
| Steady animation | ≤ 50k px per frame, ≤ 8 ms per frame |
| Transitions | ≤ 90k px per frame, for ≤ 500 ms |
| Full-screen redraw | only once, at a screen change (≈ 25 ms frame), never animated |
| Continuous orb motion | driven by one **orb ticker** `lv_timer` at 33 ms (30 fps), parameters computed from `lv_tick_get()`. `lv_anim` is used only for one-shot transitions, which run at LVGL's refresh rate (`LV_DEF_REFR_PERIOD 16`). |
| Partial invalidation | the orb is one object, so any change redraws its whole box; there is no per-part invalidation and no layers |

**Acceptance test.** With lv_sysmon running on the device, every row of §6.2 must hold ≥ 30 fps, and LVGL CPU on core 1 must stay ≤ 70 % during A3/A7 at 80 MHz QSPI. If not, apply these fallbacks in order: hero bars 24 → 16, drop halo2 while listening, ticker 33 → 40 ms.

### 6.2 Animation table

| # | Animation | LVGL mechanism | Animated properties | Duration · easing | Invalidated / frame | Est. cost / frame |
|---|---|---|---|---|---|---|
| A1 | Idle breathing | 2 × `lv_anim` on orb params; exec_cb invalidates only when the integer radius changes | h1 r 80↔84, h2 r 92↔98 (phase +1500 ms) [compact h2 42↔44] | 3000 ms each way, ease_in_out, reverse, infinite | 41.6k [8.5k], **only ≈ 4–6 redraws/s** | ≈ 6 ms per redraw; < 5 % CPU |
| A2 | Press-in → listening | `lv_anim_timeline` | core r 70→64 (90 ms ease_out) → 70 (110 ms ease_out); colour mode→listen (`lv_color_mix`, 200 ms); bar scale 0→1 (180 ms ease_out); pill and ⚙ `opa` 255→0 (150 ms ease_in, then HIDDEN) | 200 ms | 41.6k + 7k + 2.7k = 51.3k | ≈ 7 ms |
| A3 | Listening level | orb ticker reads `atomic_int mic_level` | bar lengths, core r | continuous, 30 Hz | 41.6k [8.5k] | ≈ 6 ms [1.5 ms] |
| A4 | Release → uploading | `lv_anim` + ticker | bars 1→0 (160 ms ease_in); colour listen→mode mid (200 ms ease_out); progress arc from `s_upload_pct` | 200 ms, then steady | 41.6k | ≈ 5 ms |
| A5 | Thinking | orb ticker | arc phases (+300°/s, −200°/s), core r 66↔70 (1.2 s, sine) | continuous | 41.6k [8.5k] | ≈ 5 ms |
| A6 | Thinking → speaking | `lv_anim_timeline` | arcs opa 255→0 (150 ms ease_in); colour → speak (250 ms ease_out) | 250 ms | 41.6k | ≈ 5 ms |
| A7 | Speaking level | orb ticker reads `atomic_int tts_level` (RMS per 20 ms I2S block) | core r, 2 echo-ring radii | continuous, 30 Hz | 41.6k [8.5k] | ≈ 5 ms |
| A8 | Back to idle | `lv_anim_timeline` | rings → 0 (200 ms ease_in); colour → mode (300 ms ease_in_out); pill/⚙ opa 0→255 (200 ms ease_out) | 300 ms | 51.3k | ≈ 7 ms |
| A9 | Answer-ready pulse | `lv_anim` (reverse) | h1 colour halo1 → mid → halo1 | 300 ms ease_out + 600 ms ease_in_out | 41.6k | ≈ 5 ms |
| A10 | Hero → compact (first message) | `lv_anim_timeline` | orb `lv_obj_set_pos/size` y 90→244, box 204→92, params scaled (0–320 ms ease_in_out); orb label opa 255→0 (0–100 ms); chat container opa 0→255 (280–460 ms ease_out) | 460 ms | ≤ 83k (old + new orb boxes) early, ≤ 50k later | ≤ 12 ms (≥ 30 fps worst case) |
| A10r | Compact → hero ("Новый разговор") | same timeline, `lv_anim_timeline_set_reverse()` | – | 460 ms | ≤ 83k | ≤ 12 ms |
| A11 | Message appears | `lv_anim`, custom exec sets style `opa` + `translate_y` (neither creates a layer) | opa 0→255, translate_y 12→0 | 220 ms ease_out; reply +80 ms | ≤ 252×120 = 30k | ≈ 5 ms |
| A12 | Auto-scroll | `lv_obj_scroll_to_y(..., LV_ANIM_ON)` | scroll_y | LVGL-computed (≈ 200–400 ms) | 41.3k | ≈ 9–10 ms (text-dense) |
| A13 | Mode colour morph | `lv_anim` 0→255, exec mixes colours | core gradient, halos, pill accent | 400 ms ease_in_out | 41.6k + 7k | ≈ 6 ms |
| A14 | Pill/status text swap, toasts | 2 × `lv_anim` on label `opa` | old 255→0 (100 ms ease_in), text swap, new 0→255 (160 ms ease_out) | 260 ms | ≤ 9k | < 1.5 ms |
| A15 | Error shake | `lv_anim`, custom path | core x offset = 6·sin(6πt)·(1−t); colour → error (120 ms) | 360 ms | 41.6k (the shake stays inside the box) | ≈ 5 ms |
| A16 | List screen entrance | `lv_anim` on the list container (`opa` multiplies children, no layer) | translate_y 16→0, opa 128→255 | 180 ms ease_out (after an instant `lv_screen_load`) | 68k | ≈ 13 ms (≥ 30 fps) |
| A17 | Rim scroll indicator | `lv_arc_set_angles` on `LV_EVENT_SCROLL`; `lv_anim` fade | thumb angles; opa → 0 after 600 ms idle (300 ms ease_in) | – | arc segment bbox ≈ 60×190 = 11k | ≈ 1.5 ms |
| A18 | Press feedback | `lv_style_transition_dsc_t` | bg_color | 80 ms linear | the target only (≤ 16k) | < 3 ms |
| A19 | Keyboard map swap | none (instant) | – | – | 67.7k once | ≈ 14 ms, single frame |
| A20 | Arc adjuster drag | native `lv_arc` (invalidates only the changed segment) | indicator angle, value label | – | ≤ 15k | ≈ 3 ms |
| A21 | PWR hold ring | `lv_anim` on `lv_arc` 340×340 at (10, 10), w 4, r 166–170 | value 0→360 | 3000 ms linear; release → retract 200 ms ease_in | segment only | < 2 ms |
| A22 | Boot splash | `lv_anim_timeline` | core r 0→70 (500 ms ease_out), halo colours black → halo (600 ms) | 600 ms | 41.6k | ≈ 5 ms |
| A23 | Backlight dim / wake | LEDC `ledc_set_fade_with_time` | duty | 800 ms / 150 ms | 0 | 0 |
| A24 | Busy arc (headers, Q) | ticker-style `lv_timer` 33 ms | arc rotation, r 9 | continuous | 28×28 | ≈ 0.2 ms |
| A25 | Screen change | `lv_screen_load(scr)`, **no load animation** | – | – | 129.6k once | ≈ 25 ms single frame |

### 6.3 Patterns that are not used, and why

| Pattern | Why |
|---|---|
| `clip_corner` on the root screen or the curved list | In LVGL 9.5 it renders children into ARGB8888 layers for the rounded bands on every redraw. With a 360 circle that covers the whole screen. The bezel already masks the corners. |
| `transform_scale` / rotation / skew on large objects | One layer per transformed area. Sizes animate through custom-draw parameters or width/height instead. |
| Large `box_shadow`, glow shadows | Slow and memory-hungry. "Glow" is two flat opaque halos. |
| Lottie / ThorVG, full-screen vector graphics | ARGB8888 w·h·4 buffers, and the S3 renders vectors in software. `LV_USE_LOTTIE 0`. |
| Full-screen FADE or MOVE screen loads, tileview swipes | Redraw 129.6k px every frame and block input. Screens switch instantly. |
| `opa_layered`, blend modes, bitmap masks | Create simple layers. |
| Radial/conical gradients | Need `LV_USE_DRAW_SW_COMPLEX_GRADIENTS` and per-pixel math. Not needed. |
| `lv_roller` INFINITE | Copies the option list across pages. Not used at all. |
| `lv_list`, `lv_menu`, `lv_win` | Deprecated in 9.6. Lists are plain `lv_obj` flex columns. |

---

## 7. LVGL 9.5 implementation notes

### 7.1 `lv_conf` / Kconfig

| Setting | Value | Note |
|---|---|---|
| `LV_COLOR_DEPTH` | 16 | RGB565; `swap_bytes` set in the port |
| `LV_DEF_REFR_PERIOD` | 16 | one-shot anims up to 60 fps; continuous motion uses the 33 ms ticker |
| `LV_DPI_DEF` | 194 | physical density; raises the buttonmatrix extra-click cap to 19 px |
| `LV_USE_DRAW_SW` / `LV_DRAW_SW_COMPLEX` | 1 / 1 | needed for radii, arcs, round line caps |
| `LV_DRAW_SW_DRAW_UNIT_CNT` | 1 | core 0 stays free for Wi-Fi and audio |
| `LV_USE_DRAW_SW_COMPLEX_GRADIENTS` | 0 | – |
| `LV_DRAW_SW_SHADOW_CACHE_SIZE` / `LV_DRAW_SW_CIRCLE_CACHE_SIZE` | 0 / 8 | the orb uses several radii |
| `LV_USE_STDLIB_MALLOC` | `LV_STDLIB_CUSTOM` | `lv_malloc_core` → `heap_caps_malloc(sz, MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT)` (and realloc/free), so small LVGL objects do not eat internal RAM |
| `LV_USE_OBSERVER` | 1 | – |
| `LV_USE_LABEL`, `LV_USE_ARC`, `LV_USE_BUTTONMATRIX`, `LV_USE_KEYBOARD`, `LV_USE_TEXTAREA`, `LV_USE_CANVAS`, `LV_USE_QRCODE` | 1 | xiaozhi sets `LV_USE_KEYBOARD=n`; do not copy that |
| `LV_USE_LIST`, `LV_USE_MENU`, `LV_USE_WIN`, `LV_USE_TILEVIEW`, `LV_USE_ROLLER`, `LV_USE_SWITCH`, `LV_USE_SPINNER`, `LV_USE_MSGBOX`, `LV_USE_ARCLABEL`, `LV_USE_LOTTIE`, `LV_USE_THORVG_INTERNAL`, `LV_USE_VECTOR_GRAPHIC` | 0 | not used |
| `LV_FONT_MONTSERRAT_*` | all 0 | saves flash |
| `LV_FONT_CUSTOM_DECLARE` / `LV_FONT_DEFAULT` | `LV_FONT_DECLARE(ui_16) LV_FONT_DECLARE(ui_20) LV_FONT_DECLARE(ui_28) LV_FONT_DECLARE(ui_48)` / `&ui_20` | – |
| `LV_USE_FONT_PLACEHOLDER` | 1 | – |
| `LV_TXT_ENC` | `LV_TXT_ENC_UTF8` | – |
| `LV_USE_THEME_DEFAULT` | 0 | own shared styles (§7.4) |
| `LV_USE_SNAPSHOT` | 1 | dev-console screenshots |
| `LV_USE_SYSMON`, `LV_USE_PERF_MONITOR`, `LV_USE_MEM_MONITOR` | 1 in debug builds | – |
| `LV_ATTRIBUTE_FAST_MEM_USE_IRAM` | y | – |
| Compiler | `COMPILER_OPTIMIZATION_PERF` | – |
| Indev timing | `long_press_time` 400 ms, `long_press_repeat_time` 100 ms, scroll limit 10 px, scroll throw 10 % | via `lv_indev_set_*` after creation |

### 7.2 esp_lvgl_port 2.9

```c
const lvgl_port_cfg_t port_cfg = {
    .task_priority = 4, .task_stack = 8192, .task_affinity = 1,   /* LVGL on core 1 */
    .task_max_sleep_ms = 500, .timer_period_ms = 5 };
const lvgl_port_display_cfg_t disp_cfg = {
    .io_handle = lcd_io, .panel_handle = lcd_panel,
    .buffer_size = 360 * 40, .double_buffer = true,               /* 2 × 28.8 KB */
    .hres = 360, .vres = 360, .monochrome = false,
    .color_format = LV_COLOR_FORMAT_RGB565,
    .flags = { .buff_dma = 1, .buff_spiram = 0, .swap_bytes = 1 } };
/* touch: CST816T on I2C1 (SDA1/SCL3), INT GPIO4 → lvgl_port_add_touch(); the task wakes on the IRQ */
```

- If the ST77916 shows odd-alignment artefacts, add an invalidate-area rounder: `x1 &= ~1; x2 |= 1;` and the same for y. Use the esp_lvgl_port `rounder_cb` (2.7.1+) or `LV_EVENT_INVALIDATE_AREA`.
- Tearing: QSPI has no TE coordination in these drivers (TEON is not sent). Animated regions are small, so treat TE as a later research spike, not a dependency.
- Keep NVS writes out of animations: PSRAM is inaccessible while the flash cache is disabled, so a write freezes LVGL for a moment. Settings saves are debounced and run when `s_ui_state == IDLE` and no animation is active.

### 7.3 Widget choices

| Need | Implementation |
|---|---|
| Orb | one `lv_obj` (style removed, transparent) with `LV_EVENT_DRAW_MAIN`. It draws with `lv_draw_rect` (circles via `LV_RADIUS_CIRCLE`, vertical gradient core), `lv_draw_arc` (progress, thinking, echo rings, hollow outline) and `lv_draw_line` (bars, round caps, `lv_point_precise_t`). Plus one child `lv_label` for the label or icon, and a custom `LV_EVENT_HIT_TEST` with `LV_OBJ_FLAG_ADV_HITTEST` and `LV_OBJ_FLAG_PRESS_LOCK`. All parameters live in a struct (`orb_params_t`); the ticker and the anims write it and call `lv_obj_invalidate(orb)`. |
| Pill, round buttons, rows, cards, dialog buttons | `lv_obj` + `lv_label`, with shared styles and `LV_OBJ_FLAG_CLICKABLE`. `lv_button` brings nothing extra once the theme is off. |
| Chat, lists | `lv_obj` with `LV_FLEX_FLOW_COLUMN`, `LV_DIR_VER`, `LV_SCROLLBAR_MODE_OFF`, plus a rim `lv_arc` indicator |
| Keyboard | `lv_keyboard` + `lv_textarea` (§4.5) |
| Adjuster, rim indicator, PWR ring | `lv_arc` (indicator-only ones are non-clickable) |
| QR | `lv_qrcode`: I1 draw buffer of size²/8 + 8 B palette, ECC M |
| Toggles | text "Вкл/Выкл" in the row's right slot; the whole 252×64 row is the target |
| Busy indicator | small `lv_arc` rotated by a 33 ms timer (no `lv_spinner`) |

### 7.4 Styles

All styles are static `lv_style_t` objects initialised once in `ui_theme_init()`. Objects add them with `lv_obj_add_style(obj, &st_x, selector)`. No per-object styles are used except the animated local properties (`opa`, `translate_y`, colours on the orb struct). Token colours come from §1.1/§1.2, and the accent styles are re-initialised on a mode change with `lv_obj_report_style_change(&st_accent)`.

Shared styles:
- `st_screen`: bg `#000`, `opa_cover`, pad 0, border 0, not scrollable.
- `st_pill`, `st_pill_pr`.
- `st_btn_round` / `_pr`.
- `st_row` / `_pr`.
- `st_title20`, `st_body20`, `st_caption16`, `st_hint16`.
- `st_bubble_user`, `st_reply`, `st_turn`, `st_card[status]`.
- `st_key` / `st_key_pr` / `st_key_dis` (`LV_PART_ITEMS`).
- `st_field`.
- `st_arc_main` / `st_arc_ind` / `st_arc_knob`.
- `st_stadium_primary` / `_secondary`.
- `st_fade_top` / `st_fade_bottom`.
- One `lv_style_transition_dsc_t trans_tap` (bg_color, 80 ms), referenced by every `_pr` style.

### 7.5 Subjects (observer) and the 9.6-safe wrapper

| Subject (int) | Values | Written by (via `ui_post`) | Observed by |
|---|---|---|---|
| `s_ui_state` | IDLE, REC, REC_LATCHED, REC_CANCEL, UPLOAD, THINK, SPEAK, ANSWERED, ERROR, OFFLINE | app FSM | orb, pill/status, ⚙ visibility, ticker on/off |
| `s_mode` | 0 assistant, 1 translator | UI (M) | orb colours, pill, chat model |
| `s_voice_out`, `s_speaker` | 0/1 | UI | 🔊 button, FSM |
| `s_fault` | NONE, NO_WIFI, BAD_KEY, NO_CREDIT, LOW_CREDIT, REGION, MODEL, MIC | net/app | pill, orb OFFLINE, H routing |
| `s_net`, `s_rssi` | OFF/CONNECTING/OK/FAIL; dBm | Wi-Fi task | W, D, fault logic |
| `s_upload_pct` | 0–100 | net task | orb progress, status |
| `s_volume`, `s_brightness` | 0–100 / 5–100 | UI | A, backlight observer → LEDC, audio (atomic copy) |
| `s_ui_lang` | 0 ru / 1 en | UI | i18n rebuild of H + the current screen |
| `s_lang_a`, `s_lang_b`, `s_tr_dir` | index; 0 auto / 1 A→B / 2 B→A | UI | pill, prompt builder |
| `s_battery`, `s_charging` | −1 USB, 0–100; 0/1 | power task | ambient, toasts |
| `s_portal` | OFF, AP_UP, PHONE_JOINED, DATA_RX, DONE, FAIL | portal task | Q |
| `s_clock_min` | 0–1439 | RTC/SNTP task, once a minute | Z |
| `s_hints` | 0–3 | app | orb label, hint strings |

**Bindings.** Use only `lv_subject_add_observer_obj()`, which exists in 9.5 and 9.6. Do **not** use `lv_obj_bind_flag_if_*` or `lv_obj_bind_state_if_*`: they are deprecated in 9.6. Mic and TTS levels are **not** subjects; they are `atomic_int` values read by the ticker.

```c
/* ui_subjects.h — the only place that knows the LVGL observer API version */
#include "lvgl.h"
typedef lv_subject_t *ui_subj_t;

ui_subj_t ui_subj_int(int32_t init, int32_t min, int32_t max);
void      ui_set(ui_subj_t s, int32_t v);              /* clamps; LVGL task only */
static inline int32_t ui_get(ui_subj_t s) { return lv_subject_get_int(s); }
static inline lv_observer_t *ui_watch(ui_subj_t s, lv_observer_cb_t cb, lv_obj_t *obj, void *ud) {
    return lv_subject_add_observer_obj(s, cb, obj, ud);   /* auto-removed when obj is deleted */
}

/* ui_subjects.c */
typedef struct { lv_subject_t *s; int32_t min, max; } ui_subj_meta_t;
ui_subj_t ui_subj_int(int32_t v, int32_t min, int32_t max) {
#if LV_VERSION_CHECK(9, 6, 0)
    lv_subject_t *s = lv_subject_create(LV_SUBJECT_TYPE_INT);   /* 9.6: LVGL owns it */
    lv_subject_set_int(s, v);
#else
    lv_subject_t *s = lv_malloc_zeroed(sizeof(lv_subject_t));   /* 9.5: caller-owned, heap */
    lv_subject_init_int(s, v);
#endif
    meta_register(s, min, max);       /* own clamping: identical behaviour on both versions */
    return s;
}
void ui_set(ui_subj_t s, int32_t v) { v = meta_clamp(s, v); if (v != ui_get(s)) lv_subject_set_int(s, v); }
```

Observer callbacks have the 9.5 signature `void cb(lv_observer_t *o, lv_subject_t *s)`. Get the target with `lv_observer_get_target_obj(o)` and the user data with `lv_observer_get_user_data(o)`.

### 7.6 Thread-safety rules

1. **Only the LVGL task touches `lv_*` objects, anims, timers and subjects.**
2. Other tasks call **`ui_post(&msg)`**: a FreeRTOS queue of 24 × `ui_msg_t`, sent with `xQueueSend(q, &m, 0)`. If the queue is full, non-critical messages are dropped and counted. Text payloads are `heap_caps_malloc(MALLOC_CAP_SPIRAM)` buffers whose ownership passes to the UI, which frees them. The queue is drained by `lv_timer_create(ui_pump_cb, 20, NULL)`, which calls `ui_set()` and the chat model functions.
3. High-rate values (mic RMS, TTS RMS, upload bytes) are `atomic_int` written by their producers and read by the ticker. No locks, no queue.
4. Use `lvgl_port_lock(50)` / `lvgl_port_unlock()` only for rare direct calls from other tasks: the dev-console snapshot and touch injection. Never hold it across I/O or other mutexes, never call it from an ISR, and never call it from the LVGL task.
5. LVGL event handlers never block. They post commands to the app FSM (`app_post(CMD_TALK_DOWN)`, …).
6. Physical buttons: a GPIO ISR (any edge, active low) sends to the input task. That task debounces (20 ms), measures the 400 ms tap/hold, detects the 3 s PWR hold, and calls `app_post()` and `ui_post()`.

```c
typedef struct { uint16_t type; int32_t a, b; char *text; } ui_msg_t;   /* text owned by UI after post */
static void ui_pump_cb(lv_timer_t *t) {
    ui_msg_t m;
    while (xQueueReceive(ui_q, &m, 0) == pdTRUE) { ui_dispatch(&m); if (m.text) heap_caps_free(m.text); }
}
```

### 7.7 Screen lifecycle and memory per screen

- **H is permanent.** Every other screen is built on entry with `ui_nav_push(builder)` and deleted on exit with `ui_nav_back()`, which runs `lv_screen_load(prev)` and then `lv_obj_delete(cur)`. The stack depth is at most 4.
- Deleting a screen removes its observers automatically (`ui_watch` is object-bound).
- Estimates below assume about 100 B per `lv_obj` including its style list, plus label text.

| Screen | Objects | LVGL heap (PSRAM) | Other |
|---|---|---|---|
| H (permanent) | 14 + ≤ 2 per message (≤ 54) | ≈ 8 KB + ≤ 20 messages × ≈ 0.9 KB ≈ **26 KB** | chat text models for both modes ≤ 40 KB PSRAM; cached last clip ≤ 1 MB PSRAM |
| M / L / S | ≈ 26 / ≈ 20 / ≈ 50 | ≈ 5 / 4 / 9 KB | – |
| W (≤ 20 networks) | ≈ 90 | ≈ 16 KB | scan records 20 × 80 B |
| AI / V / language list | ≤ 60 | ≤ 11 KB | `/models` JSON parse buffer ≤ 100 KB PSRAM, freed once the curated list is built |
| K | 8 + keyboard (≤ 31 buttons: areas 8 B + ctrl 2 B each) | ≈ 3 KB | textarea ≤ 512 B |
| A | 7 | ≈ 2 KB | – |
| Q | 9 + qrcode | ≈ 3 KB + I1 buffer (150²/8 + 8 ≈ 2.9 KB) + encoder temp ≈ 1 KB | portal HTTP server in its own task |
| D | ≈ 70 | ≈ 13 KB | – |
| C / O-step / Z / T_READ | 7 / 10 / 4 / 5 | ≤ 2 KB each | – |

**Fixed costs:**
- internal SRAM: draw buffers 2 × 28.8 KB (DMA) + LVGL task stack 8 KB;
- flash: fonts ≈ 145 KB, L2 key maps ≈ 3 KB;
- debug only: snapshot 259.2 KB PSRAM, transient.

Peak LVGL heap (H + the largest sub-screen + layers) stays below 80 KB. The custom PSRAM allocator has no fixed pool limit.

### 7.8 Test hooks and simulator

- **Dev console** (USB serial), per the requirements:

  | Command | What it does |
  |---|---|
  | `ui shot` | `lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565)` → PNG on the PC |
  | `ui tap x y [ms]` | drives a second, virtual `LV_INDEV_TYPE_POINTER` |
  | `ui btn boot\|pwr down\|up` | goes through the same input-task path as the real buttons |
  | `ui state <n>` | forces orb states for screenshots |
  | `ui fps on\|off` | toggles lv_sysmon |
  | `ui lang ru\|en` | switches the UI language |

- **Simulator:** `lv_port_pc_visual_studio` at 360×360. The UI code is hardware-free. A **simulator-only** overlay on `lv_layer_top()` draws black outside r = 180 (the bezel) and an optional r = 168 guide circle.
- **String-fit test (CI):** measure every i18n string with `lv_text_get_size()` against its slot width:

  | Slot | Width (px) |
  |---|---|
  | pill | 124 |
  | status line, empty state | 200 |
  | status line, conversation | 180 |
  | header | 118 |
  | row title | 164 |
  | row subtitle | 184 |
  | prompt label | 150 |
  | key labels | key width − 8 |

  The test fails the build if a Russian or English string overflows.

### 7.9 File layout

```
main/ui/
  ui.c/.h            init, nav stack, ui_post queue + pump timer, dim/ambient timers
  ui_theme.c/.h      colour tokens, shared styles, motion tokens
  ui_subjects.c/.h   subjects + 9.5/9.6 wrapper
  ui_orb.c/.h        custom-draw orb, presets, ticker, state timelines
  ui_list.c/.h       curved list (fade, rim indicator), row builders
  ui_kb.c/.h         keyboard layouts, action tables, handler, draw-task styling
  scr_home.c scr_mode.c scr_settings.c scr_picker.c scr_wifi.c scr_text.c
  scr_adjust.c scr_qr.c scr_provider.c scr_diag.c scr_dialog.c scr_onboard.c
  scr_ambient.c scr_reader.c
  i18n/strings.h strings_ru.c strings_en.c
  fonts/ui_16.c ui_20.c ui_28.c ui_48.c   (generated, committed)
```

---

## 8. To verify on the hardware (before freezing this spec)

1. **Real render throughput** of LVGL 9.5 without S3 SIMD at 360×360. Measure every row of §6.2 with lv_sysmon at 80 MHz and at 40 MHz QSPI, and apply the §6.1 fallbacks if a row drops below 30 fps.
2. **CST816T behaviour during a long stationary hold.** Does it keep reporting "pressed" (auto-sleep or report-mode registers), so that on-screen hold-to-talk works? Latched mode and BOOT are the fallbacks.
3. **Physical position of the BOOT and PWR keys** on the case. Hints say only "кнопка BOOT" until this is known.
4. **ST77916 partial-flush alignment** (whether the rounder is needed), and visible tearing during chat or list scrolling without TE.
5. **Inter glyph coverage** of every requested code point. Check the lv_font_conv warnings, especially U+0218–021B and U+20B8.
6. **QR scan reliability** at 5 px/module (version 3) on iOS and Android cameras from 10–20 cm, and whether captive-portal auto-open works with our DNS hijack.
7. **Streamed structured output.** Does the `transcript` field arrive early enough to show the user bubble during THINKING?
8. **Backlight PWM** at 15 % duty: no visible flicker (choose the LEDC frequency, e.g. ≥ 20 kHz).
