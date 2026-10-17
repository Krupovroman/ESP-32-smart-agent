# Orbi: round UI specification (concept: Expressive Character)

Target: Waveshare ESP32-S3-Touch-LCD-1.85 rev 2.1. Screen: 360×360 round IPS, ST77916 over QSPI (about 40 MB/s), CST816T single touch. GUI: LVGL 9.5 through esp_lvgl_port 2.9, PARTIAL render mode, two internal-SRAM DMA buffers of 360×40 px (28.8 KB each). 8 MB PSRAM. Physical buttons: BOOT (GPIO0) and PWR (GPIO6). UI languages: Russian (default) and English.

The decisions in `docs/requirements.md` and the corrections in `docs/research/stack_2026-10-17.json` (`verify:*`) apply. The ones that shape this document:
- no `clip_corner` on the root screen;
- no tileview and no full-screen fades;
- QSPI tearing mode is NONE, so there is no TE sync;
- 9.6 renames the subject API, so we use a wrapper;
- `lv_roller` INFINITE mode is not suitable for long lists;
- the font needs General Punctuation and №;
- all LVGL calls happen in the LVGL task.

---

## 0. The concept on one page

**Orbi** (RU: **Орби**) is a glowing sphere with a face. It does several jobs at once:
- it is the push-to-talk button;
- it shows the current state;
- it doubles as the progress bar;
- it is the first thing that shows an error.

It has moods. It looks at what you touch, it glances down at new messages, it nods while you talk, it looks up and to the side while it thinks, and it smiles while it speaks. Everything else on the screen is arranged around it:
- **satellites** (two small round buttons) orbit it;
- **rim text** runs along the bezel;
- lists curve with the circle;
- the keyboard opens **petals** under your finger.

**One rule for touching Orbi:** *touch = interrupt whatever it is doing, hold = talk, tap while idle = hands-free listening.*

**Performance contract** (section 7):
- no layers, no style transforms, no shadows, no Lottie, no screen fades, no tileview;
- continuous animations invalidate at most 30 k px per frame;
- one-shot transitions invalidate at most 70 k px per frame;
- a full-screen redraw happens only on the single frame after a screen load.

**Coordinate conventions:**
- Screen pixels, origin at top-left, centre **C = (180,180)**.
- Rectangles are written as edges, `x 58–118` = left edge 58 and right edge 118, so width 60. LVGL's inclusive `x2` is the right edge minus 1.
- Angles follow LVGL: 0° = 3 o'clock, increasing clockwise (90° = 6 o'clock, 270° = 12 o'clock).
- The **usable circle** has r ≤ 168 (12 px rim margin). The usable chord at vertical offset d from the centre is `w(d) = 2·√(168² − d²)`.

| d | 0 | 30 | 50 | 60 | 80 | 90 | 100 | 110 | 120 | 130 | 140 | 144 | 150 | 156 | 160 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| w(d) | 336 | 330.6 | 320.8 | 313.8 | 295.5 | 283.7 | 270.0 | 254.0 | 235.2 | 212.8 | 185.7 | 173.1 | 151.3 | 124.7 | 102.4 |

A rectangular row spanning y1–y2 fits when its width ≤ w(d_max), where d_max = max(|y1−180|, |y2−180|). A pill (end caps of radius r centred at P) fits when |P − C| + r ≤ 168. Touch targets are at least 50×50 px.

---

## 1. Visual language

### 1.1 Palette (RGB565 panel; hex values are the authoring values)

**Neutrals**

| Token | Hex | Use |
|---|---|---|
| `bg` | `#0B0C14` | every screen background (opaque) |
| `surface-1` | `#151826` | list rows, text field, cards |
| `surface-2` | `#1F2336` | keys, assistant bubble, satellites, chips |
| `surface-3` | `#2B3050` | pressed state, petal bubbles |
| `stroke` | `#343A5C` | arc tracks, dividers, inactive page dots |
| `text-1` | `#F3F4FA` | primary text (≈17:1 on `bg`) |
| `text-2` | `#A7ACC6` | secondary text (≈8.7:1) |
| `text-3` | `#6A7090` | disabled, timestamps (≈4:1) |
| `user-bubble` | `#2D2C51` | user chat bubble (violet 20 % over `surface-1`) |
| `eye` | `#F7F6FF` | Orbi's eyes and mouth |
| `cheek` | `#FF8FB0` | blush, drawn at 35–60 % opacity |

**Character states.** The body is a vertical 2-stop gradient (top → bottom). The halo rings are **pre-blended opaque colours**: the accent mixed with `bg` at 22 / 12 / 6 %. This gives a glow without any alpha.

| State | Body top | Body bottom | Accent | Halo 22 % | Halo 12 % | Halo 6 % |
|---|---|---|---|---|---|---|
| Idle / Assistant (Violet) | `#B3A6FF` | `#6A55F0` | `#8C7BFF` | `#272448` | `#1A1930` | `#131322` |
| Listening (Coral) | `#FFA3B8` | `#F0507A` | `#FF6F91` | `#412230` | `#281823` | `#1A121C` |
| Uploading / Thinking (Amber) | `#FFD58C` | `#F0982E` | `#FFB547` | `#41311F` | `#28201A` | `#1A1617` |
| Speaking (Aqua) | `#8DF3EA` | `#22B3BE` | `#3DDBD9` | `#163A3F` | `#11252C` | `#0E1820` |
| Error (Ember) | `#FF9A9A` | `#D9434B` | `#FF5A5F` | `#411D25` | `#28151D` | `#1A1119` |
| Offline / Doze (Slate) | `#9AA2C4` | `#4C5476` | `#6E7699` | `#212331` | `#171924` | `#11121C` |

Formula for any other accent: `halo_k = lv_color_mix(accent, bg, k)` with k = 56, 31, 15 (out of 255).

**Translator.** Language A is Sky `#5AA9FF` and language B is Tangerine `#FFA24C`. In the idle translator the body is a horizontal gradient `#8CC4FF → #FFBE80`.

**Semantic:** success `#46D69A`, warning `#FFB547`, danger `#FF5A5F`, info `#5AA9FF`.

**Primary pill:** fill = mode accent, text `#0B0C14` (≈5.9:1). **Danger pill:** fill `#FF5A5F`, text `#0B0C14`.

### 1.2 Typography

**Font:** **Nunito** (SIL OFL 1.1; Latin Ext, Cyrillic and Cyrillic Ext). Its rounded terminals match the character.
- The googlefonts/nunito repo ships only `fonts/variable/`. Take the **static** instances (`static/Nunito-SemiBold.ttf`, `-Bold`, `-ExtraBold`) from the Google Fonts family download.
- lv_font_conv's README does not say whether it supports variable fonts, so do not feed it a variable font.
- Fallback: Montserrat (OFL, static TTFs, Cyrillic).

All fonts are 4 bpp, `--no-compress` (no decompression CPU cost), kerning kept.

| Name | File / weight | px | Line space | Used for | Glyphs |
|---|---|---|---|---|---|
| `f_cap_14` | Nunito SemiBold | 14 | +2 | rim text (crown, hints, toasts, list labels), list values, timestamps, small flick glyphs, body text in error cards | TEXT set + 26 icons |
| `f_body_20` | Nunito SemiBold | 20 | +4 | chat text, list titles, buttons, text field, chips | TEXT set + 52 icons |
| `f_title_24` | Nunito Bold | 24 | +2 | key centre letters, petal bubbles, dialog/onboarding titles, language codes | TEXT set + 52 icons |
| `f_disp_40` | Nunito ExtraBold | 40 | 0 | dial values, doze clock | `0x20 0x25 0x2B-0x2E 0x30-0x3A 0xB0` (≈20 glyphs) |

**TEXT glyph set** (passed as separate `-r` options):
- `0x20-0x7E` ASCII
- `0xA0-0xFF` Latin-1: « » ° ± × ÷ § £ ¥ ¢
- `0x100-0x17F` Latin Ext-A (pl/cs/tr/hu/ro)
- `0x218-0x21B` ș ț
- `0x400-0x45F` Cyrillic incl. Ёё and uk/be/sr/bg/mk letters
- `0x490-0x491` Ґґ
- `0x2010-0x2027` dashes, curly quotes, •, …
- `0x2039-0x203A` ‹ ›
- `0x20AC` €, `0x20BD` ₽, `0x2116` №, `0x2122` ™

That is ≈451 glyphs. If Nunito lacks a glyph (₽ is the likely one), lv_font_conv reports it. Drop that glyph from the symbol keyboard.

**Example command** (run by the developer; nothing is installed by this spec):
```
lv_font_conv --bpp 4 --size 20 --format lvgl --no-compress --lv-font-name f_body_20 -o f_body_20.c \
  --font Nunito-SemiBold.ttf -r 0x20-0x7E -r 0xA0-0xFF -r 0x100-0x17F -r 0x218-0x21B \
     -r 0x400-0x45F -r 0x490-0x491 -r 0x2010-0x2027 -r 0x2039-0x203A -r 0x20AC,0x20BD,0x2116,0x2122 \
  --font FontAwesome5-Solid+Brands+Regular.woff -r <icon list for this size, §1.3>
```

**Flash budget.** Scaled from the measured Montserrat figures: ≈42 B/glyph at 16 px, ≈88 B at 24 px, plus 8 B per descriptor. Estimates:

| Font | Size |
|---|---|
| F14 | ≈19 KB |
| F20 | ≈36 KB |
| F24 | ≈47 KB |
| F40 | ≈5 KB |
| kerning | 20–40 KB |
| **Total** | **< 180 KB** |

**Text sanitiser** (applied to all model output before it reaches a label):
- U+00A0 → space; U+2028/2029 → `\n`;
- drop U+1F000–1FAFF, U+2600–27BF and U+FE0F (emoji);
- strip markdown `**`, `__`, `` ` `` and leading `#`; a leading `- ` becomes `• `;
- any other code point outside the set → `?`.

Keep `LV_USE_FONT_PLACEHOLDER=1` in development builds so missing glyphs show as boxes.

### 1.3 Icons

Approach:
- **FontAwesome 5 Free Solid**, merged into F14/F20/F24 from LVGL's `scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff`.
- **No bitmap images anywhere in the UI.** Orbi, halos, bars, badges and QR codes are drawn procedurally, so no image decoder or image cache is needed.
- Where LVGL's own `LV_SYMBOL_*` uses the same codepoint (OK, CLOSE, BACKSPACE, WIFI, VOLUME_MAX, KEYBOARD, BATTERY_*, CHARGE…), reuse the macro. Define the rest, for example `#define SYM_MIC "\xEF\x84\xB0"` (U+F130).

| Icon | Code | Sizes | Icon | Code | Sizes |
|---|---|---|---|---|---|
| check | F00C | 14/20/24 | times | F00D | 14/20/24 |
| power-off | F011 | 20/24 | cog | F013 | 20/24 |
| clock | F017 | 14/20 | signal | F012 | 14 |
| volume-up | F028 (`\xEF\x80\xA8`) | 14/20/24 | volume-down | F027 | 20/24 |
| volume-mute | F6A9 (`\xEF\x9A\xA9`) | 14/20/24 | microphone | F130 (`\xEF\x84\xB0`) | 14/20/24 |
| microphone-slash | F131 | 14/20/24 | keyboard | F11C (`\xEF\x84\x9C`) | 20/24 |
| language | F1AB | 20/24 | globe | F0AC | 14/20/24 |
| exchange-alt | F362 (`\xEF\x8D\xA2`) | 20/24 | wifi | F1EB (`\xEF\x87\xAB`) | 14/20/24 |
| lock | F023 | 14/20 | eye / eye-slash | F06E / F070 | 20 |
| key | F084 | 14/20/24 | qrcode | F029 | 20/24 |
| mobile-alt | F3CD | 20/24 | brain | F5DC | 20/24 |
| robot | F544 | 20/24 | sliders-h | F1DE | 20 |
| sun / moon | F185 / F186 | 20/24 | coins | F51E | 14/20/24 |
| info-circle | F05A | 20 | question-circle | F059 | 20 |
| exclamation-triangle | F071 | 14/20/24 | chevron-left/right | F053 / F054 | 14/20 |
| arrow-up (shift) | F062 (`\xEF\x81\xA2`) | 24 | backspace | F55A (`\xEF\x95\x9A`) | 24 |
| paper-plane (send) | F1D8 (`\xEF\x87\x98`) | 24 | play / stop | F04B / F04D | 20 |
| redo (retry) | F01E | 14/20 | history | F1DA | 20/24 |
| trash-alt | F2ED | 20 | battery full/half/empty | F240 / F242 / F244 | 14 |
| bolt / plug | F0E7 / F1E6 | 14 | server / link | F233 / F0C1 | 20 |
| bug / microchip / memory | F188 / F2DB / F538 | 20 | comments / comment-dots | F086 / F4AD | 20/24 |
| closed-captioning (text only) | F20A | 20/24 | plus / minus / ellipsis-h | F067 / F068 / F141 | 20/24 |

### 1.4 Shapes and spacing

- 4 px grid.
- Radii: pills = h/2 (26 for 52-px pills), keys 14, bubbles 18, cards 24, rows 30 (pill rows).
- Borders only 2–3 px, and only as an accent: focused text field, language chip, bubble side stripe.
- Elevation comes from fill steps (`surface-1/2/3`), never from shadows.

**Rim zones** (the same on every screen):

| Zone | Geometry | Content |
|---|---|---|
| **Crown** | top arc, text at r = 150–166, angles 236°–304° | status: mode · time · Wi-Fi/battery icons (F14). Never interactive on main screens. |
| **Header back zone** (sub-screens only) | invisible button x 90–270, y 6–56 | tap = Back. The title is an arclabel in the crown position, prefixed with `‹`. |
| **Bottom cap** | y ≥ 306 | page dots (y = 342) and the **Mode Ring** tap zone (invisible button x 120–240, y 306–356) |
| **Side rims** | x < 40 or x > 320 | no interactive content |

---

## 2. The character: Orbi

### 2.1 Anatomy

Orbi is drawn in one custom widget (`orbi`, §8.3). Parameters scale with body radius **R**. STAGE R = 88, CHAT R = 36, translator chip view uses the same values, onboarding R = 48 or 60, Mode Ring R = 40.

| Part | Geometry (relative to body centre) | Drawing |
|---|---|---|
| Halo | 3 concentric circles of radius R + round(0.09R), R + round(0.18R), R + round(0.27R) (STAGE: 96/104/112) | opaque pre-blended colours (§1.1), outermost first |
| Body | circle of radius R; squash ±10 % (w/h) → stadium shape | `lv_draw_rect`, radius `LV_RADIUS_CIRCLE`, `bg_grad_dir = LV_GRAD_DIR_VER` (HOR in the idle translator) |
| Highlight | pill 0.42R × 0.22R, centre (−0.38R, −0.55R) | white, opa 56 (22 %) |
| Eyes | two rounded rects, radius = w/2; default 0.25R × 0.5R, centres (±0.32R, −0.07R) + gaze | `eye` colour |
| Lids | top lid: body-coloured rect + triangle (tilt); bottom lid: body-coloured circle r = eye_w, centred below the eye → crescent "smiling eyes" | lid colour = body gradient colour at the lid's mid-y |
| Mouth | rounded rect 0.27R × (0.05–0.2R) at (0, +0.34R), or an arc (smile/frown) | `eye` colour |
| Cheeks | 2 circles of 0.16R at (±0.45R, +0.18R) | `cheek` colour, opa 90–150 |
| Badge | 30 px circle centred on the body edge at 45° (bottom-right), `bg` fill, 2 px accent ring, F14 icon | shown for errors / offline / no speaker |
| Aura bars | STAGE: 32 radial lines from r = R + 30 (118) to ≤ R + 58 (146), width 6, round caps. CHAT: 16 lines, r 50 → ≤ 62, width 4 | `lv_draw_line` in the accent colour |
| Thought dots | 3 circles (STAGE 10/8/6 px, CHAT 6/5/4 px) on r = R + 16, 18° apart | body-top colour |
| Progress arc | r = R + 12, width 6 (CHAT 4), rounded, start 270° | `lv_draw_arc`, accent |

### 2.2 Expression presets

Values are px at R = 88 and are scaled by R/88. The "gap" is the distance between eye centres. Lid cover goes from 0 to 1. A positive tilt means the inner corner is down (angry); a negative tilt means it is up (sad).

| Expression | Eye w×h | Gap | Eye y | Gaze (x,y) | Top lid | Bottom lid | Mouth | Cheeks | Used for |
|---|---|---|---|---|---|---|---|---|---|
| NEUTRAL | 22×44 | 56 | −6 | 0,0 | 0 | 0 | — | 0 | idle |
| ATTENTIVE | 26×48 (+10 %·level) | 58 | −8 | 0,−4 | 0 | 0 | — | 0 | listening |
| SENDING | 22×40 | 56 | −8 | 0,−8 | 0 | 0.2 | — | 0 | uploading |
| FOCUSED | 20×34 | 54 | −8 | +8,−10, saccade ±8 | 0.15 / 0° | 0 | — | 0 | thinking |
| SPEAK | 22×40 | 56 | −6 | 0,+4 | 0 | 0.45 | rect 24 × (4–18) | 0.35 | speaking (mood neutral) |
| JOY | 24×44 | 58 | −8 | 0,0 | 0 | 0.6 | smile arc w30 t4 | 0.5 | mood happy, success |
| PLAYFUL | L closed 22×4, R 22×44 | 56 | −6 | +4,0 | — | 0.3 | smile arc | 0.4 | mood playful (wink) |
| SURPRISED | 30×30 | 60 | −8 | 0,−2 | 0 | 0 | "o" 12×14 | 0 | mood surprised |
| SAD | 22×38 | 54 | −2 | 0,+6 | 0.35 / −14° | 0 | frown arc w22 | 0 | mood sad/sorry, credit/region errors |
| CONFUSED | L 22×44, R 22×28 | 56 | −6 | −6,0 | R only 0.35 / +10° | 0 | line 16×3 at x+6 | 0 | bad key, model gone |
| SHRUG | 22×40 | 56 | −6 | ±6 glance | 0.2 / −8° | 0 | line 18×3 | 0 | cancel, timeout |
| SLEEPY | 22×44 | 56 | −4 | 0,+4 | 0.6 | 0 | — | 0 | offline, doze |
| ASLEEP | 22×4 (closed) | 56 | 0 | 0,+6 | 1.0 | 0 | — | 0 | screen dim |
| LOVE | hearts 26 px (2 circles + 1 triangle each) | 58 | −6 | 0,0 | — | — | smile | 0.6 | mood love (rare) |

**LLM mood.** The assistant JSON schema is `{transcript, mood, reply}`, in that order, so the face can react before the reply text arrives. The `mood` enum is: neutral, happy, playful, thinking, sad, sorry, surprised, love. While speaking (or for 2 s in text-only mode) it maps to:

| mood | Expression |
|---|---|
| neutral | SPEAK |
| happy | JOY |
| playful | PLAYFUL |
| thinking | FOCUSED |
| sad / sorry | SAD |
| surprised | SURPRISED |
| love | LOVE |

Anything else falls back to SPEAK.

### 2.3 State → appearance

| Agent state | Body | Expression | Extras | Hint text RU / EN (STAGE, bottom rim) |
|---|---|---|---|---|
| IDLE | Violet (translator: A→B gradient) | NEUTRAL, blinks 2.5–6 s (20 % double blink), glance every 5–10 s | halo "breathing" (§7.2) | «Держите и говорите» / "Hold & talk" (translator: «Говорите: RU или EN») |
| LISTEN_PTT | Coral | ATTENTIVE | aura bars from mic level; record-time arc (r = R+7, width 3, 50 % white) | «Отпустите — отправлю» / "Release to send" |
| LISTEN_LATCH | Coral | ATTENTIVE | same | «Коснитесь — отправлю» / "Tap to send" |
| CANCEL_ZONE (finger slid away) | Slate | SAD-lite (lids 0.2) | bars grey; ✕ badge | «Отпустите — отменю» / "Release to cancel" |
| UPLOADING | Amber | SENDING | progress arc = bytes sent / total | «Отправляю…» / "Sending…" |
| THINKING | Amber | FOCUSED + saccades | 3 thought dots orbit | «Думаю…»; after 6 s «Ещё чуть-чуть…»; after 20 s «Долго… коснитесь, чтобы отменить» |
| SPEAKING | Aqua (translator: target-language colour) | mood mapping; mouth follows TTS level | aura bars (amplitude ≤ 14 px, slower) | «Коснитесь — замолчу» / "Tap to stop" |
| TEXT_REPLY (voice off) | Aqua for 2 s → Violet | mood mapping; gaze tracks the streaming text (x ±6 at 1.5 Hz) | — | — |
| ERROR | Ember, then the error-specific face (§4.15) | per error | badge icon | error card |
| OFFLINE | Slate | SLEEPY | wifi badge + retry countdown ring on the badge | «Нет сети» |
| DOZE (60 s idle) | Slate-dim; backlight 30 % | SLEEPY, breathing 6 s | time in F40 at y 276–324 | — |
| SLEEP (5 min) | backlight off | ASLEEP | rendering stops | — |

### 2.4 Micro-interactions

- **Gaze follows touch.** Any press on a satellite, chip, bubble or list row on the main screen calls `orbi_look_at(point)`. The gaze vector is the normalised (p − orb) × 8 px; it eases back after 900 ms.
- **Reading glance.** While the chat scrolls, gaze_y = ±6 in the scroll direction. When a new bubble appears, gaze_y = +8 for 1 s.
- **Squish on touch.** Body w +8 %, h −8 % for 90 ms, then release with ease-out-back (220 ms).
- **Shrug on cancel.** SHRUG face, body dx wobble ±4 px (300 ms).
- **"Shh" on voice off.** Eyes close happily and a line mouth appears for 300 ms. **Voice on:** "o" mouth and wide eyes for 300 ms.
- **Celebrate** (setup success, first Wi-Fi connection): hop (−18 px), landing squash, 10 sparkles (§7.4).
- **Translator turn.** The source-language chip pulses. Orbi tints to the target colour and looks toward the target chip, as if turning from one person to the other.
- **Boot.** Orbi grows from a dot with its eyes closed, opens them, glances left and right, then smiles. A «Привет!» toast follows.

---

## 3. Screen inventory and navigation

### 3.1 Inventory

| ID | Screen RU / EN | Type | Entry | Exit |
|---|---|---|---|---|
| S0 | Заставка / Splash | instant, ≈1.4 s | power-on | → O1 (first boot) or S2 |
| O1–O7 | Знакомство / Onboarding | template screens | first boot, «Сброс настроек» | → S2 |
| S2 | Главный: Ассистент / Assistant | main screen, mode = ASSIST, layouts STAGE / CHAT / ERRORCARD | root | swipe ← S4, ring |
| S4 | Главный: Переводчик / Translator | same main screen, mode = TRANSL | swipe → from S2 | swipe → S2, swipe ← S5 |
| S4a | Выбор языка / Language picker | orbit list | tap chip → «Сменить» | back |
| S5 | Настройки / Settings | orbit list (carousel page 3) | swipe ← from S4 | swipe → S4 |
| S6a | Wi-Fi | orbit list | S5 | back |
| S7 | Сети Wi-Fi / Wi-Fi scan | orbit list | S6a, O5b | → S8 / connecting |
| S7c | Подключение / Connecting | message template | S7 / S8 | → result |
| S8 | Ввод текста / Text entry | keyboard screen | satellites, S7, S9, S15, O5c | ✓ / ✕ / PWR |
| S9 | Модель ИИ / AI model | orbit list | S5 | back |
| S10 | Голос / Voice | orbit list (2 levels) | S5 | back |
| S12 | Громкость, Яркость / Volume, Brightness | dial template | S5 | «Готово» / swipe → / PWR |
| S11 | Настройка с телефона / Phone setup | QR screen | S5, S6a, O4/O5c | back / done |
| S15 | Ключи и провайдеры / Keys & providers | orbit list + editors | S5, error cards | back |
| S3 | История / History | orbit list | Mode Ring | back |
| S3b | Разговор / Session view | read-only chat | S3 | back |
| S16 | Диагностика / Diagnostics & About | orbit list + test screens | S5 | back |
| S17 | Ошибки / Error states | ERRORCARD layout of S2/S4, plus toasts | automatic | action / back |
| S18 | Кольцо режимов / Mode Ring | radial menu screen | PWR short at root, tap bottom cap | item / outside / PWR |
| S18p | Питание / Power | 3 pills | ring item, PWR hold 1.5 s | back |
| S19 | Подтверждение / Confirm | modal screen | destructive actions | button / back |
| S20 | Сон / Doze & Sleep | state of S2/S4 | inactivity | any touch, BOOT, PWR |

### 3.2 Navigation map

```
 S0 ──first boot──> O1 Hello ─> O2 Mic check ─> O3 Speaker? ─> O4 Internet ─┬─(phone)──> S11 ───────────┐
  │                                                                          └─(device)─> S7 ─> S8 ─> S7c ─> O5c Key ─┬─(phone) S11
  │                                                                                                                    └─(type) S8
  │                                                                         O6 Check ─> O7 Tips ─> S2 <───────────────┘
  └──────────────────────────────────────────────────────────────────────────────────────────────> S2

  CAROUSEL (horizontal swipe, page dots):   [S2 Ассистент] ⇄ [S4 Переводчик] ⇄ [S5 Настройки]
     S2 satellites:  🔊/🔇 toggle voice · ⌨ → S8 (chat message)
     S4 chips:       tap → popover {🌐 → S4a, ⌨ → S8 (text in that language)}
     S5 rows:        Wi-Fi → S6a → S7 → S8 · Модель → S9 (→ S8) · Голос → S10 · Громкость/Яркость → S12
                     Переводчик → S4a×2 + toggles · С телефона → S11 · Ключи → S15 (→ S8 / S11)
                     История → S3 · Язык интерфейса → choice · Экран и сон → list · Диагностика → S16
  MODE RING S18 (PWR short at root / tap bottom cap):  Ассистент · Переводчик · История(S3) · Настройки · Сон · Питание(S18p)
  BACK (every sub-screen): tap header zone · swipe right · PWR short.   PWR hold 1.5 s → rim progress → S18p
  BOOT anywhere: hold = talk (jumps to the last talking mode first; in S8 = dictation)
```

### 3.3 Global input model and why the inputs do not conflict

| Input | Where | Action | Why it does not collide |
|---|---|---|---|
| **Press & hold / tap Orbi** | the orb only | talk (§6.1) | The orb is never inside a scrollable container. `LV_OBJ_FLAG_GESTURE_BUBBLE` is cleared on it, so a slide on the orb never becomes a page swipe. `PRESS_LOCK` (set by default on children) keeps it pressed while the finger slides out for slide-to-cancel. The hit test is circular (§8.3). |
| **Vertical drag** | chat viewport, orbit lists, history | scroll | These containers use `lv_obj_set_scroll_dir(…, LV_DIR_VER)`. LVGL stops gesture detection once scrolling starts (scroll limit 10 px). |
| **Horizontal swipe** (`LV_EVENT_GESTURE`; `lv_indev_set_gesture_min_distance(indev, 50)`, `…min_velocity(indev, 3)`) | main screens: switch page. Sub-screens: right = Back. | carousel / back | No container scrolls horizontally, so a horizontal drag never starts a scroll and becomes a gesture that bubbles to the screen. Excluded: orb, keyboard, dial arcs (all clear `GESTURE_BUBBLE`). After acting, the handler calls `lv_indev_wait_release(lv_indev_active())`, so the row under the finger gets no `CLICKED`. |
| **Tap bottom cap** | x 120–240, y 306–356 | open Mode Ring | The chat viewport ends at y = 300. Lists fade to opacity < 35 % below y = 300. The zone is disabled while an error card is shown. |
| **Tap header zone** | sub-screens, x 90–270, y 6–56 | Back | Not present on main screens or on S8, where the text field sits at y 44. |
| **BOOT** | global | hold = PTT, click = hands-free toggle / stop speaking | hardware; no double-click (it would collide with click-start/click-stop) |
| **PWR short / hold** | global | Back (sub-screens), Mode Ring (root) / power menu after 1.5 s hold | hardware |

A tileview was rejected: it needs a full-width horizontal scroll that redraws the whole screen every frame, and it fights the vertical chat scroll, the curved lists and hold-to-talk. Page changes are discrete; local elements animate instead (§7.3 T8).

---

## 4. Screen geometry

### 4.1 Main screen frame (S2 Assistant and S4 Translator share one screen object)

| Object | LVGL type | STAGE | CHAT | ERRORCARD |
|---|---|---|---|---|
| Crown | `lv_arclabel`, F14, radius 158, start 236°, size 68°, centred, `OVERFLOW_ELLIPSIS`, recolor on; object 236×48 at (62,6) | «Ассистент · 14:32» + icons; hidden while listening/speaking | same | visible |
| Orbi | `orbi` widget; object = body bbox; ext draw 64 | centre (180,164), R 88; body 92–268 × 76–252; halo to 68–292 × 52–276 | centre (180,76), R 36; body 144–216 × 40–112; halo r 46 → 134–226 × 30–122 | as CHAT, plus badge |
| Satellite L | `lv_button` 52×52, radius circle, `surface-2`, F24 icon | centre (87,273) = r 132 @135° | centre (82,98) = r 128 @220° | hidden |
| Satellite R | same | centre (273,273) @45° | centre (278,98) @320° | hidden |
| Hint | `lv_arclabel` F14 `text-2`, radius 144, start 62°, size 56° | visible | hidden | hidden |
| Chat viewport | `lv_obj`, scroll VER, scrollbar off, flex column, pad 12/14/10/14, row gap 8 | hidden | x 40–320, y 128–300 | hidden |
| Fades | `lv_obj` with VER gradient `bg` (opa 255 → 0) | — | top y 128–152, bottom y 280–300 (bottom fade reversed) | — |
| Page dots | custom 64×16 object at (148,334): dots 8 px at x 162/180/198, y 342; active = 20×8 pill, `text-1`; others `stroke` | visible | visible | hidden |
| Ring zone | transparent button x 120–240, y 306–356 | on | on | off |
| Toast | custom (§4.16) | on demand | on demand | on demand |

**Checks.**
- STAGE satellites: |(87,273) − C| = 131.5, +26 = 157.5 ≤ 168. Each spans ±11.4° around its angle, so the hint span (62°–118°) is clear.
- CHAT satellites: 127.8 + 26 = 153.8 ≤ 168. Gap to the head halo is 100.4 − 46 − 26 = 28 px.
- STAGE halo top: |(180,52) − C| = 128, below the crown text's inner radius of 150.
- STAGE bars reach r 146 from (180,164). The top point is at screen distance 162, so the crown hides during listening. The 135° bar ends at (77,267), over satellite L, so the satellites shrink away during listening.

**Bubble column** (CHAT): x 54–306 (max 252 px). It is fully inside the circle for 70 ≤ y ≤ 290 (w(110) = 254). `pad_bottom` = 10, so the last bubble's bottom stops at y ≤ 290.

**Satellite content:**
- Assistant, left: voice replies. 🔊 `volume-up` in `text-1` when on; `volume-mute` in `text-3` when off; plus a 10 px ember dot when no speaker is configured.
- Assistant, right: ⌨ keyboard.
- Translator: the two satellites **are the language chips**, showing the code ("RU" / "EN", F24) in the language colour with a 3 px border in that colour.

### 4.2 Assistant

- **Layout rules.**
  - The first SEND from STAGE switches to CHAT (T6).
  - CHAT stays CHAT for every later turn. Listening, thinking and speaking show around the 72 px head (16 compact bars), so the layout never changes in the middle of a turn.
  - 45 s with no touch and no audio → back to STAGE (T7). The messages stay in memory.
  - A gap of more than 10 min before the next turn starts a new session: the context resets and a divider «Новый разговор · 15:10» is inserted.
- **Assistant bubble.**
  - Left-aligned at x = 54, `max_width` 252, `LV_SIZE_CONTENT`.
  - Padding 10 / 14, radius 18, fill `surface-2`.
  - `border_side = LV_BORDER_SIDE_LEFT`, 3 px, colour = accent of the current mood.
  - Label F20 `text-1`, `LV_LABEL_LONG_MODE_WRAP`, line space 4.
- **User bubble.** Right edge at x = 306, `max_width` 220, fill `user-bubble`, radius 18, F20.
- **Waveform placeholder.** Before the transcript arrives, the user bubble (220×44) shows the recorded RMS envelope as 32 bars of 3 px. It is drawn once in the bubble's own draw event.
- **System line.** Centred, F14 `text-3`, no fill: timestamps, «Нет сети — повторю, когда появится».
- At most **30 bubbles** live on the main screen; older ones are only in History.

### 4.3 Translator (S4) and language picker (S4a)

- The main frame is identical to S2. The chips take the satellite positions.
- **Translator bubble.**
  - Language A bubbles align left with a 3 px left stripe `#5AA9FF`. Language B bubbles align right with a right stripe `#FFA24C`. Width ≤ 252.
  - Line 1: «RU · » in the language colour, then the original text, F14 `text-2`.
  - Line 2+: the translation, F20 `text-1`.
  - The bubble appears centred and grey (waveform) and slides to its side (translate_x ±40 px, 220 ms) as soon as `src_lang` is parsed.
- **Chip popover.** Tap a chip → two 52 px bubbles on layer_top at r 128:
  - STAGE left chip at 135°: bubbles at 107° and 163°, i.e. (143,302) and (58,217). Right chip mirrored.
  - CHAT left chip at 220°: bubbles at 192° (55,153) and 164° (57,215). Right chip mirrored.
  - Bubble contents: 🌐 → S4a for that slot; ⌨ → S8 in that language. Typing fixes the direction, so no detection is needed.
  - Tap outside to close.
- **Bubble tap → actions popover:** «Повторить озвучку» and «Не тот язык ⇄». The second re-runs the stored audio with the direction forced.
- **S4a language picker** (orbit list, §4.5).
  - Header «‹ Язык A» in the slot colour.
  - Rows: native name (F20) and «RU · Russian» (F14).
  - The other slot's language is disabled (`text-3`). Current choice shows a ✓.
  - A speaker-slash accessory means no TTS voice exists for that language: text only on that side.
  - Language list (renderable with the TEXT set): ru, en, uk, be, de, fr, es, it, pt, pl, cs, nl, sv, fi, tr, ro, hu, sr, bg.

### 4.4 History (S3) and session view (S3b)

- **S3** (orbit list).
  - Header «‹ История».
  - Row 0: «+ Новый разговор», accent title; clears the context and opens S2.
  - Then one row per session (newest first, at most 20): «Сегодня, 14:32» (F20) and the first user message (F14, `LV_LABEL_LONG_MODE_DOTS`).
  - Last row: «Очистить всё», danger title → S19.
- **S3b.**
  - Header «‹ Сегодня, 14:32».
  - Viewport x 40–320, y 56–320, fades at y 56–80 and 296–320.
  - Same bubble styles, read-only.
  - The last item is a 220×52 primary pill «Продолжить разговор», which loads the session as context and opens S2.

### 4.5 Orbit list (curved list component; used by S3, S4a, S5, S6a, S7, S9, S10, S15, S16)

- **Container.**
  - x 54–306, y 40–320 (252×280; one scroll frame ≤ 70.5 k px).
  - `scroll_dir` VER, `scrollbar_mode` OFF, `scroll_snap_y` = `LV_SCROLL_SNAP_CENTER`.
  - Flex column, centred, pad_top and pad_bottom 110 (so the first and last rows can sit at y = 180), row gap 6 → pitch 66.
- **Row.** 252×60 pill, radius 30, `surface-1`; pressed → `surface-3` (80 ms style transition).
  - Icon disc 40 px (accent at 18 % over `surface-1`, F20 icon in the accent) at row x 10–50.
  - Title F20 `text-1` at x 60; value F14 `text-2` underneath; both with `LONG_MODE_DOTS` and width 150.
  - Accessory at x 214–244: chevron, ✓, toggle (36×20 pill) or ▶.
  - Info rows (not tappable) have no fill.
- **Fade.** In `LV_EVENT_SCROLL`, each row's opacity is a function of its centre offset d:
  - 255 for |d| ≤ 66 (the row's edge is then inside w(96) = 276);
  - linear down to 0 at |d| = 150.
  - Only write the style when it changes by ≥ 8.
- **Scroll indicator.** Custom arc on the right rim: r 164, width 4, track 330°–30° in `stroke`, thumb in `text-2`. Length = visible / total; it invalidates only its ≈30×170 box.
- Rows are created when the screen is built (there is no virtualisation; every list here has ≤ 40 rows).

### 4.6 Settings (S5) rows

| # | Icon | Title RU / EN | Value example | Opens |
|---|---|---|---|---|
| 1 | wifi | Wi-Fi | «HomeNet» / red «Не подключено» | S6a |
| 2 | brain | Модель ИИ / AI model | «Gemini 3.1 Flash-Lite» | S9 |
| 3 | comment-dots | Голос / Voice | «Маша · MAI-Voice» | S10 |
| 4 | closed-captioning | Голосовые ответы / Voice replies | toggle | toggle (same subject as the satellite) |
| 5 | volume-up | Громкость / Volume | «70 %» | S12 |
| 6 | sun | Яркость / Brightness | «80 %» | S12 |
| 7 | exchange-alt | Переводчик / Translator | «RU ⇄ EN» | list: Язык A, Язык B, Озвучивать перевод (toggle), Авто-продолжение (toggle, default off) |
| 8 | qrcode | Настройка с телефона / Phone setup | «QR-код» | S11 |
| 9 | key | Ключи и провайдеры / Keys & providers | «OpenRouter · ключ ✓» | S15 |
| 10 | history | История / History | «12 разговоров» | S3 |
| 11 | language | Язык интерфейса / UI language | «Русский» | choice list RU / EN |
| 12 | moon | Экран и сон / Display & sleep | «Затемнение через 1 мин» | list: dim after 30 s / 1 / 2 min, off after 2 / 5 / 10 min, UI sounds toggle |
| 13 | bug | Диагностика / Diagnostics | «v0.4.2 · слот A» | S16 |

### 4.7 Wi-Fi (S6a, S7, S7c)

- **S6a rows** (orbit list):
  - info «Сеть: HomeNet ✓», «Сигнал −58 дБм», «IP 192.168.1.42»;
  - «Другие сети…» → S7;
  - «С телефона (QR)» → S11;
  - «Забыть сеть» (danger) → S19.
- **S7.**
  - While scanning: Orbi R = 24 at C in FOCUSED with thought dots, and «Ищу сети…» F14 at y 214–232.
  - Rows: SSID (F20) and «Защищена · −58 дБм» (F14).
  - Accessory: wifi glyph coloured by RSSI (≥ −60 `#46D69A`, ≥ −72 `#FFB547`, else `#FF5A5F`), plus a lock for secured networks.
  - The connected network shows a ✓.
  - First row «Обновить»; last rows «Скрытая сеть…» (S8 SSID → S8 password) and «С телефона (QR)».
- **S7c** (message template, §4.14):
  - Orbi R 60 at (180,120) in FOCUSED with thought dots; title «Подключаюсь к «HomeNet»».
  - Success → celebrate, «Готово!», auto-back after 1.2 s.
  - Failure → ERROR face and title «Неверный пароль» (`WIFI_REASON_AUTH_FAIL` / handshake timeout) with pill «Ввести снова», or «Сеть не найдена» with pill «Обновить».

### 4.8 Text entry (S8)

See §5 for the full keyboard. Screen objects:
- crown-position rim hint (`lv_arclabel` F14 `text-2`, r 158, 236°–304°, non-interactive);
- text field pill x 65–295, y 44–96;
- keyboard x 30–330, y 100–324;
- petal overlay on `lv_layer_top()`.

No header back zone and no swipe navigation on this screen.

### 4.9 AI model picker (S9)

- Header «‹ Модель ИИ».
- Rows 0–2, curated:
  - «Gemini 3.1 Flash-Lite» with «$0.25 / $1.50 · аудио $0.50 за 1M»;
  - «Gemini 3.5 Flash-Lite» with «$0.30 / $2.50 за 1M».
- Then «Загрузить из OpenRouter…»: mini thinking orb, then up to 40 rows sorted by price. The query is `GET /api/v1/models?input_modalities=audio&output_modalities=text&sort=pricing-low-to-high`. Rows are dropped when:
  - `supported_parameters` lacks either `response_format` or `structured_outputs`;
  - the id ends in `:batch` or `-preview`, or starts with `~`;
  - any price is negative;
  - `expiration_date` is non-null.
- Last row «Свой ID модели…» → S8 (prefill `google/`) or S11.
- The current model shows a ✓; tapping a row makes it current (✓ pop, 160 ms).

### 4.10 TTS voice picker (S10)

- **Level 1 rows:**
  - «Модель озвучки — MAI-Voice 2.1 Flash» (list from `GET /api/v1/models?output_modalities=speech`);
  - «Голос ассистента — Маша»;
  - «Голоса переводчика» → one row per language.
- **Level 2:** voices from `supported_voices`, filtered by locale for MAI (e.g. `ru-RU-*:MAI-Voice-2.1-Flash`). Display names come from a map (Masha → Маша, Lev → Лев, Grant → Грант, Harper → Харпер); otherwise the raw name.
- **Preview.** Accessory ▶ is a 44 px circle at row x 204–248 with `lv_obj_set_ext_click_area(…, 8)`, giving a hit area ≥ 60×60.
  - It plays «Привет! Я буду говорить вот так.» and caches the audio in PSRAM.
  - While playing it shows ■ and a 3-bar equaliser (bars 4 × 4–16 px).

### 4.11 Volume / Brightness dial (S12)

| Element | Geometry |
|---|---|
| `lv_arc` | 300×300 at (30,30) (radius 150); width 24 (r 126–150); bg angles 135° → 45° (gap at the bottom); range 0–100 (brightness 5–100); `lv_arc_set_change_rate(arc, 540)`; `LV_OBJ_FLAG_ADV_HITTEST` (only the ring is pressable); `GESTURE_BUBBLE` cleared |
| Knob | 40 px (pad 8) on the centreline r 138; outer edge at r 158 |
| Icon | F24 volume-up / sun, centred, y 96–124 |
| Value | F40 «70» at y 132–180, plus «%» in F20 |
| Label | F20 «Громкость» / «Яркость», y 186–212 |
| [−] [+] | 56 px circles centred at (126,250) and (234,250) → max r 116.4, inside the inner radius 126 |
| «Готово» | pill 104×50, x 128–232, y 284–334 (w(154) = 134) |

- Binding: `lv_arc_bind_value(arc, ui_s_volume)`.
- Brightness changes the LEDC duty live.
- Volume plays a tick at every 10 % step if a speaker is present.
- Settings are saved to NVS 1.5 s after the last change, and only in IDLE.

### 4.12 Phone setup (S11)

- The device starts a SoftAP `Orbi-XXXX` (last two MAC bytes) with a random 8-digit WPA2 password and a captive portal.
- Step 1 QR `WIFI:T:WPA;S:Orbi-7568;P:48151623;;` joins the network from the phone camera.
- On `WIFI_EVENT_AP_STACONNECTED` the QR crossfades (200 ms, card area only) to step 2: `http://192.168.4.1/?k=<6-char token>`.

| Element | Geometry |
|---|---|
| Header | «‹ Настройка с телефона» (crown position) + back zone |
| Card | 210×210 at (75,47), radius 24, fill `#F3F4FA`. Corner caps: \|(99,71) − C\| = 135.8, +24 = 159.8 |
| `lv_qrcode` | `lv_qrcode_set_size(qr, 170)` at (95,67); `lv_qrcode_set_quiet_zone(qr, false)` (the card margin is the quiet zone: about 6 modules); dark `#0B0C14`, light `#F3F4FA`. A version-3 code (29 modules) → integer scale 5 → 145 px code, about 0.65 mm modules |
| Caption 1 | F20 «1 Наведите камеру телефона» / «2 Откройте страницу», y 264–290 |
| Caption 2 | F14 «Сеть: Orbi-7568», y 294–312 |
| Caption 3 | F14 «Пароль: 48151623» / «192.168.4.1», y 314–332 |

- The QR buffer is 1 bpp (`LV_COLOR_FORMAT_I1`): about 22 B × 170 rows ≈ 3.7 KB.
- When the form arrives: the card shows a ✓ (F24 in a 64 px mint circle) and the lines «Wi-Fi ✓ · Ключ ✓».
- Then STA connects → celebrate → AP stops → back (or onboarding O6).
- Portal fields: Wi-Fi (the device's scan list plus manual SSID), password, OpenRouter key (paste; never echoed back, shown only as «сохранён ••••a1b2»), chat model, TTS voice, advanced per-endpoint base URL / key / model, and UI language. esp-wifi-connect's page must be extended for these fields.

### 4.13 Diagnostics and About (S16)

**Rows** (info unless marked):
- «Прошивка 0.4.2 · слот A · OTA ✓»
- «IDF 6.1 · LVGL 9.5.x»
- «IP 192.168.1.42 · −58 дБм»
- «SRAM своб. 84 КБ · PSRAM 6.1 МБ»
- «FPS 45 · CPU 23 %» (`LV_USE_SYSMON` in development builds)
- «Ключ OpenRouter ✓ · лимит $4.20». This comes from `GET /api/v1/key` `limit_remaining`; when it is null the row shows «лимит не задан — задайте лимит ключа». The account balance is not shown, because it needs a management key, which never goes on the device.
- «Тест микрофона» → mic test screen
- «Тест динамика»: plays a chime, then asks «Слышно?» [Да] [Нет]
- «Откат прошивки» (only if the other slot is valid) → S19
- «Перезагрузка»
- «Сброс настроек» (danger, hold to confirm)
- «Лицензии»: xiaozhi-esp32 MIT notice, LVGL MIT, Nunito OFL, FontAwesome Free (OFL font / CC BY 4.0 icons)

**Mic test screen:**
- Orbi R 70 at (180,160) in ATTENTIVE, 32 bars from r 84 to ≤ 112;
- value «−32 дБFS» F24 at y 282–312;
- hint arclabel «Коснитесь шара — сменить канал» at r 144, angles 50°–130°;
- tapping the orb toggles the I2S slot RIGHT ⇄ LEFT, with a toast.

### 4.14 Onboarding (O1–O7)

**"Choice" template:**
- Orbi R 48 at (180,104) (halo to y 43–165);
- title F24 centred at y 170–200;
- pill A 240×52, x 60–300, y 208–260 (w(80) = 295.5);
- pill B 188×52, x 86–274, y 266–318 (w(138) = 191.6);
- step dots at y 342.

**"Message" template:**
- Orbi R 60 at (180,120) (halo to y 44–196);
- title F24 at y 202–232;
- body F14 2 lines, max width 260, y 236–272;
- primary pill 156×52, x 102–258, y 276–328 (w(148) = 159).

| Step | Template | Content RU (EN in the string table) | Behaviour |
|---|---|---|---|
| O1 | choice | «Привет! · Hello!» · [Русский] [English] | Orbi wakes up; the pick sets the UI language |
| O2 | message | «Скажите «Привет»» / «Удерживайте шар — я слушаю» · [Дальше] | the orb is a live PTT (local level meter, no network); speech for 300 ms → JOY, «Слышу вас!», pill enabled; nothing in 8 s → pill «Пропустить» and a hint to run the mic test |
| O3 | choice | «Слышите звук?» · [Да, слышу] [Нет — только текст] | chime on entry; sets `speaker` and `voice_out` |
| O4 | choice | «Подключим интернет» · [📱 С телефона] (badge «быстрее») [Выбрать сеть] | → S11 or S7 |
| O5c | choice | «Ключ OpenRouter» · [📱 С телефона] [⌨ Ввести здесь] | skipped if the phone already sent a key |
| O6 | message | «Проверяю ключ…» → «Всё готово!» / «Модель: Gemini 3.1 Flash-Lite» · [Начать] | checks `GET /api/v1/key` and the model in `/models`; 401/402/403 → error card (§4.15) with a way back to O5c / S15 |
| O7 | message ×3 | «Свайп ← → — режимы», «Нижний край или PWR — меню», «BOOT — тоже кнопка разговора» · [Дальше] | → S2 |

### 4.15 Error states (ERRORCARD layout of the main screen)

Orbi moves to the head position (180,76) R 36 with the error face and badge. Satellites, hint, dots and the ring zone are hidden or disabled.
- Title F24, centred, y 128–158 (w(52) = 320).
- Body F14, ≤ 3 lines, width 260, y 162–216.
- Primary pill 220×52, x 70–290, y 222–274 (w(94) = 278).
- Secondary pill 148×50, x 106–254, y 280–330 (w(150) = 151.3).

Back (PWR or swipe right) dismisses the card and returns to IDLE. The last recording stays in PSRAM until a send succeeds, so «Отправить снова» never asks the user to speak again.

| Error | Trigger | Face / badge | Title / body (RU) | Primary | Secondary | Automatic |
|---|---|---|---|---|---|---|
| Нет Wi-Fi | STA lost, no IP | SLEEPY / wifi + ✕ | «Нет Wi-Fi» / «Не могу подключиться к «%s».» | Повторить | Сети Wi-Fi | retry after 5/10/30/60 s with a countdown ring on the badge; if the user spoke while offline: «Отправлю, когда появится сеть» (auto-send within 2 min) |
| Нет интернета | IP ok, DNS/TLS fails | SLEEPY / globe | «Нет интернета» / «Wi-Fi есть, но сервер не отвечает.» | Повторить | Диагностика | backoff as above |
| Плохой ключ | HTTP 401 | CONFUSED / key | «Ключ не подошёл» / «OpenRouter отклонил ключ (401).» | С телефона | Клавиатура | — |
| Нет средств | 402 | SAD / coins | «Закончились средства» / «На ключе нет кредитов (402). Пополните баланс или смените модель.» | Повторить | Модель | — |
| Регион | 403 | SAD / globe | «Недоступно в регионе» / «Провайдер отклонил запрос (403). Выберите другого.» | Провайдеры (S15) | Подробнее | — |
| Лимит запросов | 429 | SURPRISED / clock | «Слишком часто» / «Повторю через %d с.» | Повторить сейчас (disabled until the countdown ends) | — | auto-retry |
| Сервер / таймаут | 5xx, 524/529, timeout, `finish_reason: error` | SHRUG / redo | «Сервер не ответил» / «Запись сохранена — можно отправить ещё раз.» | Отправить снова | Отмена | — |
| Модель недоступна | 404 / model not found / expired | CONFUSED / brain | «Модель недоступна» / ««%s» больше не работает.» | Выбрать модель | — | — |
| Микрофон молчит | RMS ≈ 0 for the whole clip | ATTENTIVE squint / mic-slash | «Не слышу микрофон» / «Запись пустая.» | Тест микрофона | Отмена | — |
| Слишком коротко | < 400 ms of speech | — | toast «Держите дольше» | — | — | — |
| TTS failed | `/audio/speech` error | — | toast «Голос недоступен — показываю текст»; warning dot on the voice satellite | — | — | text continues |
| No speaker | `speaker = 0` | — | voice satellite shows mute + ember dot; once per boot, toast «Динамика нет — отвечаю текстом». Tap the satellite → toast with action «Подключили? Проверить» (chime test) | — | — | TTS is never called (no cost) |

### 4.16 Mode Ring (S18), Power (S18p), Confirm (S19), Toast, Doze

- **S18 Mode Ring** (opaque screen, never an overlay).
  - Orbi R 40 at C; its gaze follows the finger.
  - 6 items: 64 px circles, `surface-2`, F24 icon, on r 112 at:

    | Angle | Centre | Item |
    |---|---|---|
    | 270° | (180,68) | Ассистент |
    | 330° | (277,124) | Переводчик |
    | 30° | (277,236) | История |
    | 90° | (180,292) | Настройки |
    | 150° | (83,236) | Сон |
    | 210° | (83,124) | Питание |

  - Neighbouring items are 48 px apart; outer edge r 144.
  - Labels: `lv_arclabel` F14, r 158, centred on each item's angle, 40° span.
  - The current mode has a 3 px accent ring.
  - Tap outside the items, tap Orbi, or press PWR → close.
- **PWR hold.**
  - A custom rim arc at r 164, width 6, ember, sweeps 0 → 360° over 1500 ms; only the tip is invalidated.
  - Release early → nothing happens.
  - At 100 % → S18p.
- **S18p Power.**
  - Pills 220×52 at y 116–168 «Сон», 176–228 «Перезагрузка», 236–288 «Выключить» (w(108) = 257).
  - «Выключить» is enabled only on battery (drops the GPIO7 latch); otherwise it is disabled with «только от батареи».
  - Both destructive pills are **hold-to-confirm**: hold 1.2 s while an ember fill grows left → right inside the pill.
- **S19 Confirm.**
  - Opaque screen.
  - Icon disc 64 px at (180,100); title F24 y 144–174; body F14 y 180–216.
  - [Отмена] pill 120×52 at x 52–172 and [Да/Удалить] pill 120×52 at x 188–308, both y 230–282 (half-chord 138.3 at d = 102). Destructive → hold-to-confirm.
- **Toast.**
  - A custom object 250×60 at (55,290) draws a track arc centred at C (r 150, width 26 → r 137–163, `surface-2`, rounded) from 90° ± span/2. A child `lv_arclabel` (F14, r 150) carries the text.
  - Span animates 0 → 70° (200 ms) → hold 2.5 s → reverse.
  - Hint and dots hide while a toast is shown. Tapping it dismisses it.
- **Doze / Sleep.**
  - After 60 s idle: SLEEPY face, backlight to 30 % (LEDC hardware fade), and the time «14:32» in F40 at y 276–324.
  - After 5 min: lids close (ASLEEP), backlight off, LVGL stops refreshing, and the mic pre-roll stops.
  - A touch, BOOT or PWR wakes it. A BOOT hold wakes and records at once.

---

## 5. The keyboard

### 5.1 Choice: phone-keypad flick petals

The layout is the 3×3 Russian keypad familiar from feature phones (2 АБВГ, 3 ДЕЖЗ, 4 ИЙКЛ, 5 МНОП, 6 РСТУ, 7 ФХЦЧ, 8 ШЩЪЫ, 9 ЬЭЮЯ, 1 punctuation). Each key is a **flick key**, as in the standard Japanese 12-key flick input:
- **tap** = the big letter (the group's most frequent one);
- **slide left / up / right** = the other letters, clockwise in alphabetical order;
- **slide down** = the digit;
- **hold on Е** = Ё (the same long-press convention as Russian smartphone keyboards).

When you press, five bubbles bloom around your finger showing exactly what each direction gives. The method teaches itself.

Why the alternatives lose on this screen. About 20 keys of ≥ 50 px fit under a text field (verify: round-ui):
- **Pages** (≈15 letters per page): Russian needs 3 pages; constant page switching and visual search.
- **Long-press for every variant**: ≥ 400 ms per letter. We use it only for the rare Ё.
- **T9 multi-tap**: 1–4 taps plus timeouts per letter. Predictive T9 needs a dictionary.
- **Rim picker**: 36 targets at r 150 are 26 px each (< 50). The COMPASS evidence comes from physical-bezel watches.

What flick gives instead:
- **one gesture per letter**;
- keys 60×53 px;
- digits without switching mode;
- 46 % of Russian letters (а е и о т ч ы я) as plain taps, because the centre is the frequent letter.

### 5.2 Geometry and proof that it fits

- Keyboard object at x 30, y 100, size 300×224.
- Style: `pad_all 0`, `pad_row 4`, `pad_column 4`, `bg_opa 0`, `border 0`.
- LVGL row layout: (224 − 3·4) / 4 = **53 px rows**.
- Width units: 4 px per unit in every row (§5.5).
- Hit areas grow into the 4 px gaps, so every key's touch target is ≥ 54×57.

| Row | y | d_max | Allowed x (half-chord) | Keys (x edges · width) | Used x | Slack |
|---|---|---|---|---|---|---|
| Field | 44–96 (pill, caps r 26 at (91,70),(269,70)) | — | caps: √(89²+110²) + 26 = 167.5 ≤ 168 | 65–295 · 230 | 65–295 | 0.5 |
| R1 | 100–153 | 80 | 32.3–327.7 (147.7) | [1] 58–118·60 · [2] 122–182·60 · [3] 186–246·60 · [⌫] 250–302·52 | 58–302 | 25.7 |
| R2 | 157–210 | 30 | 14.7–345.3 (165.3) | [⇧] 30–82·52 · [4] 86–146·60 · [5] 150–210·60 · [6] 214–274·60 · [123] 278–330·52 | 30–330 | 15.3 |
| R3 | 214–267 | 87 | 36.3–323.7 (143.7) | [7] 58–118 · [8] 122–182 · [9] 186–246 · [🌐] 250–302·52 | 58–302 | 21.7 |
| R4 | 271–324 | 144 | 93.5–266.5 (86.5) | [✕] 94–146·52 · [␣] 150–210·60 · [✓] 214–266·52 | 94–266 | 0.5 (corner (94,324): r = 167.7) |

```
            ╭──────────── rim hint (r158) ─────────────╮
               ( text field pill 65–295 × 44–96 )
          [ .,?! ][ АБВГ ][ ДЕЖЗ ][ ⌫ ]               R1 y100–153
     [ ⇧ ][ ИЙКЛ ][ МНОП ][ РСТУ ][123]                R2 y157–210
          [ ФХЦЧ ][ ШЩЪЫ ][ ЬЭЮЯ ][ EN ]               R3 y214–267
                [ ✕ ][ пробел ][ ✓ ]                   R4 y271–324
```

- **Text field.** `lv_textarea`, one line (`lv_textarea_set_one_line`), F20, radius 26, fill `surface-1`, 2 px accent border when focused, accent cursor.
  - Pad 20 / 20; pad_right 52 when the eye toggle is present.
  - Placeholder in `text-3`.
- **Eye toggle** (password fields): button 40×40 centred at (265,70), `ext_click_area` 8, so the hit area is 56×56.
- **Rim hint** above the field: e.g. «Пароль · HomeNet · 12/63».

### 5.3 Key semantics

**Flick keys** (`CUSTOM_1`):
- Dead zone |v| < 16 px → centre.
- Otherwise the direction is the dominant axis (|vx| > |vy| → ←/→, else ↑/↓).
- |v| > 96 px → cancel (petals collapse, nothing is typed).
- Commit on release.
- **Hold variant:** in the centre for ≥ 450 ms, the centre bubble morphs (Е → Ё, . → …) with a 120 ms wiggle. Release commits the variant; sliding away cancels it.

**Function keys:**

| Key | Behaviour |
|---|---|
| **⌫** | fires on press and auto-repeats (`lv_indev_set_long_press_repeat_time(indev, 80)`); after 10 repeats it deletes whole words |
| **⇧** | tap = one-shot shift; double-tap within 350 ms = caps lock (key filled with accent, `CHECKED`); maps switch lower/upper |
| **123** | → NUMBER; in NUMBER it reads «#+=» → SPECIAL; in SPECIAL it reads «123» → NUMBER |
| **🌐** | label shows the *target* language («EN» while in RU, «РУ» while in EN); switches the letter layout; in NUMBER and SPECIAL the R2-left key «АБВ»/«ABC» returns to letters |
| **␣** | tap = space; horizontal drag > 14 px = cursor mode (each 14 px → `lv_textarea_cursor_left/right`, no space typed); flick ↓ = «0»; double-tap = «. » (chat contexts only) |
| **✕** | sends `LV_EVENT_CANCEL` |
| **✓** | sends `LV_EVENT_READY`; the icon is paper-plane for chat and translator, check otherwise |

**Auto-capitalisation** (chat and translator contexts): the first letter, and the first after «. », «! » or «? », gets one-shot shift.

### 5.4 Layouts

Each cell reads **centre / ← / ↑ / → / ↓ (hold)**. Key faces show the centre glyph in F24 `text-1` and the four small glyphs in F14. Letters are `text-2`; digits and symbols on ↓ (and EN ↑) are in the mode accent.

**RU lower (USER_1)** (upper case, USER_2, is identical with capitals):

| Key | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|
| centre | . | а | е | и | о | т | ч | ы | я |
| ← | , | б | д | й | м | р | ф | ш | ь |
| ↑ | ? | в | ж | к | н | с | х | щ | э |
| → | ! | г | з | л | п | у | ц | ъ | ю |
| ↓ | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
| hold | … | — | **ё** | — | — | — | — | — | — |

32 letters + Ё = **33**. The ␣ key gives ↓ «0».

**EN lower (TEXT_LOWER)** (upper case, TEXT_UPPER, is identical with capitals; symbols unchanged). Three-letter groups carry a useful symbol on ↑:

| Key | 1 | 2 abc | 3 def | 4 ghi | 5 jkl | 6 mno | 7 pqrs | 8 tuv | 9 wxyz |
|---|---|---|---|---|---|---|---|---|---|
| centre | . | a | e | i | l | o | s | t | w |
| ← | , | b | d | g | j | m | p | u | x |
| ↑ | ? | @ | - | _ | / | : | q | ' | y |
| → | ! | c | f | h | k | n | r | v | z |
| ↓ | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |

**NUMBER** (direct taps, no flicks):
- R1 [1] [2] [3] [⌫]
- R2 [АБВ/ABC] [4] [5] [6] [#+=]
- R3 [7] [8] [9] [0] (the 52 px slot)
- R4 [✕] [.] [✓]

**SPECIAL** (flick groups; covers all 32 ASCII symbols):

| Key | S1 | S2 | S3 | S4 | S5 | S6 | S7 | S8 | S9 | R3-right ":)" |
|---|---|---|---|---|---|---|---|---|---|---|
| centre | . | - | @ | ( | ) | / | " | * | ₽ | :) |
| ← | , | _ | # | [ | ] | \ | ' | № | € | ;) |
| ↑ | ? | + | $ | { | } | \| | ` | ° | £ | :D |
| → | ! | = | % | < | > | : | ~ | § | ¥ | :( |
| ↓ | … | — | & | « | » | ; | ^ | • | ¢ | <3 |

SPECIAL rows: R1 [S1][S2][S3][⌫] · R2 [АБВ][S4][S5][S6][123] · R3 [S7][S8][S9][:)] · R4 [✕][␣][✓]. The text emoticons stand in for emoji the font cannot draw.

### 5.5 Mapping onto lv_keyboard / lv_buttonmatrix

The verified 9.5 behaviour this design relies on:
- `lv_keyboard_set_map(kb, mode, map, ctrl_map)` stores maps per mode in **static tables shared by all instances**.
- The constructor adds `lv_keyboard_def_event_cb` on `VALUE_CHANGED`.
- Buttonmatrix sends `VALUE_CHANGED` **on PRESSED** for keys that are neither `CLICK_TRIG` nor `POPOVER`, and **on RELEASED** for `CLICK_TRIG` keys.
- When the finger slides onto another key, the buttonmatrix *discards* the selection (`btn_id_sel = NONE`).
- `LONG_PRESSED_REPEAT` re-sends `VALUE_CHANGED` unless `NO_REPEAT` is set.
- Width units are the low 4 bits (1–15). Layout formula: `btn_x1 = (max_w_no_gap·units_before)/unit_cnt + btn·pcol`.

```c
/* Button indices: identical in all six modes (22 buttons) */
enum { K_SP0, K_G1, K_G2, K_G3, K_BKSP, K_SP1,
       K_SHIFT, K_G4, K_G5, K_G6, K_MODE,
       K_SP2, K_G7, K_G8, K_G9, K_LANG, K_SP3,
       K_SP4, K_CLOSE, K_SPACE, K_OK, K_SP5 };

#define SP " "   /* hidden spacer label, never drawn */
static const char * const map_ru_lo[] = {
  SP, ".", "а", "е", LV_SYMBOL_BACKSPACE, SP, "\n",
  SYM_SHIFT, "и", "о", "т", "123", "\n",
  SP, "ч", "ы", "я", "EN", SP, "\n",
  SP, LV_SYMBOL_CLOSE, " ", LV_SYMBOL_OK, SP, "" };
/* map_ru_up: ".","А","Е" / "И","О","Т" / "Ч","Ы","Я"
   map_en_lo: ".","a","e" / "i","l","o" / "s","t","w", lang key "РУ"   (map_en_up: capitals)
   map_num_ru/en: "1","2","3" / "АБВ"|"ABC","4","5","6","#+=" / "7","8","9","0" / CLOSE,".",OK
   map_sym: ".","-","@" / "АБВ","(",")","/","123" / "\"","*","₽",":)" / CLOSE," ",OK */

#define H  LV_BUTTONMATRIX_CTRL_HIDDEN
#define G  (LV_BUTTONMATRIX_CTRL_CUSTOM_1 | LV_BUTTONMATRIX_CTRL_NO_REPEAT)   /* flick group */
#define F  (LV_BUTTONMATRIX_CTRL_CUSTOM_2 | LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG)
#define BK (LV_BUTTONMATRIX_CTRL_CUSTOM_2)                                   /* press + repeat */
#define D  (LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG) /* direct char */
static const lv_buttonmatrix_ctrl_t ctrl_flick[22] = {
  H|6,  G|15, G|15, G|15, BK|13, H|6,      /* R1: 6+15·3+13+6 = 70 u; (300-5·4)/70 = 4 px/u */
  F|13, G|15, G|15, G|15, F|13,            /* R2: 71 u; (300-4·4)/71 = 4 px/u */
  H|6,  G|15, G|15, G|15, F|13, H|6,       /* R3: 70 u */
  H|15, F|13, G|15, F|13, H|15 };          /* R4: 71 u */
/* ctrl_num: D instead of G (K_LANG slot = D|13 for "0", K_SPACE = D|15 for ".")
   ctrl_sym: as ctrl_flick, K_LANG slot = G|13 (":)" group) */
```

**Setup:**
```c
kb = lv_keyboard_create(scr);
lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);          /* we own all text insertion */
lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_1, map_ru_lo, ctrl_flick);
lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_2, map_ru_up, ctrl_flick);
lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, map_en_lo, ctrl_flick);
lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, map_en_up, ctrl_flick);
lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_NUMBER, map_num_ru, ctrl_num);  /* re-set on language change */
lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, map_sym, ctrl_sym);
lv_keyboard_set_textarea(kb, ta);                                /* keeps the cursor shown */
lv_obj_align(kb, LV_ALIGN_TOP_LEFT, 30, 100); lv_obj_set_size(kb, 300, 224);
lv_obj_remove_flag(kb, LV_OBJ_FLAG_GESTURE_BUBBLE);
lv_obj_add_flag(kb, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
lv_obj_add_event_cb(kb, kb_value_cb, LV_EVENT_VALUE_CHANGED, ctx);
lv_obj_add_event_cb(kb, kb_flick_cb, LV_EVENT_ALL, ctx);        /* PRESSED / PRESSING / RELEASED / PRESS_LOST */
lv_obj_add_event_cb(kb, kb_face_cb,  LV_EVENT_DRAW_TASK_ADDED, ctx);
```

- **`kb_value_cb`** (handles F, BK and D keys).
  - `id = *(uint32_t *)lv_event_get_param(e)`. If `lv_buttonmatrix_has_button_ctrl(kb, id, CUSTOM_1)`, return, because flicks are handled elsewhere.
  - Otherwise switch on (mode, id): ⌫ → `lv_textarea_delete_char`; ⇧ → shift / caps logic (after any `lv_keyboard_set_mode`, re-apply `CHECKED` with `lv_buttonmatrix_set_button_ctrl(kb, K_SHIFT, LV_BUTTONMATRIX_CTRL_CHECKED)`, because mode changes reload the ctrl map); 123 / #+= / АБВ / 🌐 → `lv_keyboard_set_mode`; ✕ / ✓ → `lv_obj_send_event(kb, LV_EVENT_CANCEL/READY, NULL)`; D keys → `lv_textarea_add_text(ta, lv_buttonmatrix_get_button_text(kb, id))`.
- **`kb_flick_cb`** (G keys).
  - `PRESSED`: `id = lv_buttonmatrix_get_selected_button(kb)`; user callbacks run after the class handler, so the id is valid. Store `id`, `p0 = lv_indev_get_point`, `t0 = lv_tick_get()`, and open the petals.
  - `PRESSING`: compute `v`, the direction, cancel, and hold-variant dwell (`lv_tick_elaps(t0)`); update the highlighted bubble.
  - `RELEASED` / `PRESS_LOST`: commit `table[mode][id - group_base].{c,l,u,r,d,hold}` and close the petals.
  - The stored `id` is used because the matrix clears its selection once the finger crosses into a neighbouring key. `PRESS_LOCK` (set on children by default) keeps the keyboard receiving `PRESSING` outside its bounds.
  - ␣ is handled here as well (cursor drag, ↓ «0», double-tap).
- **`kb_face_cb`** (key faces; the pattern from lv_example_buttonmatrix_2).
  - `t = lv_event_get_draw_task(e)`, `base = lv_draw_task_get_draw_dsc(t)`. Act only when `base->part == LV_PART_ITEMS`. `base->id1` is the button index.
  - For `LV_DRAW_TASK_TYPE_LABEL` on G keys: keep the centre label (F24) and add four `lv_draw_label` calls on `base->layer` (F14) in boxes relative to the known key rect (§5.2): left x1+3–x1+17, up y1+2–y1+20, right x2−17–x2−3, down y2−19–y2−1, all centred.
  - For F keys: set `label_dsc->font = &f_body_20` («123», «EN», «АБВ»).
  - ␣: set `label_dsc->opa = 0` and draw «пробел»/«space» (F14) plus «0» (↓).
  - Fill colours by ctrl: G keys `surface-2`; F and BK keys `surface-1` with accent text; pressed `surface-3`; `CHECKED` accent.
- **Petal overlay.**
  - 5 pre-created `lv_obj` circles of 52 px (`surface-3`, F24 label child, `CLICKABLE` cleared) on `lv_layer_top()`.
  - Offsets: centre (0,0), ← (−58,0), ↑ (0,−58), → (58,0), ↓ (0,58) from the key centre.
  - **Clamp:** move the whole cluster toward C in 2 px steps until every bubble centre b satisfies |b − C| ≤ 142 (= 168 − 26). Flicks are relative, so moving the cluster never changes what a gesture types.
  - Highlighted bubble: 52 → 60 px, accent fill, text `#0B0C14`.

### 5.6 Context presets

| Context | Start layout | Field | ✓ | Extras |
|---|---|---|---|---|
| Wi-Fi password | EN lower | `lv_textarea_set_password_mode(ta, true)` (last char shown 1500 ms, `LV_TEXTAREA_DEF_PWD_SHOW_TIME`), max 63 | check «Подключить» | eye toggle; no auto-cap; BOOT dictation off |
| SSID | EN lower | max 32 bytes (counter shown after 24) | check | — |
| API key | EN lower, prefill `sk-or-v1-` | max 256 | check | validation: prefix and length ≥ 20, else the field shakes; hint «удобнее с телефона» links to S11 |
| Base URL / Model ID | EN lower, prefill `https://` or `google/` | max 200 | check | — |
| Chat message | UI language lower | max 500, auto-cap, double-space «. » | paper-plane (sends a text turn) | BOOT hold = dictation (coral border pulse; STT result inserted at the cursor) |
| Translator text | language of the tapped chip | max 300 | paper-plane (translate) | direction fixed by the chip |

---

## 6. Interaction design

### 6.1 Push-to-talk (Orbi and BOOT use the same state machine)

```
IDLE ──touch-down on Orbi / BOOT down──> ARMED (recording now; 300 ms pre-roll ring kept while a talking screen is on)
ARMED ──released < 300 ms, moved < 16 px──> LATCHED (hands-free)
ARMED ──held ≥ 300 ms──> PTT
PTT ──release──> speech ≥ 400 ms ? SEND : toast «Держите дольше» → IDLE
PTT ──finger > R + 48 from orb centre──> CANCEL_ZONE ──back inside──> PTT
CANCEL_ZONE ──release──> discard, SHRUG → IDLE
LATCHED ──tap Orbi | BOOT click | 1.2 s silence after speech | 30 s cap──> SEND
LATCHED ──no speech within 6 s──> discard quietly (SHRUG) → IDLE
SEND → UPLOADING → THINKING → (transcript) → (reply streams) → SPEAKING (if voice) → IDLE
```

- Recording starts at touch-down, so the first syllable is never clipped. Hold-vs-tap is decided after the fact.
- The VAD is energy-based: 20 ms frames; speech when RMS > 3 × the noise floor, which is tracked during the idle pre-roll. It drives the latched auto-stop and the "too short" check.
- A BOOT hold on any non-talking screen first jumps to the last talking mode (instant screen load), then runs ARMED.

### 6.2 Cancel, interrupt, barge-in

**Touching Orbi always interrupts:**
- UPLOADING / THINKING + tap → abort the HTTP request, SHRUG, IDLE.
- SPEAKING + tap → stop I2S, flush the PCM ring, cancel queued sentence TTS requests, IDLE.
- A BOOT click does the same.
- **Hold during SPEAKING or THINKING** → interrupt and immediately ARMED (barge-in).
- A touch on the orb while BOOT-PTT is active cancels the recording.

### 6.3 Text-only mode and no speaker

- The left satellite toggles `ui_s_voice_out`, with the "Shh" / "o" face and a toast «Только текст» / «Голос включён».
- In text-only mode TTS is never requested. The face shows the mood for 2 s and then reading eyes while the text streams.
- When `ui_s_speaker == 0`, `voice_out` is forced to 0 and the satellite shows mute + an ember dot (§4.15).
- The whole UI works with no speaker. Earcons (press pop 40 ms, send whoosh 120 ms, error double blip, success two-note chime; PCM in flash) play only when a speaker is present and «Звуки интерфейса» is on.

### 6.4 Translator turn-taking

**One orb with automatic detection** (requirement 2):
- The translator schema is `{src_lang, transcript, translation}`, with `src_lang` first.
- The prompt restricts detection to exactly the two selected languages.

Turn sequence:
1. Release → the waveform bubble appears centred.
2. `src_lang` is parsed → the bubble slides to the A or B side and the source chip pulses (52 → 60 → 52 px, 300 ms ease-out-back).
3. Orbi tints to the **target** colour, looks at the target chip, and speaks the translation (mouth driven by TTS level).
4. If «Авто-продолжение» is on (default off), Orbi then enters LATCHED listening for 6 s, facing the listener's chip.

Two "me / them" buttons were rejected:
- they halve the orb;
- both people must learn which button is theirs;
- the requirement asks for detection.

Mis-detection between close languages (ru/uk, es/pt) is fixed in one tap: «Не тот язык ⇄» on the bubble re-runs the stored audio with the direction forced. Typing via a chip also fixes the direction.

### 6.5 Chat bubble lifecycle

1. **SEND:** the user bubble appears at once with the waveform of the recording (slide up 20 px + fade, 220 ms).
2. **Transcript parsed:** the waveform crossfades into the text (200 ms, bubble area only).
3. **Reply streaming.**
   - With `stream: true` + structured output, the SSE parser:
     - ignores `: OPENROUTER PROCESSING` comment lines;
     - treats `finish_reason: "error"` as a failure;
     - extracts the `reply` string incrementally (escape-aware, never splitting a UTF-8 sequence).
   - Text is appended to the assistant bubble at most every 66 ms (15 Hz) via `lv_label_ins_text(label, LV_LABEL_POS_LAST, chunk)`.
   - The viewport follows with `lv_obj_scroll_to_y(…, LV_ANIM_OFF)` only when a new line wraps (≤ 4 Hz).
4. **Spoken highlight.** With voice on, the reply is split into sentences for TTS. The label uses `lv_label_set_recolor(label, true)` and is re-set once per sentence: spoken part in `text-1`, the rest in `text-2`.
5. **Prompt rule for the UI:** replies ≤ 300 characters, plain text, no markdown, no emoji, and one `mood` value.

### 6.6 Recovery

- The last recording (≤ 30 s × 32 KB/s ≈ 960 KB, PSRAM) is kept until a send succeeds. Every error card offers «Отправить снова».
- Offline speech is queued and auto-sent within 2 min of reconnecting.
- NVS writes happen only in IDLE, debounced by 1.5 s, because the flash cache is disabled during writes and that stalls PSRAM access.

---

## 7. Animations

### 7.1 Cost model and budgets (estimates; calibrate in bring-up)

- **Flush:** QSPI 80 MHz × 4 lines ≈ 40 MB/s → **50 ns/px**. It runs on DMA in parallel with rendering the next chunk (two buffers).
- **Software render** (no S3 SIMD with LVGL ≥ 9.2):

  | Content | Estimated cost |
  |---|---|
  | opaque fill | 5–10 ns/px |
  | VER/HOR gradient | 10–20 ns/px |
  | alpha blend | 30–50 ns/px |
  | 4-bpp text | 60–90 ns per glyph-box px |
  | anti-aliased edge px (circles, arcs, lines) | 100–200 ns |
  | overhead per refresh area / chunk (tree walk + draw-task creation) | 50–150 µs |

- **Chunking:** a buffer holds 14,400 px, so an area of width w is drawn in strips of ⌊14400/w⌋ lines.
- **Budgets:**
  - **Continuous loops ≤ 30 k px/frame.** About 2–4 ms per frame → ≤ 12 % of core 1 at 30 fps.
  - **One-shot transitions ≤ 70 k px/frame.** About 5–9 ms render; the 3.5 ms flush overlaps. That is ≥ 60 fps in theory and **≥ 30 fps with a 2× safety margin**.
  - A full-screen frame (129.6 k px) happens only on the single frame after a screen load.
- **Calibration and acceptance:**
  1. Run `lv_demo_benchmark` and a primitives test screen; `LV_USE_SYSMON` + `LV_USE_PERF_MONITOR`.
  2. Replace the per-pixel constants with measured values.
  3. Acceptance: every row below runs at ≥ 30 fps on the device with Wi-Fi and TLS active.
- **Settings:** `LV_DEF_REFR_PERIOD 16` (transitions may reach 60 fps). Continuous loops are driven by a **33 ms face-tick timer**, not by `lv_anim`, so they cap at 30 fps.

### 7.2 Continuous state loops (driven by the face tick, 33 ms)

| State | Mechanism | Animated parameters | Timing / easing | Invalidated per frame | Est. |
|---|---|---|---|---|---|
| Idle "breathing" | face tick at 10 fps | halo mix weights 22→30 %, 12→16 %, 6→8 % | 4000 ms sine | halo box 224×224 = 50 k px at **10 fps** (≈2 % CPU) | ok |
| Blink (idle, all states) | `lv_anim` with custom exec | eye_h 44 → 4 → 44 | close 70 ms ease-in, hold 50, open 110 ms ease-out; random 2.5–6 s; 20 % double blink | eye box ≈ 80×52 = 4.2 k | ≥ 60 |
| Glance | `lv_anim` | gaze x/y to a random target (±10, ±6) | 260 ms `LV_ANIM_SET_EASE_IN_OUT_CUBIC`, hold 0.8–2 s | eye union ≈ 98×64 = 6.3 k | ≥ 60 |
| Listening (STAGE) | face tick 30 fps; level from an atomic | 32 bar lengths `L_i = 4 + 24·lvl·(0.55 + 0.45·sin(3θ_i + 2.1t)·sin(2θ_i − 1.3t))`; level attack 40 ms, release 160 ms; eye scale +10 %·lvl | — | **per-bar boxes** (≤ 30×30 at 45°, 40×6 at 0°) ≈ 19 k + eyes 6.3 k ≈ **25 k** | ≥ 30 |
| Listening (CHAT) | same | 16 bars, L 3–12 | — | ≈ 3 k + eyes 1.5 k | ≥ 60 |
| Record-time arc | face tick 10 fps | arc end angle | linear over 30 s | tip delta box ≈ 12×12 | ok |
| Uploading | face tick ≤ 15 fps | progress arc end = 360°·sent/total | — | tip delta box | ok |
| Thinking | face tick 30 fps + `lv_anim` (saccade) | 3 dots, 360°/1400 ms linear; gaze x −8 ↔ +8 | saccade 900 ms ease-in-out with reverse, infinite | 3 dots × 2 boxes × 14² ≈ 1.2 k + eyes 6.3 k ≈ 7.5 k | ≥ 60 |
| Speaking | face tick 30 fps | mouth h = 4 + 14·tts_lvl; 32 aqua bars, L ≤ 14 | level smoothing attack 30 / release 120 ms | mouth 30×22 + bars ≈ 12 k ≈ **13 k** | ≥ 60 |
| Text reading | face tick 15 fps | gaze x ±6 | 1.5 Hz sine | eyes 6.3 k | ok |
| Offline / Doze | face tick 10 fps | breathing 6000 ms; slow blink every 8 s | — | as idle | ok |

Bars are invalidated **one by one** with `lv_obj_invalidate_area()`. 32 bars plus other areas can exceed the default `LV_INV_BUF_SIZE` of 32. On overflow LVGL drops the list and redraws the **whole screen**. So compile with `LV_INV_BUF_SIZE=64` (§8.2).

### 7.3 Transitions

| ID | Transition | Mechanism | Properties, duration, easing | Peak invalidated / frame |
|---|---|---|---|---|
| T1 | Idle → Listening (touch-down) | `lv_anim` ×4 | body squash x +8 % / y −8 % (90 ms ease-out), then back to 1.0 (220 ms `EASE_OUT_BACK`); colour Violet → Coral 160 ms ease-out; crown + hint opa → 0 (150 ms); satellites 52 → 0 px centre-anchored (150 ms ease-in) | body/halo 50 k + crown 11 k + satellites 5 k ≈ **66 k** |
| T2 | Listening → Uploading | `lv_anim` | bars collapse 120 ms ease-in; colour → Amber over 80–280 ms; progress arc starts | ≤ **69 k** (overlap only 80–120 ms) |
| T3 | Uploading → Thinking | custom draw | arc completes, then its start chases its end ("unwind", 250 ms ease-in; tip deltas only, **no opa fade**, which would invalidate a 212² box); dots grow 0 → 10 px (160 ms) | < 5 k |
| T4 | Thinking → Speaking | `lv_anim` | colour Amber → Aqua 200 ms; expression blend 0 → 1024 over 240 ms (`EASE_IN_OUT_CUBIC`) | 50 k (STAGE) / 12 k (CHAT) |
| T5 | Speaking → Idle | `lv_anim` | mouth → 0 (120 ms), bars collapse (150 ms), colour → Violet (300 ms), crown/hint fade in, satellites 0 → 52 (200 ms `EASE_OUT_BACK`) | ≈ 60 k |
| T6 | STAGE → CHAT (first SEND) | **`lv_anim_timeline`**, 420 ms | a1 0–320 ms: orb cy 164 → 76 (`EASE_IN_OUT_CUBIC`), R 88 → 36 (`EASE_OUT_BACK`); a2 0–360 ms: satellites orbit L 135° → 220°, R 45° → −40°, r 132 → 128 (custom exec sets pos = C + r·(cos θ, sin θ) − 26); a3 180–400 ms: user bubble translate_y 20 → 0 + opa 0 → 255 (`EASE_OUT_CUBIC`); hint opa → 0 | orb ≈ 30 k + satellites 10.8 k + bubble 25 k ≈ **66 k** at t ≈ 250 ms |
| T7 | CHAT → STAGE (45 s idle) | `lv_anim_timeline`, 500 ms, **sequenced** | 0–180 ms viewport content opa → 0 (48 k/frame); 180–500 ms a1 + a2 reversed | ≤ 55 k (no overlap) |
| T8 | Mode switch (swipe, in-screen) | `lv_anim` | CHAT: viewport content slides 32 px toward the swipe + opa → 0 (140 ms ease-in), swap containers, slide in from the other side (180 ms ease-out); orb colour crossfade (240 ms) + glance toward the swipe (±10 px); satellites "flip" w 52 → 4 → 52 (200 ms) with the icon swap at the midpoint; crown text crossfade; page-dot pill slides | STAGE: orb 50 k + satellites 5 k. CHAT: 48 k + 12 k ≈ 60 k |
| T9 | Screen load (S5, sub-screens) | `lv_screen_load` (instant) + `lv_anim` | the first frame is full-screen; then visible rows cascade translate_y 16 → 0 + opa 0 → 255 (180 ms `EASE_OUT_CUBIC`, 30 ms stagger). Back = instant, no cascade | ≈ 30–45 k (2–3 rows active) |
| T10 | Keyboard open | instant load + `lv_anim` | text field width 52 → 230, centre-anchored (200 ms `EASE_OUT_BACK`); keys are static | 12 k |
| T11 | Petal bloom / pick / commit | `lv_anim` (size + pos exec) | bloom: 5 bubbles 0 → 52 px, stagger 0/15/30/45/60 ms, 140 ms `EASE_OUT_BACK`; pick: 52 → 60 px + accent (90 ms ease-out); commit: a "letter flight" label from the bubble to the cursor (`lv_label_get_letter_pos`) in 150 ms `EASE_IN_CUBIC`, opa → 0 in the last 50 ms (the character is inserted at once, the flight is decorative); collapse 100 ms ease-in | ≈ 29 k bloom, 7–8 k otherwise |
| T12 | Mode Ring open | instant load + `lv_anim` | items grow 0 → 64 (stagger 30 ms, 220 ms `EASE_OUT_BACK`); arclabels opa 0 → 255 (150 ms); centre Orbi R 0 → 40 (260 ms `EASE_OUT_BACK`) | ≈ 27 k |
| T13 | Celebrate | `lv_anim` ×3 | hop: body dy 0 → −18 (180 ms ease-out) → 0 (320 ms `lv_anim_path_bounce`); landing squash ±10 % (120 ms); 10 sparkles (4-point stars: 2 crossed 10 px lines, width 3) fly 0 → 40 px outward with opa 255 → 0 over 600 ms | orb ≈ 36 k + sparkles 4 k |
| T14 | Error | `lv_anim`, **sequenced** | colour → Ember 0–200 ms (50 k); shake 200–680 ms: body dx = 8·e^(−4t)·sin(6πt) (custom path) (35 k); then the T6 geometry to the head + error card cascade (title, body, pills: translate_y 12 → 0 + opa, 180 ms, 40 ms stagger) | ≤ 50 k |
| T15 | Toast | `lv_anim` | arc span 0 → 70° (200 ms ease-out) + text opa; reverse after 2.5 s | 15 k |
| T16 / T17 | Wake / Sleep | LEDC hardware fade + `lv_anim` | wake: backlight 0 → set value in 300 ms; eyes h 4 → 50 → 44 (260 ms `EASE_OUT_BACK`). Sleep: lids close 600 ms ease-in-out, then backlight fades out over 800 ms (no render cost) | 6 k |
| T18 | Press feedback (pills, rows, satellites) | shared `lv_style_transition_dsc_t` | `bg_color`, 80 ms ease-out | object box only |
| T19 | Waveform → transcript | `lv_anim` | waveform opa 255 → 0 while the text opa goes 0 → 255 (200 ms) | ≈ 252×60 = 15 k |
| T20 | Translator bubble to its side | `lv_anim` | translate_x 0 → ±40 (220 ms `EASE_OUT_CUBIC`) + stripe colour | ≈ 25 k |

Scrolling (chat viewport 280×172 = 48 k px; orbit list 252×280 = 70.5 k px) uses LVGL's own scroll animation and stays within the transition budget.

Tearing: there is no TE sync on QSPI. Large movements are therefore limited to ≤ 420 ms (T6/T7), and continuous loops never move large objects.

### 7.4 Slow patterns that are banned, and their replacements

| Banned | Instead |
|---|---|
| `clip_corner` on the root or on lists (ARGB8888 layers + masks every frame) | the bezel hides the corners; layout keeps content inside r 168; rows fade with style `opa` |
| style `transform_scale` / `rotation` / `skew` on objects > 64 px | squash, grow and orbit by changing w/h/pos or custom-draw parameters |
| box shadows / glow shadows | 3 opaque pre-blended halo rings |
| `opa_layered`, blend modes, bitmap masks | per-object `opa` (no layer); opaque modal screens instead of dimming overlays |
| full-screen Lottie / ThorVG | procedural face (`LV_USE_LOTTIE 0`, `LV_USE_THORVG 0`) |
| radial / conical / angled gradients (`LV_USE_DRAW_SW_COMPLEX_GRADIENTS`) | 2-stop VER/HOR gradients on ≤ 176 px bodies only |
| `LV_SCREEN_LOAD_ANIM_FADE_*` / `MOVE_*`, tileview | instant loads + local entrance animations |
| `lv_label_set_text` of long text every frame | batched appends at ≤ 15 Hz |
| > 64 invalidated areas per frame | per-bar invalidation with `LV_INV_BUF_SIZE=64` |
| NVS writes during animations | debounced saves in IDLE |

---

## 8. LVGL 9.5 implementation notes

### 8.1 Modules (`main/ui/`)

| File | Role |
|---|---|
| `ui.c` | screen manager, back stack, global gestures, PWR/BOOT mapping |
| `ui_theme.c` | palette, fonts, all shared styles |
| `ui_state.c` | subject wrappers, UI event queue pump |
| `ui_face.c` | Orbi widget |
| `ui_main.c` | crown, orb, satellites, viewport, dots, hint, toast, error card |
| `ui_chat.c` | bubbles, streaming, sanitiser, history glue |
| `ui_translator.c` | chips, translator bubbles |
| `ui_orbit_list.c` | curved list |
| `ui_keyboard.c` | maps, flick tables, petals, handlers |
| `ui_dial.c`, `ui_qr.c`, `ui_settings.c`, `ui_wifi.c`, `ui_models.c`, `ui_voices.c`, `ui_diag.c`, `ui_onboarding.c`, `ui_ring.c`, `ui_dialog.c` | individual screens |
| `fonts/f_*.c` | generated fonts |
| `strings/ui_str_{ru,en}.h` | string tables (or the 9.5 `LV_USE_TRANSLATION` module with `lv_label_set_translation_tag`, behind the same wrapper) |

The UI is hardware-independent and builds unchanged in the LVGL PC simulator at 360×360 (draw the 168/180 circles as a debug overlay there).

### 8.2 Configuration

**lv_conf** (Kconfig in ESP-IDF):

| Setting | Value / note |
|---|---|
| `LV_COLOR_DEPTH` | 16 |
| `LV_DEF_REFR_PERIOD` | 16 |
| `LV_USE_OS` | `LV_OS_FREERTOS` |
| `LV_USE_DRAW_SW_ASM` | NONE |
| `LV_USE_DRAW_SW_COMPLEX_GRADIENTS` | 0 |
| `LV_DRAW_SW_CIRCLE_CACHE_SIZE` | 16 (many radii per frame: halo, body, eyes, satellites, pills, keys) |
| `LV_DRAW_SW_SHADOW_CACHE_SIZE` | 0 |
| `LV_USE_OBSERVER` | 1 |
| `LV_USE_ARCLABEL` | 1 |
| `LV_USE_QRCODE` | 1 |
| `LV_USE_KEYBOARD` | **1** (xiaozhi's defaults turn it off) |
| `LV_USE_TEXTAREA` / `LV_USE_ARC` / `LV_USE_LABEL` | 1 |
| `LV_USE_LIST` / `LV_USE_MENU` / `LV_USE_WIN` / `LV_USE_TILEVIEW` / `LV_USE_ROLLER` | 0 (the first three are deprecated in 9.6) |
| `LV_USE_LOTTIE` / `LV_USE_THORVG` / `LV_USE_VECTOR_GRAPHIC` | 0 |
| `LV_USE_SNAPSHOT` | 1 (dev console screenshots) |
| `LV_OBJ_STYLE_CACHE` | 1 |
| `LV_LABEL_TEXT_SELECTION` | 0 |
| `LV_USE_FONT_PLACEHOLDER` | 1 |
| `LV_USE_SYSMON` / `LV_USE_PERF_MONITOR` | 1 in development builds only |
| `LV_USE_THEME_DEFAULT` | 0 (every style is ours) |

**Invalidation buffer.** `LV_INV_BUF_SIZE` lives in the private header `lv_display_private.h` behind `#ifndef`. Add `LV_INV_BUF_SIZE=64` as a compile definition (`idf_build_set_property(COMPILE_DEFINITIONS "LV_INV_BUF_SIZE=64" APPEND)`).

**Heap.** Put the LVGL heap in **PSRAM**: copy LVGL's clib allocator core and use `heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`, falling back to internal RAM. Draw buffers stay internal.

**esp_lvgl_port:**
- `lvgl_port_cfg_t`: `task_priority 4`, `task_stack 10240` (internal), `task_affinity 1`, `task_max_sleep_ms 500`, `timer_period_ms 5`.
- Display: `buffer_size = 360*40`, `double_buffer = true`, `hres = vres = 360`, `color_format = LV_COLOR_FORMAT_RGB565`, `flags = { .buff_dma = 1, .buff_spiram = 0, .swap_bytes = 1 }`.
- Add a `rounder_cb` (even x1 / odd x2) only if the panel shows odd-alignment artefacts.

**Indev:**
- `lv_indev_set_scroll_limit(indev, 10)`
- `lv_indev_set_gesture_min_distance(indev, 50)`
- `lv_indev_set_gesture_min_velocity(indev, 3)`
- `lv_indev_set_long_press_time(indev, 400)`
- `lv_indev_set_long_press_repeat_time(indev, 80)`

Every screen clears `LV_OBJ_FLAG_SCROLLABLE` and has an opaque `bg`.

### 8.3 The Orbi widget

```c
lv_obj_t * orbi_create(lv_obj_t * parent);
void orbi_set_geometry(lv_obj_t * o, int32_t cx, int32_t cy, int32_t r);   /* also used as anim exec target */
void orbi_set_state(lv_obj_t * o, orbi_state_t s);                          /* colour + which loops run */
void orbi_set_expression(lv_obj_t * o, orbi_expr_t e, uint32_t blend_ms);
void orbi_look_at(lv_obj_t * o, lv_point_t p, uint32_t hold_ms);
void orbi_set_badge(lv_obj_t * o, const char * icon, lv_color_t c);         /* NULL = none */
void orbi_celebrate(lv_obj_t * o);  void orbi_shake(lv_obj_t * o);
```

- **Object.** A plain `lv_obj` sized to the body bbox, with `LV_OBJ_FLAG_ADV_HITTEST`.
  - `LV_EVENT_HIT_TEST`: hit when the distance to the centre ≤ R + 12.
  - `LV_EVENT_REFR_EXT_DRAW_SIZE`: `lv_event_set_ext_draw_size(e, 64)` (halo, bars, dots, badge).
  - `LV_EVENT_DRAW_MAIN`: draws everything in §2.1 order with `lv_draw_rect`, `lv_draw_arc`, `lv_draw_line` (round caps) and `lv_draw_triangle` on `lv_event_get_layer(e)`.
  - `GESTURE_BUBBLE` cleared.
  - No child objects (one object = one draw pass).
- **State.** A parameter struct: geometry, expression from/to + blend 0–1024, gaze, level, progress, dots phase, bar lengths[32], squash, body dx/dy, colours.
  - One-shot changes use `lv_anim` with `lv_anim_set_custom_exec_cb` and `LV_ANIM_SET_EASE_*` bezier presets.
  - Loops use one `lv_timer` (33 ms).
- **Precise invalidation.** `orbi_invalidate(o, ORBI_EYES | ORBI_MOUTH | ORBI_BODY | ORBI_HALO | ORBI_BARS | ORBI_DOTS | ORBI_ARC_TIP | ORBI_BADGE)` computes the union of old and new boxes for each part and calls `lv_obj_invalidate_area(o, &abs_area)`. Never call `lv_obj_invalidate(o)` from loops.
- Geometry animations call `lv_obj_set_pos/size`. LVGL invalidates old and new coordinates (+ ext draw) itself.

### 8.4 Shared styles (static `lv_style_t`, created once in `ui_theme_init`)

| Group | Styles |
|---|---|
| Text | `st_scr`, `st_txt_cap`, `st_txt_body`, `st_txt_title`, `st_txt_disp` |
| Pills | `st_pill`, `st_pill_primary`, `st_pill_danger`, `st_pressed` (+ one `lv_style_transition_dsc_t`, 80 ms, props `{BG_COLOR, 0}`) |
| Rows | `st_row`, `st_row_info`, `st_row_icon` |
| Bubbles | `st_bubble_ai`, `st_bubble_user`, `st_bubble_tr_a`, `st_bubble_tr_b`, `st_sys_line` |
| Satellites / chips | `st_sat`, `st_chip_a`, `st_chip_b` |
| Keyboard | `st_kb_main`, `st_kb_items`, `st_kb_items_fn`, `st_kb_items_pressed`, `st_kb_items_checked` |
| Text field | `st_ta`, `st_ta_focus`, `st_ta_cursor` |
| Arc | `st_arc_main`, `st_arc_ind`, `st_arc_knob` |
| Misc | `st_card_qr`, `st_fade_top`, `st_fade_bot` |

Attach styles with selectors (`LV_PART_ITEMS | LV_STATE_PRESSED`, etc.). Do **not** use local `lv_obj_set_style_*` on list rows or bubbles. Only per-instance values are local: bubble stripe colour, row opacity, translate.

### 8.5 State: subjects behind a 9.6-ready wrapper

| Subject | Type | Values |
|---|---|---|
| `ui_s_mode` | int | ASSIST / TRANSL |
| `ui_s_agent` | int | IDLE, LISTEN_PTT, LISTEN_LATCH, CANCEL_ZONE, UPLOADING, THINKING, SPEAKING, ERROR, OFFLINE, DOZE |
| `ui_s_layout` | int | STAGE / CHAT / ERRORCARD |
| `ui_s_mood` | int | mood enum |
| `ui_s_voice_out`, `ui_s_speaker` | int | 0 / 1 |
| `ui_s_wifi` | int | OFF / SCANNING / CONNECTING / ONLINE / FAILED |
| `ui_s_rssi` | int | dBm |
| `ui_s_err` | int | error code |
| `ui_s_volume` | int | 0–100 |
| `ui_s_bright` | int | 5–100 |
| `ui_s_ui_lang` | int | RU / EN |
| `ui_s_tr_a`, `ui_s_tr_b` | int | language ids |
| `ui_s_batt` | int | −1 = USB, else 0–100 |
| `ui_s_clock` | string | "HH:MM", 8 B |

```c
/* ui_state.h — the only place that knows the LVGL subject API version */
lv_subject_t * ui_subject_int(int32_t init);
lv_subject_t * ui_subject_str(size_t cap, const char * init);
void ui_set(lv_subject_t * s, int32_t v);            /* LVGL task only */
lv_observer_t * ui_bind_hidden_unless(lv_obj_t * o, lv_subject_t * s, int32_t v);
lv_observer_t * ui_bind_cb(lv_subject_t * s, lv_obj_t * o, lv_observer_cb_t cb);
/* 9.5: static lv_subject_t pool[24]; lv_subject_init_int(&pool[i], init); return &pool[i];
   9.6: s = lv_subject_create(LV_SUBJECT_TYPE_INT); (set min/max first) lv_subject_set_int(s, init);
   Bindings are implemented with lv_subject_add_observer_obj() (present in both versions);
   lv_obj_bind_flag_if_* (deprecated in 9.6) is never called directly.
   lv_arc_bind_value / lv_label_bind_text are used as-is. */
```

### 8.6 Threading rule (strict)

1. **Only the LVGL task** (esp_lvgl_port, core 1) touches LVGL objects, styles, animations, timers or subjects.
2. Audio, network, Wi-Fi event and button tasks (core 0) **never block on LVGL**.
   - They post a `ui_evt_t {type, i32, ptr}` with `xQueueSend(q, &e, 0)`. The queue depth is 32; overflows are counted.
   - An `lv_timer` pump (20 ms) drains up to 16 events per tick and applies them (`ui_set`, `orbi_*`, `chat_*`).
3. Hot telemetry (mic RMS, TTS RMS, upload bytes) is in `atomic_uint`, read by the face tick.
4. Streamed reply text goes into a PSRAM ring buffer behind a mutex (held < 1 ms). The pump moves ≤ 64 B per 66 ms, cut at UTF-8 boundaries.
5. `lvgl_port_lock(0)` / `lvgl_port_unlock()` only for initialisation before the UI starts and for dev-console commands. Never in the audio path.
6. Never call LVGL from an ISR. Buttons use a GPIO ISR → task notification → queue.

### 8.7 Memory per screen (LVGL heap in PSRAM unless noted)

| Item | Estimate | Lifetime |
|---|---|---|
| Draw buffers 2 × 360×40 RGB565 | **57.6 KB internal DMA** | permanent |
| LVGL task stack | 10 KB internal | permanent |
| Fonts | ≈150–180 KB flash rodata | permanent |
| Main screen: frame + Orbi + ≤ 30 bubbles (≈110 objects × ~150 B + ≈12 KB text) | ≈30 KB | permanent |
| Settings (13 rows × 4 objects) | ≈9 KB | permanent |
| Orbit-list sub-screens: Wi-Fi ≤ 20 rows | ≈14 KB | created on entry, deleted on exit |
| Models ≤ 40 rows (+ ≈100 KB `/models` JSON, freed after parsing) | ≈28 KB | on entry / exit |
| Voices | ≈20 KB | on entry / exit |
| Languages | ≈12 KB | on entry / exit |
| History | ≈15 KB | on entry / exit |
| Session view | ≈30 KB | on entry / exit |
| Diagnostics | ≈14 KB | on entry / exit |
| Keyboard screen (22 buttons ≈ 0.3 KB, textarea ≈ 1.5 KB, petals 6 objects) | ≈5 KB | on entry / exit |
| Phone setup (QR I1 buffer 3.7 KB + labels) | ≈6 KB | on entry / exit |
| Dial | ≈2 KB | on entry / exit |
| Mode Ring | ≈4 KB | on entry / exit |
| Confirm / Power | ≈2 KB | on entry / exit |
| Recording buffer (30 s) / PCM ring | 960 KB / 64–144 KB (non-LVGL) | per turn |
| `lv_snapshot` for the dev console | 259.2 KB | transient |

Peak UI heap: main + settings + one list + keyboard ≈ 75 KB (+ 100 KB JSON transient). Internal SRAM used by the UI is only the draw buffers and the task stack.

### 8.8 Test hooks

- Every coordinate in §4–§5 can be scripted through the dev-console touch injection. Examples:
  - `touch 180 164 hold 1500` = PTT in STAGE;
  - `touch 152 126 drag -40 0` = flick «б».
- `lv_snapshot` PNGs give visual regression checks.
- Debug builds draw the 168 px and 180 px circles as an overlay on `lv_layer_sys()` (off by default).
