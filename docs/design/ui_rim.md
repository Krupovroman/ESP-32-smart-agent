# Round AI Assistant: UI/UX specification, "Rim-Centric" concept

Target: Waveshare ESP32-S3-Touch-LCD-1.85 rev 2.1. The screen is a 360×360 round IPS panel, RGB565, ST77916 over QSPI. Touch is a CST816T with a single finger. Software is LVGL 9.5.x on esp_lvgl_port 2.9, rendering in PARTIAL mode with 2 × 360×40 internal-SRAM DMA buffers (28.8 KB each). Physical buttons are BOOT (GPIO0) and PWR (GPIO6). The UI is in Russian with English as the second language.

Sources: `docs/requirements.md`, `docs/hardware.md`, `docs/research/stack_2026-10-17.json` (the `verify:` corrections are applied throughout), and LVGL 9.5 sources (`lv_arclabel`, `lv_buttonmatrix`, `lv_keyboard`, `lv_qrcode`, `lv_observer`, `lv_indev`, `lv_translation`, `lv_conf_template.h`) read on 2026-10-17.

---

## 0. Concept in one paragraph

The circle's edge is the main interface, not a margin to stay away from. The outer ring of the panel (r ≥ 118 px from the centre) is 57 % of the panel's area. Rectangular layouts waste it, so this design gives it real jobs:

- **Mode Ring.** A radial mode switcher opens when you touch or slide along the rim, or press PWR.
- **Caps.** The top cap of every sub-screen is "back + title". The bottom cap is the screen's one primary action.
- **Rim text.** Status, hints and titles are drawn along the edge with `lv_arclabel`.
- **Activity ring.** A thin arc at r=166 shows recording time, upload progress and playback.
- **Translator poles.** The two languages sit on opposite rims (west = me, east = partner).
- **Ring keyboard.** Letters live on the rim and the controls live in the centre.

The centre (r < 116) holds content: the orb, the reply text, lists and the text field. A touch that **starts** on the rim and a touch that starts in the centre are routed differently, so rim gestures never collide with chat scrolling or hold-to-talk.

### Coordinate conventions used everywhere

- The screen origin is the top-left corner (0,0). The centre is C = (180,180).
- Angles θ follow LVGL: 0° = 3 o'clock, increasing **clockwise**. So 90° = 6 o'clock, 180° = 9 o'clock and 270° = 12 o'clock. `lv_arc` and `lv_arclabel` use the same convention, since they use cos/sin in screen coordinates.
- Safe radius R = 168 (12 px rim margin). Nothing important goes past r = 168. Touch zones may extend to the physical edge, r = 180.
- Usable chord at vertical offset d is `w(d) = 2·sqrt(168² − d²)`: d=0 → 336, 30 → 330.6, 50 → 320.8, 64 → 310.7, 80 → 295.5, 100 → 270.0, 120 → 235.2, 140 → 185.7, 150 → 151.3, 160 → 102.4.
- A rectangle is "inside" when all four corners satisfy `x_off² + d² ≤ 168²`. Every geometry below was checked this way, and the corner radius r is given next to each one.
- Touch target minimum: 50 px in both dimensions. Ring sectors are measured at their visual midline.

---

## 1. Visual language

### 1.1 Palette (dark; hex values are sRGB, the panel quantises to RGB565)

| Token | Hex | Use |
|---|---|---|
| `bg` | `#0A0D12` | Screen background, always solid. No full-screen gradients or images. |
| `surface1` | `#131922` | Pills, list items, bubbles (assistant) |
| `surface2` | `#1B2330` | Keys and ring sectors (normal) |
| `surface3` | `#263142` | Pressed keys, arc tracks |
| `outline` | `#2F3B4E` | 1–2 px borders, dividers |
| `text1` | `#EEF2F7` | Primary text (≈17:1 on bg) |
| `text2` | `#9AA6B6` | Secondary text (≈8:1) |
| `text3` | `#5E6A7A` | Disabled text and hints (≈3.5:1, never for essential info) |
| `user_bubble` | `#1D3557` | User bubbles and the user transcript chip |
| `primary` | `#4C8DFF` | Primary action caps (text on it: `bg`) |

State accents. The orb core is a **vertical** gradient (cheap "simple" gradient) running from the top colour to the bottom colour.

| State | Accent | Orb gradient top → bottom | Rim activity ring |
|---|---|---|---|
| idle | `#4C8DFF` | `#7FB0FF` → `#2F5BD9` | hidden |
| listening | `#22D3EE` | `#67E8F9` → `#0E7490` | elapsed time, cyan |
| uploading | `#A78BFA` | `#C4B5FD` → `#6D28D9` | upload %, violet |
| thinking | `#A78BFA` | `#C4B5FD` → `#6D28D9` | hidden (orb arcs instead) |
| speaking | `#34D399` | `#6EE7B7` → `#047857` | playback progress, green |
| error | `#F87171` | `#FCA5A5` → `#B91C1C` | one red 360° flash |
| warning | `#FBBF24` | (orb idle colours + amber halo) | amber |
| cancel-armed | `#F87171` | unchanged | red |

- Translator sides: language A (me, west pole) `#22D3EE` cyan, language B (partner, east pole) `#F59E0B` amber. This pair stays distinguishable under deuteranopia.
- RGB565 bands long smooth gradients, and LVGL 9 has no dithering. Gradients are therefore used only on small objects: the orb core (Ø ≤ 112) and the preview bubble.

### 1.2 Typography

**Font: Inter v4.1 (SIL OFL 1.1, © The Inter Project Authors).** It covers Latin, Latin Extended, Cyrillic including Ukrainian, and Vietnamese, and it reads better than Montserrat at 14–18 px on a 1.85" panel. Use the static TTFs from `Inter-4.1.zip` (Regular and SemiBold; check the path inside the zip). Keep `OFL.txt` in `main/ui/fonts/`.

Icons: FontAwesome 5 Free from LVGL's own `scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff`, the same file `built_in_font_gen.py` uses, merged into the text fonts.

Glyph sets:

- **FULL** (any text an LLM can return in a supported language): `0x20-0x7E, 0xA0-0x17F, 0x1A0-0x1A1, 0x1AF-0x1B0, 0x218-0x21B, 0x400-0x45F, 0x490-0x491, 0x1EA0-0x1EF9, 0x2010-0x2027, 0x2030, 0x2039-0x203A, 0x20AC, 0x20BD, 0x2116, 0x2122, 0x2190-0x2193, 0x2212`. This covers Latin-1 and Ext-A (pl, cs, hu, tr, ro …), RO ș/ț, Cyrillic plus Ukrainian ґ, Vietnamese, punctuation (— – “ ” « » … ‰), € ₽ № ™, arrows and the minus sign.
- **UI**: `0x20-0x7E, 0xB0, 0xAB, 0xBB, 0x401, 0x410-0x44F, 0x451, 0x2013-0x2014, 0x2026, 0x2116`.
- **KEYS**: `0x20-0x7E, 0x401, 0x410-0x44F, 0x451, 0x2116, 0x20BD`.
- **ICONS** (FA5 codepoints): `0xF00C ok, 0xF00D close, 0xF011 power, 0xF013 cog, 0xF021 sync, 0xF026 mute, 0xF027 vol-mid, 0xF028 vol-max, 0xF029 qrcode, 0xF04D stop, 0xF053 chevron-left, 0xF054 chevron-right, 0xF05A info, 0xF062 arrow-up(shift), 0xF06E eye, 0xF070 eye-slash, 0xF071 warning, 0xF084 key, 0xF086 comments, 0xF0AC globe, 0xF0E7 bolt, 0xF11C keyboard, 0xF130 microphone, 0xF131 mic-slash, 0xF185 sun, 0xF1AB language, 0xF1DA history, 0xF1EB wifi, 0xF240-0xF244 battery, 0xF2ED trash, 0xF362 exchange-alt, 0xF3CD mobile-alt, 0xF544 robot, 0xF555 wallet, 0xF55A backspace, 0xF6A9 volume-mute`. lv_font_conv warns about missing glyphs; F544, F555 and F6A9 are FA ≥ 5.0.13 / 5.3, so confirm they are present in LVGL's woff and drop any that are missing.

| Font | Weight | Size | Glyph set | Used for | Est. flash |
|---|---|---|---|---|---|
| `font_ui14` | Regular | 14 | UI + ICONS | Rim arclabels, captions, values | ≈ 12 KB |
| `font_t16` | Regular | 16 | FULL + ICONS | User transcript, list subtitles, history | ≈ 32 KB |
| `font_t18` | Regular | 18 | FULL + ICONS | Reply text, bubbles, text field | ≈ 40 KB |
| `font_t20` | SemiBold | 20 | FULL + ICONS | List titles, translation text, headings | ≈ 47 KB |
| `font_k24` | SemiBold | 24 | KEYS + ICONS | Ring-keyboard letters, pole codes, core keys | ≈ 21 KB |
| `font_k36` | SemiBold | 36 | KEYS | Key-preview bubble, big values (65 %) | ≈ 36 KB |
| `font_fa32` | — | 32 | ICONS only | Mode Ring icons, beads | ≈ 7 KB |

Total is about 195 KB of flash `rodata`. RAM cost is zero because fonts are `const`. Example command:

```
lv_font_conv --no-compress --no-prefilter --force-fast-kern-format --bpp 4 --size 18 --format lvgl \
  --font Inter-Regular.ttf -r 0x20-0x7E,0xA0-0x17F,0x1A0-0x1A1,0x1AF-0x1B0,0x218-0x21B,0x400-0x45F,0x490-0x491,0x1EA0-0x1EF9,0x2010-0x2027,0x2030,0x2039-0x203A,0x20AC,0x20BD,0x2116,0x2122,0x2190-0x2193,0x2212 \
  --font FontAwesome5-Solid+Brands+Regular.woff -r 0xF00C,0xF00D,0xF011,0xF013,0xF021,0xF026-0xF029,0xF04D,0xF053,0xF054,0xF05A,0xF062,0xF06E,0xF070,0xF071,0xF084,0xF086,0xF0AC,0xF0E7,0xF11C,0xF130,0xF131,0xF185,0xF1AB,0xF1DA,0xF1EB,0xF240-0xF244,0xF2ED,0xF362,0xF3CD,0xF544,0xF555,0xF55A,0xF6A9 \
  --lv-font-name font_t18 -o font_t18.c
```

- `--lv-fallback font_t16` can chain a fallback where it helps. `LV_FONT_FMT_TXT_LARGE` stays 0, because each font is under 1 MB.
- Line heights: 14 → 18, 16 → 20, 18 → 24, 20 → 26, 24 → 30 px.
- **Text sanitiser.** All model text passes through it before it reaches a label. It does five things:
  1. Strips markdown (`**`, `__`, `#`, backticks, `- ` list markers become `• ` U+2022, which is added to FULL).
  2. Maps emoji and unsupported codepoints (U+1F000+, U+2600–27BF, and anything outside the font) to nothing, or to "·" if a run would vanish entirely.
  3. Collapses runs of 3 or more newlines.
  4. Hard-limits each message to 1200 characters on screen (the full text is kept for TTS).
  5. Renders non-Latin/Cyrillic targets (zh, ja, ko, ar, hi, th) as romanisation (§5.6).

### 1.3 Icons and shapes

- **Icons are font glyphs, never images.** That means no image decoder, no image cache and recolouring for free.
- **Shapes are drawn with LVGL primitives:**
  - circles: `radius LV_RADIUS_CIRCLE`
  - arcs and annular sectors: `lv_arc` or `lv_draw_arc`
  - rounded pills: radius 26
- **Shadows:** none (`shadow_width 0` everywhere).
- **Borders:** 1–2 px `outline` / accent.

---

## 2. Screen inventory and navigation

### 2.1 Inventory

| ID | Screen | Lifetime | Rim policy |
|---|---|---|---|
| S0 | Boot splash | 1.2 s | off |
| S1 | Onboarding O1–O6 | first boot / factory reset | off |
| S2 | **Assistant** (orb + current exchange) | persistent | **RING** |
| S3 | **History** (all turns) | created on open | **RING** |
| S4 | **Translator** (poles + orb + translation) | persistent | **RING** with pole exclusions |
| S4a | Translator language picker | modal | off |
| S5 | Settings (lens list) | created on open | off (cap back) |
| S5a | Wi-Fi scan list | on open | off |
| S5b | Text entry (ring keyboard) | modal | off |
| S5c | AI model picker | on open | off |
| S5d | TTS model → voice picker | on open | off |
| S5e/f | Volume dial / Brightness dial | on open | off |
| S5g | Phone setup (SoftAP + QR) | on open | off |
| S5h | Provider endpoints (chat/STT/TTS: base URL, key, model) | on open | off |
| S5i | Diagnostics / About | on open | off |
| S5j | Balance (`GET /api/v1/key`) | on open | off |
| OV-MR | Mode Ring overlay | on `lv_layer_top()` | — |
| OV-ERR | Error card (inline in S2/S4) | inline | — |
| OV-PWR | Power menu | overlay | — |
| OV-CONF | Hold-to-confirm | inline cap behaviour | — |

### 2.2 Navigation map

```
S0 → (first boot) S1:O1 lang → O2 setup method ─┬─ phone → S5g → O4
                                                └─ device → S5a → S5b(pwd) → S5b(key) → O4
     O4 sound/mic check → O5 how-to → S2
S2 Assistant ⇄ S4 Translator ⇄ S3 History ⇄ S5 Settings    (via Mode Ring, any to any)
S5 → S5a/S5c/S5d/S5e/S5f/S5g/S5h/S5i/S5j → (S5b when text is needed)
S4 → S4a (tap a pole)            S2/S4 → S5b (Mode Ring "Ввод текста")
Back from any sub-screen: top cap tap | swipe right | PWR short → parent
```

### 2.3 How the user moves between modes

**Mode Ring (OV-MR)** is a radial switcher with 6 segments of 60° each. Every segment is an annular sector r 112–168, drawn as 56° with a 2° gap each side.

| Segment | Centre θ | Icon | Label (RU / EN) | Action |
|---|---|---|---|---|
| Assistant | 270° (top) | F544 robot | Ассистент / Assistant | go to S2 |
| Translator | 330° | F1AB language | Переводчик / Translator | go to S4 |
| History | 30° | F1DA history | История / History | go to S3 |
| Type | 90° (bottom) | F11C keyboard | Ввод текста / Type | open S5b for the current mode |
| Settings | 150° | F013 cog | Настройки / Settings | go to S5 |
| Voice | 210° | F028 / F6A9 | Голос / Только текст | toggle voice replies in place; ring stays open 800 ms to show the new state |

- **Icon placement:** `font_fa32` icon at r = 146 on the segment's centre angle.
- **Label placement:** a straight (unrotated) `font_ui14` label centred at r = 121. Example: Translator label at (284.8,119.5), 80 px wide, far corner r = 155.6 ✓.
- **Centre disc:** Ø 216, `bg` at opa 235. It shows the highlighted segment name (`font_t20`), a one-line description (`font_ui14`, e.g. "RU ⇄ EN · авто") and the hint "Отпустите — открыть" / "Release to open".

**Ways to open it, on top-level screens only (S2, S3, S4):**

1. **Rim tap.** Press and release in the rim band (r ≥ 150) within 400 ms and moving less than 10 px. The ring opens in *sticky* mode: tap a segment to choose, tap the centre disc to close, or wait 5 s for auto-close.
2. **Rim slide ("scrub").** Press in the rim band and move ≥ 12° around the circumference. The ring opens immediately and highlights the segment under the finger: `seg = floor(((θ − 240) mod 360) / 60)`. Releasing on a segment commits. Releasing with r < 100 (finger pulled into the centre) cancels.
3. **PWR short press** toggles the sticky ring.

**Back** on sub-screens works three ways:
- the top cap (y < 60) is a single large back target that also shows the title ("‹ Wi-Fi")
- a horizontal swipe right (`LV_EVENT_GESTURE`, `LV_DIR_RIGHT`)
- PWR short press

**BOOT is the universal talk key.** From any screen except S5b, pressing BOOT switches to the last conversational mode (S2 or S4) and starts listening. In S5b it dictates into the field; dictation is disabled for password and key fields.

### 2.4 Why nothing conflicts

| Gesture | Where it starts | Owner | Conflict prevention |
|---|---|---|---|
| Hold/tap to talk | Orb (r < 60 stage, docked orb Ø 56) | `ui_orb` | Starts inside the centre, so the rim catcher never sees it. Dragging out of the orb only arms *cancel*; it never scrolls. |
| Chat/history scroll | Content area (r < 140) | scroll container, `scroll_dir = VER` | Starts in the centre, so it scrolls. Rim band presses never reach it. |
| Mode Ring | r ≥ 150 | rim catcher on `lv_layer_top()` with `LV_OBJ_FLAG_ADV_HITTEST` | The hit test accepts only r ≥ 150 minus per-screen exclusions. Content in S2/S4 stays inside r ≤ 158 but is laid out so interactive parts end by r ≤ 140. The 10 px gap is a dead zone. |
| Translator poles | sectors θ 150–210 / 330–30 | pole objects | Listed as exclusions in the rim catcher hit test for S4. |
| Back swipe | content of sub-screens | screen `LV_EVENT_GESTURE` | Lists scroll only vertically, so a horizontal fling becomes a gesture (LVGL default min distance 50 px, min velocity 3). |
| Keyboard | ring + core | `ui_ringkb`, `lv_buttonmatrix` | Rim catcher disabled (`rim_policy = OFF`). |

Rim catcher hit test (LVGL 9.5 `LV_EVENT_HIT_TEST`):

```c
static void rim_hit_cb(lv_event_t *e) {
    lv_hit_test_info_t *hi = lv_event_get_param(e);
    int32_t dx = hi->point->x - 180, dy = hi->point->y - 180;
    int32_t r2 = dx*dx + dy*dy;
    if (g_rim_policy != RIM_RING || r2 < 150*150) { hi->res = false; return; }
    float th = atan2f((float)dy, (float)dx) * 57.2958f; if (th < 0) th += 360.f;
    hi->res = !rim_excluded(th);          /* S4: 150..210, 330..30 excluded */
}
/* catcher: lv_obj on lv_layer_top(), 360x360, bg_opa 0, LV_OBJ_FLAG_ADV_HITTEST|CLICKABLE,
   events: HIT_TEST, PRESSED, PRESSING (angle tracking), RELEASED, PRESS_LOST */
```

---

## 3. Shared rim chrome and exact geometry

### 3.1 Rim elements (reused on every screen)

| Element | Implementation | Geometry |
|---|---|---|
| **North status / title arclabel** | `lv_arclabel`, `font_ui14` (title: `font_t16` SemiBold-equivalent `font_t20` not needed; use `font_ui14` in `text2`; titles in `text1`), `LV_ARCLABEL_DIR_CLOCKWISE`, angle_start 225, angle_size 90, horizontal align CENTER, overflow ELLIPSIS, recolor on | Object at x 60–300, y 14–46 (240×32). Centre = content centre (180,30) + `center_offset_y 150` gives (180,180). Radius 152. Glyph tops face outward. |
| **South hint arclabel** | `lv_arclabel`, `font_ui14` `text2`, `DIR_COUNTER_CLOCKWISE`, angle_start 135, angle_size 90, CENTER | The centre offset is unsigned (positive only), so the object must contain C. Use a narrow full-height object at x 96–264, y 12–348 (centre (180,180), offset 0). Radius 150. Glyph tops face the centre, so the text reads upright. *Optimisation to try in the simulator:* object y 296–348 with `pad_top = −284` to move the content centre to y = 180. |
| **Activity ring** | `lv_arc`, 336×336 at (12,12), MAIN bg_opa 0, INDICATOR arc_width 4, arc_rounded 1, rotation 270, bg_angles 0–360, range 0–1000, knob removed, not clickable | Radius 168 (band 164–168). Value updates invalidate only the changed angular span. |
| **Top cap (back)** | Transparent `lv_obj` x 66–294, y 12–58 (hit test r ≤ 180) + the north arclabel showing "‹ Title" | Chord at y = 58 (d = 122) is 231 px, so the target is about 228×46 at its narrowest. Plenty. |
| **Bottom cap (primary action)** | `lv_arc` 336×336, bg_angles 55–125, arc_width 44, MAIN colour `primary` (or `surface2` when disabled), not rotated, knob removed; label = CCW arclabel radius 146, `font_t16` in `bg` colour. Hit test: r 118–180 and θ 55–125. | Band r 124–168. The bounding box of the clickable part is x 84–276, y 282–348. |
| **Scroll indicator** | `lv_arc` 320×320 centred, bg_angles 330–30 (east rim), MAIN `surface3` width 3, INDICATOR `text2` 12° segment moved by scroll ratio | Radius 160. Hidden unless the content overflows. |
| **Bead** (round button) | `lv_button`, radius CIRCLE, Ø 52–56, `surface2`, pressed `surface3`, icon `font_fa32` or `font_k24` | Centre at polar (ρ, θ) with ρ + radius ≤ 166. |

> Arclabel cost note (verified in `lv_arclabel.c` 9.5): each glyph is drawn with its own rotation through `lv_draw_letter`, with no caching. **Rule:** arclabels are static. They change at most when the state changes and are never inside a continuously animated dirty area. That is why the thinking animation uses orb arcs and not a rim comet passing under the status text.

### 3.2 S2 Assistant: two layouts

**Layout A, "Stage"** (idle with no reply, listening, uploading, thinking):

| Object | Geometry | Notes |
|---|---|---|
| North arclabel | as 3.1 | Idle: `14:32 · F1EB · F028` (time, Wi-Fi, voice on/off; icon recoloured red/amber on problems). During a state it shows the caption: "Слушаю…", "Отправка 42 %", "Думаю…". |
| Orb | `ui_orb` box 216×216 at (72,72), centre C | Core rc = 56 idle / 50 listening / 44 uploading / 46 thinking. Bars r 62–100. |
| South hint | as 3.1 | Idle: "Удерживайте шар или BOOT" / "Hold the orb or BOOT". Hidden while listening latched. |
| Cancel bead | Ø 52 at (180,312) (ρ 132, outer r 158 ✓) | Visible only in *latched* listening. Icon F00D. |
| Activity ring | r 168 | Listening: elapsed / 30 s. Uploading: bytes sent / total. |

**Layout B, "Reading"** (speaking, text reveal, idle after a reply):

| Object | Geometry | Corner check |
|---|---|---|
| North arclabel | as 3.1 | — |
| User line | `lv_label` `font_t16` `text2`, prefix "Вы: ", long mode DOTS, max 2 lines; x 70–290, y 58–98 | (110,122) → r 164.4 ✓ |
| Reply | scroll container x 44–316, y 104–252 (272×148); `lv_label` `font_t18` `text1`, WRAP, line 24 px (6 lines visible) | (136,76) → 155.8 ✓; (136,72) → 153.9 ✓ |
| Docked orb | `ui_orb` box 100×100 centred (180,302), rc 24, bars r 28–44 | ρ 122 + 44 = 166 ✓ |
| Scroll indicator | east rim, shown if the reply overflows | — |

The reply reads as plain text on `bg` (no bubble), so it gets the full chord width. Tapping the reply opens S3 scrolled to this turn.

### 3.3 S3 History: fade-band list

- **Container:** scroll container x 44–316, y 56–304, `scroll_dir VER`, momentum on, no snap, scrollbar off (rim indicator instead).
- **Bubbles:**
  - User bubbles are `user_bubble` at x 60–316 (offset right 16 px, width ≤ 256).
  - Assistant bubbles are `surface1` at x 44–300.
  - Style: radius 18, pad 10/12, `font_t18`, row gap 8 px, timestamp-less.
- **Readable band** is y 82–278. There a 272-wide bubble fits, because `sqrt(168² − 136²) = 98.6`.
- **Fading:** in `LV_EVENT_SCROLL`, each visible bubble gets `bg_opa` and `text_opa` from its distance outside the band: 255 inside, falling linearly to 0 over 40 px. These are draw-descriptor opacities, **not** `style_opa`, so no layer is created.
- **North cap:** "‹ История". **South cap:** "Новый диалог" (hold-to-confirm, §5.8).
- **Tap actions:** tapping an assistant bubble replays it (TTS on), which needs a speaker. Tapping a user bubble does nothing.
- **Mode Ring:** rim tap/slide works here too (rim policy RING).

### 3.4 S4 Translator

| Object | Geometry |
|---|---|
| West pole (language A) | Custom `lv_obj` with bounding box x 12–78, y 96–264, hit test = sector θ 150–210, r 118–180. Drawn as `lv_draw_arc` radius 168, width 50, angles 151–209, A colour at opa 60 (idle) / 255 (active). Code label "RU" `font_k24` at (37,180) (r 143). |
| East pole (language B) | Bounding box x 282–348, y 96–264, sector θ 330–30. Code "EN" at (323,180). |
| North arclabel | "Русский ⇄ English · авто" |
| Orb (stage) | Box 200×200 centred C, rc 50, bars r 58–96. The inner edge of the poles is at x = 62 (r 118 at θ 180) and the bars reach x = 84, leaving a 22 px gap. |
| Result: source transcript | `font_t16` `text2`, 2 lines, x 76–284, y 62–102 ((104,118) → 157.3 ✓) |
| Result: direction chip | `font_ui14` "RU → EN", centred y 106–122, text in the target-side colour |
| Result: translation | `font_t20` `text1`, x 82–278, y 128–232 (4 lines of 26). Corner (−98,52): θ 152°… check against the pole: (98,52) → r 110.9 < 118 ✓ (outside the pole). |
| Docked orb | (180,302) as in S2 |
| Flow comet | `lv_arc` 320×320 centred, width 4, 30° segment, animates from the speaker's pole to the target pole through the south rim |
| Partner view (option) | Translation container gets `transform_rotation 1800` when the target is B, so a person across the table can read it. One-time layer of 196×104 ≈ 20 k px ARGB8888 ≈ 80 KB in the PSRAM heap, re-rendered only when the text changes. Off by default. |

**S4a language picker:**
- Lens list (§3.5) of a curated list: ru, en, de, fr, es, it, pt, pl, tr, uk, nl, sv, cs, hu, ro, vi, id, zh, ja, ko, ar, hi, th.
- Names are shown in the **UI language** (e.g. "Китайский"), so no CJK or Arabic glyphs are needed.
- Each item shows a voice badge: F028 if the current TTS model has a voice for that locale (from `supported_voices`), F6A9 if text only. Languages with non-Latin/Cyrillic scripts show "Aa латиница" (romanised on screen).
- North cap "‹ Язык собеседника" (or "Мой язык"). Bottom cap: "⇄ Поменять местами" (swap A/B).

### 3.5 Lens list (S5, S5a, S5c, S5d, S4a, S5i)

- **Container:** x 24–336, y 58–302, `scroll_dir VER`, `scroll_snap_y CENTER`, scrollbar off.
- **Items:** pill `surface1`, radius 26, height 52, row pitch 58, pad 12/16, `LV_FLEX_FLOW_ROW` with three parts:
  - icon (`font_fa32` at 24 px glyph box, `text2`)
  - title (`font_t20`, DOTS)
  - value (`font_ui14` `text2`, DOTS)
- **Top padding** 122 and bottom padding 122, so the first and last items can snap to the centre.
- **In `LV_EVENT_SCROLL` (lens effect):** for each child, compute its centre offset `dy` from y = 180.

```c
int32_t a = LV_ABS(dy) + 26;                       /* farthest pill edge */
int32_t w = (a < 168) ? 2*lv_sqrt32(168*168 - a*a) - 16 : 0;
w = LV_CLAMP(120, w, 296);                         /* dy=0→296, 58→275, 116→163 */
lv_obj_set_width(child, w);                        /* labels are single-line DOTS, no rewrap cost */
lv_opa_t o = LV_ABS(dy) <= 70 ? 255 : LV_MAX(40, 255 - (LV_ABS(dy)-70)*215/60);
lv_obj_set_style_bg_opa(child, o * 255 / 255, 0); lv_obj_set_style_text_opa(child, o, 0);
```

- **Focused item:** the child nearest the centre (|dy| < 29) gets the `LV_STATE_FOCUSED` style, a 2 px accent outline. With these widths the pill silhouette is a lens that follows the circle.
- **Taps:** tap = select or open. Back = top cap, swipe right or PWR.

Settings (S5) items, in order:

| # | Icon | Title (RU / EN) | Value |
|---|---|---|---|
| 1 | F1EB | Wi-Fi | current SSID |
| 2 | F029 | Настройка с телефона / Phone setup | — |
| 3 | F544 | Модель ИИ / AI model | gemini-3.1-flash-lite |
| 4 | F084 | Ключи и адреса / Keys & endpoints | openrouter.ai · sk-or-…a1f3 |
| 5 | F028 | Голос / Voice | Masha · MAI Flash |
| 6 | F130 | Распознавание / Speech input | в модели / Whisper |
| 7 | F027 | Громкость / Volume | 65 % |
| 8 | F185 | Яркость / Brightness | 80 % |
| 9 | F0AC | Язык интерфейса / UI language | Русский |
| 10 | F6A9 | Динамик подключён / Speaker connected | нет / да |
| 11 | F011 | Экран / Screen timeout | 60 с / 5 мин |
| 12 | F555 | Баланс / Balance | $4.21 |
| 13 | F05A | Диагностика / Diagnostics | v0.3.1 |

Other lens lists:

- **Wi-Fi (S5a):**
  - Item: SSID (`font_t20`) + "WPA2 · −58 dBm" + F1EB coloured by RSSI (≥ −60 `#34D399`, −60…−75 `#FBBF24`, < −75 `#F87171`). F023 lock is not in ICONS; show "WPA2" text instead.
  - Last item: "Другая сеть…" (hidden SSID).
  - Bottom cap: "↻ Обновить" (rescan, with a comet on the activity ring while scanning).
- **Model (S5c):**
  - Curated built-in list plus at most 12 live entries from `GET /api/v1/models?input_modalities=audio&output_modalities=text&sort=pricing-low-to-high`.
  - Live entries are filtered: drop `:batch`, prices < 0 (catches `openrouter/*` and `typesafe/jev-router`), a non-null `expiration_date`, `-preview` duplicates and `~` aliases. Keep only entries whose `supported_parameters` contain both `response_format` and `structured_outputs`.
  - Value line: "$0.25 / $1.50 · аудио $0.50".
  - Last item "Свой ID — с телефона" opens S5g.
  - No `lv_roller` (INFINITE mode duplicates the options).
- **TTS (S5d):**
  - Step 1 is the model: `microsoft/mai-voice-2.1-flash`, `google/gemini-3.8-flash-lite-tts`, …
  - Step 2 is the voice, filtered by the UI and translator locales: "Masha · ru-RU" with the full id `ru-RU-Masha:MAI-Voice-2.1-Flash`.
  - Bottom cap "▶ Прослушать" synthesises "Привет! Я ваш ассистент." It is disabled without a speaker, and its label warns that it is a paid call.

### 3.6 S5e/S5f Volume and Brightness dial

| Object | Geometry |
|---|---|
| Dial | `lv_arc` 336×336 at (12,12), bg_angles 120–60 (300° sweep, gap at the bottom), range 0–100 (brightness 5–100), MAIN arc_width 14 `surface3`, INDICATOR 14 accent, knob Ø 36 (`pad_all 11`, bg `text1`), `LV_OBJ_FLAG_ADV_HITTEST` (ring-only), `lv_arc_set_change_rate(arc, 360)` |
| Icon | `font_fa32` F185 / F028, centred (180,124) |
| Value | `font_k36` "65 %", centred (180,176), bound to the subject |
| Caption | `font_t16` "Яркость", centred (180,214) |
| Test bead (volume only) | Ø 52 at (180,256), "▶" plays a 300 ms 1 kHz beep at the new volume |
| Done bead | Ø 56 at (180,316) (ρ 136, outer r 164 ✓; it sits in the dial's bottom gap θ 60–120) |

Both apply live. Brightness uses LEDC 10-bit duty = `round(1023·(v/100)^2.2)` with a minimum of 5 %. Volume is a software sample multiply of `(v/100)^2`, and 0 = mute.

### 3.7 S5g Phone setup (SoftAP + `lv_qrcode`)

| Object | Geometry |
|---|---|
| North arclabel | "Шаг 1/2 · Подключите телефон" → "Шаг 2/2 · Откройте страницу" |
| Step dots | Two Ø 8 dots at r = 132, θ 262° / 278° (active dot = `primary`) |
| QR | `lv_qrcode_create`, `lv_qrcode_set_size(qr, 192)`, `lv_qrcode_set_quiet_zone(qr, true)`, light `#FFFFFF`, dark `#0A0D12`, centred (180,172), bounding box x 84–276, y 76–268. Corners (96,104) → r 141.5 ✓. |
| Fallback text | South CCW arclabel `font_t16`: "RoundAI-A1B2 · 48151623" (SSID · password) at step 1, "192.168.4.1" at step 2 |
| Bottom cap | Absent (the hint occupies it). Next and back use the top cap, PWR or swipe. |

- **Payloads:**
  - Step 1 is `WIFI:T:WPA;S:RoundAI-A1B2;P:48151623;;`: 39 bytes, so version 3 at ECC M (lv_qrcode uses `qrcodegen_Ecc_MEDIUM` with the minimum version that fits). That is 29 modules plus the quiet zone, giving a scale of 5 px/module (≈ 0.65 mm).
  - Step 2 is `http://192.168.4.1/` (19 bytes, v2).
- **Lifecycle:**
  - Auto-advance to step 2 on `WIFI_EVENT_AP_STACONNECTED`.
  - When the portal form posts, show a success card: "Wi-Fi: Home · Ключ: sk-or-…a1f3 ✓". Then connect STA and validate the key with `GET /api/v1/key`.
  - Backlight is forced to 100 % while the QR is shown.
  - The AP stops after 10 min or on back.
- **SoftAP security:** WPA2 with a random 8-digit password generated per session.
- **Portal implementation:** our own `esp_http_server` page. esp-wifi-connect's page cannot carry API keys (verified). The page has fields for Wi-Fi SSID and password, and for base URL, key and model for each of the chat, STT and TTS endpoints, plus the UI language.

### 3.8 S5i Diagnostics / About (lens list rows, live values)

- Firmware "v0.3.1 · ota_0 · 2026-10-17"
- Panel "ST77916 · ID 00 7F 7F 7F"
- Touch "CST816T"
- Wi-Fi "Home · −58 dBm · 192.168.1.23"
- Memory "SRAM 92 KB · PSRAM 6.1 MB free"
- Render "41 fps · CPU 38 %" (`LV_USE_SYSMON`)
- Mic (a live rim meter: the activity ring shows RMS)
- Speaker test
- Touch test (draws a dot at each sample)
- Balance
- Uptime
- Last error (code + time)

Actions: "Перезагрузить" and "Сброс настроек" (hold-to-confirm, §5.8).

### 3.9 Onboarding S1 (step dots on the north rim: 6 × Ø 8 at r 132, θ 250–290 every 8°)

| Step | Content | Geometry |
|---|---|---|
| O1 Language | Two half-disc targets: the left half (x < 178) "Русский" and the right half (x > 182) "English" (`font_t20`, centred at (96,180) / (264,180)). A 4 px `outline` divider at x = 180. | Each target is about 166×336. |
| O2 Setup method | Two pills 260×64: "F3CD С телефона (рекомендуется)" centred y 150, "F11C На устройстве" centred y 226. | (130,62) → r 144 ✓ |
| O3 | S5g, or S5a → S5b(password) → S5b(API key, with the note "≈ 73 символа — удобнее с телефона") | — |
| O4 Sound & mic | "Динамик подключён?" [Да, проверить] / [Нет]. Yes plays a beep and asks "Слышно?". Mic check: "Скажите что-нибудь"; the activity ring fills with RMS, and ✓ appears when RMS > −40 dBFS for 300 ms. | Pills as O2 |
| O5 How-to | Animated hints: orb with "Удерживайте шар — говорите", then a Ø 10 dot travelling around the rim (r 158, 1.2 s per lap, two laps) with "Проведите по краю — меню". | — |
| O6 Done | Orb bloom → S2 | — |

---

## 4. The ring keyboard (S5b)

### 4.1 Why a ring, and not a grid, T9 or a rim picker

- **Capacity proof for a grid.** With ≥ 50 px keys, 2–3 px gaps and a text field on top, rows inside the circle hold at most:
  - 6 keys at d ≤ 53 (row needs 315, chord 318.8)
  - 6 at d ≤ 50
  - 5 at d ≤ 103 (chord 265)
  - 2 at d ≤ 156
  
  That is about 17–19 keys per page, with **no room left** for ⇧ ⌫ space ✓ RU/EN 123. Putting the 3 most important controls on the bottom rim needs sinθ ≥ 0.95, which allows only 2 beads. A grid wastes the 57 %-of-area rim.
- **T9 groups** cost 2 taps per letter. A **rotary rim picker** (COMPASS) was validated only on physical-bezel watches (verify note), so it was rejected.
- **Ring layout (chosen).** Each layer puts 16 letters plus 2 controls on the rim. Every sector is ≥ 50 px wide at its midline and 64 px deep including the edge. The core holds a 2-line text field and 6 large controls.
- **Russian alphabet (33 letters):**
  - two alphabetical halves **А–П / Р–Я** (16 + 16)
  - **Ё = long-press Е** (the phone convention)
  - Alphabetical order reads left-to-right over the arch, from 7 o'clock up and over to 5 o'clock, so it needs no learning.
- **Expected speed:** about 1.5 taps per Russian letter (half switches) and 1 per Latin letter in a half.
- **Role of the keyboard:** it is the fallback. The primary paths are voice dictation for chat and the phone portal for keys and passwords.

### 4.2 Ring geometry (`ui_ringkb`, custom widget)

- **Widget:** one `lv_obj` 336×336 at (12,12) with `LV_OBJ_FLAG_ADV_HITTEST`. A hit is accepted when 116 ≤ r ≤ 180.
- **Sectors:** 18 sectors of 20°. Each is drawn with `lv_draw_arc` (centre C, radius 168, width 46, angles θc ± 9.4°, so about a 3 px gap at r 145).
- **Visible band:** r 122–168 ✓ (≤ 168). Touch band r 116–180 (64 px deep).
- **Sector width** at the label radius r = 145 is 2π·145/18 = **50.6 px**, and 51.7 px at the touch midline r = 148 ✓.
- **Labels:** `font_k24`, upright (not rotated), centred at `(180 + 145·cosθc, 180 + 145·sinθc)` and drawn with `lv_draw_label`.
- **Styling:**
  - letter sectors `surface2`
  - control sectors `surface3` with `text2` labels
  - pressed sector `outline` fill + accent text
  - disabled sector `text3`

| k | θc | Label centre (x,y) | RU-L lower / upper | RU-R lower / upper | EN-L lower / upper | EN-R lower / upper | 123 | #+= |
|---|---|---|---|---|---|---|---|---|
| 0 | 120° | (107.5, 305.6) | а / А | р / Р | a / A | n / N | 1 | ! |
| 1 | 140° | (68.9, 273.2) | б / Б | с / С | b / B | o / O | 2 | ? |
| 2 | 160° | (43.7, 229.6) | в / В | т / Т | c / C | p / P | 3 | : |
| 3 | 180° | (35.0, 180.0) | г / Г | у / У | d / D | q / Q | 4 | ; |
| 4 | 200° | (43.7, 130.4) | д / Д | ф / Ф | e / E | r / R | 5 | / |
| 5 | 220° | (68.9, 86.8) | е / Е (long → ё / Ё) | х / Х | f / F | s / S | 6 | ( |
| 6 | 240° | (107.5, 54.4) | ж / Ж | ц / Ц | g / G | t / T | 7 | ) |
| 7 | 260° | (154.8, 37.2) | з / З | ч / Ч | h / H | u / U | 8 | " |
| 8 | 280° | (205.2, 37.2) | и / И | ш / Ш | i / I | v / V | 9 | ' |
| 9 | 300° | (252.5, 54.4) | й / Й | щ / Щ | j / J | w / W | 0 | + |
| 10 | 320° | (291.1, 86.8) | к / К | ъ / Ъ | k / K | x / X | . | = |
| 11 | 340° | (316.3, 130.4) | л / Л | ы / Ы | l / L | y / Y | , | * |
| 12 | 0° | (325.0, 180.0) | м / М | ь / Ь | m / M | z / Z | - | & |
| 13 | 20° | (316.3, 229.6) | н / Н | э / Э | . | ' | _ | % |
| 14 | 40° | (291.1, 273.2) | о / О | ю / Ю | - | , | @ | $ |
| 15 | 60° | (252.5, 305.6) | п / П | я / Я | @ | _ | # | ~ |
| 16 | 80° | (205.2, 322.8) | **123** | **123** | **123** | **123** | **АБВ** / **ABC** (back to letters) | **АБВ** / **ABC** |
| 17 | 100° | (154.8, 322.8) | **Р–Я** | **А–П** | **N–Z** | **A–M** | **#+=** | **123** |

Count: RU-L 16 + RU-R 16 + ё (long-press) = **33** ✓. EN 13 + 13 letters, plus `. - @ ' , _` for e-mails and passwords.

### 4.3 Core geometry (inside r ≤ 114)

| Row | y | Elements (x ranges) | Width / chord | Worst corner r |
|---|---|---|---|---|
| Caption | 80–98 | `font_ui14` `text2`, ≤ 100 px, DOTS ("Пароль", "SSID", "Ключ API") | chord(114) at d 100 = 109 | — |
| Text field | 102–150 | `lv_textarea` x 98–262 (164×48), `font_t18`; one-line (scrolls horizontally) or 2 lines × 22 px for chat | chord(114) at d 78 = 166 | (82,78) → 113.2 ✓ |
| Row A | 154–206 | ⇧ 71–141, «пробел» 145–215, ⌫ 219–289 (3 × 70, gaps 4) | 218 ≤ 222 (d 26) | (109,26) → 112.1 ✓ |
| Row B | 210–262 | ✕ 101–151, RU/EN 155–205, ✓ 209–259 (3 × 50, gaps 4) | 158 ≤ 158.4 (d 82) | (79,82) → 113.9 ✓ |
| Preview bubble | transient | Ø 60, centre at r = 86 along the pressed sector's θc, `font_k36` glyph; outer r 116 (never clipped) | — | — |

The core controls are **one `lv_buttonmatrix`** at x 71–289, y 154–262 (218×108), with `pad_row 4`, `pad_column 4` and popovers off. Row B is narrowed with HIDDEN spacers; the verified behaviour is that hidden buttons keep their units:

```c
static const char * const core_map[] = {
  LV_SYMBOL_UP, "пробел", LV_SYMBOL_BACKSPACE, "\n",
  " ", LV_SYMBOL_CLOSE, "RU", LV_SYMBOL_OK, " ", "" };
static const lv_buttonmatrix_ctrl_t core_ctrl[] = {
  LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | 1,   /* ⇧ 70 px */
  LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | 1,   /* space 70 */
  1,                                     /* ⌫ 70: fires on PRESS + LONG_PRESSED_REPEAT */
  LV_BUTTONMATRIX_CTRL_HIDDEN | 1,       /* spacer: (218-16)/8*1 = 25.25 px */
  LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | 2,   /* ✕ 50.5 */
  LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | 2,   /* RU/EN 50.5 */
  LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | 2,   /* ✓ 50.5 */
  LV_BUTTONMATRIX_CTRL_HIDDEN | 1 };
```

The "RU" label is rewritten to "EN" through a second static map pair, swapped with `lv_buttonmatrix_set_map`. Shift and caps state are shown with `LV_BUTTONMATRIX_CTRL_CHECKED` on button 0. Caps lock additionally sets `CUSTOM_1`, which the draw hook renders as an underline bar.

### 4.4 Mapping to LVGL, and why `lv_keyboard` is not used

- **Why not `lv_keyboard`.** It is a rectangular buttonmatrix, its `kb_map[]` and `kb_ctrl[]` are file-static and global, and it has only 8 modes. None of that can express sectors. The ring is therefore `ui_ringkb`, which keeps **buttonmatrix map semantics**:
  - `const char * const map[18]` (the "" terminator is optional)
  - `const lv_buttonmatrix_ctrl_t ctrl[18]` reusing `HIDDEN`, `DISABLED`, `NO_REPEAT`, `CUSTOM_1` (has a long-press alternate) and `CUSTOM_2` (control key style)
  - a parallel `const char * const alt[18]` (only `alt[5] = "ё"/"Ё"` is non-NULL)
- **Map tables:** 12 static tables (RU-L/RU-R/EN-L/EN-R × lower/upper, plus 123 and #+=), indexed `maps[lang][half][case]`. A switch calls `ui_ringkb_set_map(ring, map, ctrl, alt)`, which invalidates the whole ring object once.
- **One shared VALUE_CHANGED handler** serves both `ui_ringkb` and `core_bm` and replaces `lv_keyboard_def_event_cb`:

```c
static void kb_value_cb(lv_event_t *e) {
  lv_obj_t *src = lv_event_get_target_obj(e);
  const char *t = (src == core_bm) ? lv_buttonmatrix_get_button_text(src, lv_buttonmatrix_get_selected_button(src))
                                   : ui_ringkb_get_committed_text(src);   /* letter or alt */
  if (!t) return;
  if      (!strcmp(t, LV_SYMBOL_BACKSPACE)) lv_textarea_delete_char(ta);
  else if (!strcmp(t, LV_SYMBOL_UP))   kb_shift_tap();          /* 1 tap one-shot, 2 taps <350 ms caps lock */
  else if (!strcmp(t, "пробел"))       lv_textarea_add_char(ta, ' ');
  else if (!strcmp(t, "RU") || !strcmp(t, "EN")) kb_toggle_lang();
  else if (!strcmp(t, LV_SYMBOL_OK))   kb_finish(true);         /* sends READY to owner */
  else if (!strcmp(t, LV_SYMBOL_CLOSE)) kb_finish(false);       /* CANCEL, confirm if text non-empty */
  else if (ui_ringkb_is_control(src))  kb_switch_layer(t);      /* "Р–Я","А–П","N–Z","A–M","123","#+=","АБВ","ABC" */
  else { lv_textarea_add_text(ta, t); kb_after_char(); }        /* one-shot shift off, auto-cap after ". " */
}
```

**Ring press behaviour.** The ring is our own widget, so these rules are implementable directly:

1. **PRESSED.** Select the sector under the finger, highlight it (invalidate only its precomputed bounding box), and show the preview bubble.
2. **PRESSING.** The selection *follows the finger* while r ≥ 104 ("slide to correct"). Moving to a new sector resets the long-press timer. If r < 104 the bubble shows a grey ✕ and no sector is selected.
3. **Long press.** Holding one sector for 450 ms (own `lv_timer`) swaps the bubble to `alt[k]`, if one exists.
4. **RELEASED.** If a sector is selected, send `LV_EVENT_VALUE_CHANGED`. The committed text is the alternate if it was shown, otherwise the base glyph.
5. **PRESS_LOST.** Cancel.

The ring never auto-repeats. Only ⌫ in the core repeats, after `lv_indev_set_long_press_time(indev, 400)` and `lv_indev_set_long_press_repeat_time(indev, 80)`.

### 4.5 Field types

| Type | Start layer | Behaviour | Max length |
|---|---|---|---|
| CHAT | RU-L (UI language) | 2 lines, auto-capitalise at start and after ". ", BOOT = dictation | 500 |
| SSID | EN-L | one line | 32 |
| PASSWORD | EN-L | `lv_textarea_set_password_mode(ta,true)`, last char shown 1500 ms (`LV_TEXTAREA_DEF_PWD_SHOW_TIME`), long-press on the field toggles reveal | 64 |
| URL | EN-L, then the `123` layer is one tap away | one line, no auto-cap | 200 |
| KEY | EN-L | one line, masked except the last 4 chars after entry, note "удобнее с телефона" | 160 |

Other behaviour:
- **Cancel:** PWR short press = ✕ (with "Отменить ввод?" if the text is not empty).
- **Swipe-back** is disabled on this screen.
- **Entrance:** the ring sectors "deal" clockwise from k = 0 (stagger 8 ms, each sector fades text_opa 0→255 in 120 ms; 264 ms total).

---

## 5. Interaction design

### 5.1 Push-to-talk (same rules for the orb and BOOT)

- Thresholds: `HOLD_MS = 400`, `MAX_REC_S = 30` (amber at 25 s), `PREROLL_MS = 200`. The mic I2S runs continuously into a 200 ms PSRAM ring, so the first syllable is never lost.
- **Press-down** goes to LISTENING immediately: feedback within one frame, recording includes the pre-roll.
- **Release** depends on how long the press lasted:
  - ≥ HOLD_MS: hold-to-talk, so stop and send (UPLOADING).
  - < HOLD_MS: *latched* (tap-to-talk). Recording continues and the cancel bead appears. The next press of the orb or BOOT sends. Optional VAD auto-send fires after ≥ 0.5 s of speech followed by 1.2 s of silence (energy threshold −45 dBFS).
- **Cancel:**
  - *on screen while holding:* drag the finger out to r > 116 from C (stage layout) or more than 70 px from the docked orb. The activity ring turns red, the north caption reads "Отпустите — отмена", and release discards the recording.
  - *latched:* the cancel bead or PWR.
  - *BOOT hold:* PWR pressed while BOOT is held.
  - *UPLOADING/THINKING:* a tap on the orb or a BOOT press aborts the HTTP request and shows "Отменено" in the north caption for 1.5 s.
- **Interrupting speech (SPEAKING):** a press on the orb or BOOT stops TTS **immediately** (flush the I2S DMA, close the HTTP stream). If the press is held 300 ms or more, it becomes a new LISTENING turn (barge-in). A shorter press just stops.
- **Text-only toggle:** the Mode Ring "Голос" segment, or Settings item 10/5. In text-only mode SPEAKING is replaced by REVEAL (§6) and the status shows F6A9.
- **No speaker connected:** voice replies default to off. Turning voice on while "Динамик подключён: нет" asks "Динамик подключён?" with a test beep. The board cannot detect the speaker electrically.

### 5.2 Orb states (what the user sees)

| State | Orb | Rim | North caption |
|---|---|---|---|
| idle | Blue core rc 56, halo r 64 (2 px, opa 90), slow breathing | — | time · Wi-Fi · voice |
| listening | Cyan core rc 50, 32 radial bars r 62 → up to 100 driven by mic spectrum | elapsed time arc (clockwise from 270°) | "Слушаю…" / "Нажмите — отправить" (latched) |
| uploading | Violet core contracts to 44, bars collapse | upload % arc | "Отправка 42 %" |
| thinking | Violet core rc 46 + 3 counter-rotating arcs r 58/66/74 | — | "Думаю…" |
| speaking | Docked green core rc 24 + 24 bars r 28–44 from TTS level | playback progress | model short name |
| reveal (text-only) | Docked green core, one pulse 24→28→24 | — | — |
| error | Red core, shake, then static | one red full-circle flash 400 ms | short error text in red |

### 5.3 Chat appearance

- **Assistant:** when a reply arrives, the stage morphs into reading (§6.2).
  1. The user line fades in first, starting at t = 60 ms.
  2. The reply follows, starting at t = 120 ms.
  3. With voice on, the full text appears and the docked orb speaks.
  4. Text-only mode reveals the reply line by line (§6.1).
- **History:** a new turn appended while S3 is open slides in (translate_y 20→0, bg_opa and text_opa 0→255, 220 ms ease_out) and the list auto-scrolls to the end with `lv_obj_scroll_to_view(child, LV_ANIM_ON)`.
- **Storage:** the last 20 turns are kept in a PSRAM ring of structs. History bubbles are created from it on open.

### 5.4 Translator turn-taking (decision)

**Default: one auto-detect talk control** (the orb or BOOT). The model returns `{src_lang, transcript, translation, translit}`, and the UI lights the matching pole. This frees the partner from learning a "their button".

**Explicit override: hold a pole.** Holding the west pole forces source = A ("I speak RU"). Holding the east pole forces source = B. This covers short utterances, names and similar languages, where detection is unreliable, and it is a natural "me / them" split on a device lying between two people.

**Pole tap vs hold:**
- Recording starts on press.
- A release before 400 ms discards the audio and opens S4a for that side.
- A release at 400 ms or later sends the recording with the forced source.

**Sequence after a successful turn:**
1. The source pole glows at opa 255.
2. The flow comet runs from the source pole to the target pole through the south rim (700 ms).
3. The direction chip and the translation appear (§6.2 timeline).
4. TTS speaks with the target-locale voice (MAI picks the language from the voice locale).
5. The target pole glows during playback.

### 5.5 Physical buttons

| Button | Short (< 600 ms) | Hold | Notes |
|---|---|---|---|
| BOOT (GPIO0, active low, 30 ms debounce) | Talk per §5.1 (press = start) | Talk (hold) | Universal. In S5b: dictation (not for PASSWORD/KEY). Strapping pin only matters at reset. |
| PWR (GPIO6, active low) | Top-level: toggle Mode Ring. Sub-screen: back. S5b: cancel. Listening: cancel. | ≥ 2 s: power menu OV-PWR ("Выключить", "Перезагрузить", "Отмена") | "Выключить" releases the GPIO7 latch. On USB power, show "Питание от USB — отключите кабель". |

**Screen timeout:**
- After 60 s idle the screen dims to 20 %.
- After 5 min (configurable) the backlight turns off and LVGL timers pause.
- The first touch or button press only wakes the screen and is swallowed (`lv_indev_wait_release`).
- BOOT wakes the screen **and** starts talking.

### 5.6 Translator JSON contract (UI-relevant fields)

```
{ "src_lang": "ru"|"en"|..., "transcript": "...", "translation": "...", "translit": "..."|"" }
```

`translit` is requested only when the target script is outside FULL (zh, ja, ko, ar, hi, th, el). The UI then shows `translit` in the translation slot, with the badge "Aa латиница", while TTS speaks `translation`.

### 5.7 Error states

| Trigger | Orb | North caption | Card in the reading slot (`font_t18`) | Bottom/primary action |
|---|---|---|---|---|
| No Wi-Fi / STA disconnected | idle grey (`text3` gradient), amber halo | "Нет Wi-Fi" (F1EB red) | "Нет подключения к Wi-Fi. Переподключение…" (with a comet) | Cap "Настроить Wi-Fi" → S5a |
| HTTP 401 | error | "Ключ не принят" | "OpenRouter отклонил ключ API (401)." | Cap "Ввести ключ" (S5b KEY) · long caption "или с телефона" (S5g) |
| HTTP 402 | error | "Нет средств" | "Недостаточно средств на OpenRouter (402). Баланс: $0.00" | Cap "Баланс" → S5j |
| HTTP 403 | error | "Регион недоступен" | "Сервис недоступен из вашего региона (403). Укажите другой адрес API." | Cap "Адрес API" → S5h |
| HTTP 429 | warning | "Повтор через 12 с" | countdown on the activity ring | auto-retry once |
| 5xx / 524 / 529 | warning | "Сервис перегружен" | auto-retry with the `models` fallback | — |
| Timeout (30 s) | error | "Нет ответа" | "Сервер не ответил за 30 с." | tap the orb to retry |
| Mic silent (RMS < −55 dBFS for the whole clip) | warning | "Не слышу" | "Не слышу вас — проверьте микрофон." | — |
| Empty transcript | warning | "Не распознано" | "Не удалось распознать речь." | — |
| TTS failure (voice on) | none (soft) | "Голос недоступен" | reply shown as text (REVEAL) | — |
| No speaker | — (not an error) | F6A9 in status | — | — |

Errors also go to the Diagnostics "Last error" row. The card is replaced by the next turn.

### 5.8 Hold-to-confirm (destructive actions)

Destructive actions are "Новый диалог", "Сброс настроек" and "Выключить".

1. Press and hold the bottom cap.
2. The activity ring fills clockwise from 270° over 1500 ms (`lv_anim`, linear) in `error` red for resets and `primary` for others.
3. The cap label changes to "Держите…".
4. Releasing early retracts the ring in 200 ms (ease_out).
5. Completing the hold fires the action.

---

## 6. Animations: mechanism, timing and dirty area

### 6.1 Budget model

- **Bus throughput.** QSPI at 80 MHz moves about 40 MB/s, so a full frame of 259.2 KB takes 6.5 ms. At the bring-up clock of 40 MHz it takes 13 ms. With two 28.8 KB buffers, flushing overlaps rendering, so **render time is the limit, not the bus**.
- **Per-pixel planning costs.** These are estimates for the S3 at 240 MHz with **no SIMD** (esp_lvgl_port SIMD is off for LVGL ≥ 9.2). Calibrate them in milestone UI-0 with `lv_demo_benchmark` and `LV_USE_SYSMON`.

| Operation | Estimated cost |
|---|---|
| opaque fill | ≈ 8 ns/px |
| alpha fill | ≈ 20 ns/px |
| 4bpp text | ≈ 40 ns/px of glyph box |
| anti-aliased arc/line stroke | ≈ 60 ns/px of stroke |
| simple gradient | ≈ 15 ns/px |
| rotated glyph (arclabel) | ≈ 300 ns/px |
| LVGL overhead | ≈ 0.5–1 ms per refresh |

- **Rules:**
  - *Continuous* animations keep the dirty area ≤ 45 k px (≈ 35 % of the disc) and ≤ 5 ms of estimated render time, which gives ≥ 30 fps with margin.
  - *One-shot* transitions may reach ≈ 110 k px for ≤ 400 ms.
  - Measure on the device and log through the dev console.
- **Refresh period.** Set `LV_DEF_REFR_PERIOD 16` (the default is 33). The LVGL task sleeps between events (esp_lvgl_port LVGL 9 feature).
- **Banned patterns:**
  - `clip_corner` on the root screen or any large object (it makes ARGB8888 layers plus masks every frame)
  - `transform_scale` / `transform_rotation` on objects larger than 120×120 that animate
  - `style_opa` on containers (use `bg_opa` / `text_opa` / `arc_opa`)
  - shadows
  - Lottie
  - `lv_screen_load_anim` FADE/MOVE (screens load with `lv_screen_load()`, no animation)
  - full-screen gradients or background images

### 6.2 Animation table

| # | Animation | Mechanism | Animated property | Duration / easing | Dirty area per frame | Est. render |
|---|---|---|---|---|---|---|
| A1 | Idle breathing | `lv_anim` on a custom var, exec_cb **quantised**: invalidate only when rc changes 1 px or halo opa changes ≥ 8 | rc 54↔58, halo opa 60↔120 | 3200 ms period, `ease_in_out`, `set_reverse_duration 1600`, infinite; stops after 60 s idle | Ø 132 box = 17.4 k px at ≈ 8 updates/s | 0.4 ms |
| A2 | Listening bars | `lv_timer` 33 ms reads the atomic levels, smooths each bar (`L = max(new, 0.82·L)`), invalidates the orb box if any bar moved ≥ 1 px. Drawn in the orb `LV_EVENT_DRAW_MAIN` with 32 `lv_draw_line` (width 4, round ends) | bar length 4 + 34·level | continuous, 30 Hz | 204×204 = 41.6 k px | ≈ 1.5 ms (+ 2.1 ms flush, overlapped) |
| A3 | Elapsed-time ring | `lv_arc_set_value` from an `lv_timer` at 100 ms | indicator end angle | linear | changed span only (≤ 6 k px) | < 0.3 ms |
| A4 | Listening → uploading | `lv_anim_timeline`: bars collapse (`ease_in` 180 ms), rc 50→44 (`ease_out` 220 ms) | bar scale, rc | 220 ms | 41.6 k px | ≈ 1.5 ms |
| A5 | Thinking arcs | One `lv_anim` 0–3600 (deci-deg), 3600 ms linear infinite; draw handler derives three phases (×3 at 1200 ms/rev, ×−2 at 1800 ms/rev reversed, ×4 at 900 ms/rev). Arcs drawn with `lv_draw_arc` (width 3/2/2, spans 70°/50°/30°, opa 255/180/120). | arc start angles | infinite | 152×152 = 23 k px | ≈ 0.8 ms |
| A6 | Stage → Reading | `lv_anim_timeline`, 360 ms: (a) 0–280 ms orb centre y 180→302, rc 56→24, box resized every frame (`lv_obj_set_pos/size`, **not** transform), `ease_in_out`; (b) 60–300 ms user line text_opa 0→255; (c) 120–360 ms reply text_opa 0→255 + translate_y 12→0, `ease_out` | pos, size, text_opa, translate_y | 360 ms; reverse (`lv_anim_timeline_set_reverse`) 280 ms for a new turn | ≤ 85 k px (orb strip 140×260 + reply 272×148 + user line 220×40) | ≈ 4 ms |
| A7 | Docked speaking bars | Same as A2 with 24 bars, levels from the TTS output RMS (published with a timestamp; the UI applies about 60 ms I2S latency) | bar length 2 + 16·level | 30 Hz | 96×96 = 9.2 k px | < 0.5 ms |
| A8 | Text-only reveal | `lv_timer` 50 ms grows the reply container height by 24 px (one line) | height | 50 ms per line | 272×24 = 6.5 k px per step | < 0.5 ms |
| A9 | Error shake | `lv_anim` exec_cb `translate_x = 6·sin(t·3·2π)·(1−t)` | translate_x | 360 ms, linear t | ≈ 144×132 = 19 k px | ≈ 0.6 ms |
| A10 | Mode Ring open | `lv_anim_timeline`: centre disc appears (one frame, 37 k px alpha) + 6 segment arcs grow `end_angle` from start → start+56° (`ease_out` 150 ms, stagger 15 ms) | arc end angle | 225 ms total; close = reverse 160 ms | first frame ≈ 40 k px, then ≈ 6 tips × 3.6 k = 22 k px | ≈ 1.5 ms |
| A11 | Segment highlight (scrub) | style state change (`LV_STATE_CHECKED`: arc colour = accent, icon `text1`) | colour | instant | one segment box ≈ 160×110 = 18 k px, on change only | ≈ 1 ms |
| A12 | Screen entrance (after `lv_screen_load`) | One full frame, then an element anim: orb bloom rc 0→56 (240 ms `lv_anim_path_overshoot`), or the lens-list items' translate_y 16→0 + text_opa (200 ms `ease_out`, stagger 20 ms, max 5 items) | rc / translate_y / text_opa | 240 ms | full frame once (≈ 15–25 ms), then ≤ 75 k px | ≈ 3 ms |
| A13 | Lens list scroll | LVGL scroll + `LV_EVENT_SCROLL` width/opa recompute (§3.5) | width, bg_opa, text_opa | finger-driven / momentum | viewport 312×244 = 76 k px | ≈ 3 ms |
| A14 | History scroll | LVGL scroll + fade-band opa | bg_opa, text_opa | finger | 272×248 = 67 k px | ≈ 3 ms |
| A15 | Key press | `ui_ringkb`: invalidate the sector bounding box (≤ 60×60) + preview bubble (60×60, old + new position) | fill colour, bubble pos | instant | ≈ 11 k px | < 0.5 ms |
| A16 | Keyboard layer switch | Labels swap; the ring object is invalidated once, plus a 20° accent comet sweeping the ring once (`lv_anim` 240 ms `ease_in_out`, arc box only) | map, comet angle | 240 ms | one frame of 113 k px, then ≈ 8 k px | ≈ 4 ms once |
| A17 | Dial drag (volume/brightness) | native `lv_arc` | value | finger | changed span + knob + value label (120×44) ≈ 12 k px | < 1 ms |
| A18 | Translator flow comet | `lv_anim` on `lv_arc_set_angles(comet, a, a+30)` with a 180→0 (decreasing, through 90) | angles | 700 ms `ease_in_out` | ≈ 10 k px | < 0.5 ms |
| A19 | Hold-to-confirm ring | `lv_anim` on the activity ring value | value | 1500 ms linear, retract 200 ms `ease_out` | changed span ≤ 6 k px | < 0.3 ms |
| A20 | Rim hint dot (onboarding) | `lv_anim` angle → `lv_obj_set_pos` of a Ø 10 dot at r 158 | x, y | 1200 ms/lap linear ×2 | 2 × 14×14 | negligible |

Notes:

- **Tearing.** TE is not used. Neither init table sends TEON, and QSPI supports tearing mode NONE only (verify). Small dirty areas hide tearing. The full frames happen at screen swaps and keyboard layer switches, once per user action, which is acceptable.
- **Even-coordinate rounding.** If the ST77916 shows artefacts with odd coordinates, add `rounder_cb` in esp_lvgl_port (≥ 2.7.1) to round areas to even values. It enlarges the dirty areas by ≤ 1 px per side.

---

## 7. LVGL 9.5 implementation notes

### 7.1 Widgets and modules

- **Used:** `lv_obj`, `lv_label`, `lv_arclabel` (9.4+), `lv_arc`, `lv_buttonmatrix`, `lv_textarea`, `lv_qrcode` (`LV_USE_QRCODE 1`, I1 canvas: a 192×192 code costs about 4.6 KB), `lv_anim`, `lv_anim_timeline`, `lv_timer`, `lv_translation`.
- **Custom:**
  - `ui_orb` (`lv_obj` + `LV_EVENT_DRAW_MAIN`, also handling `LV_EVENT_REFR_EXT_DRAW_SIZE` = 0 and COVER_CHECK = NOT_COVER, as in the music demo)
  - `ui_ringkb`
  - `ui_rim` (catcher)
  - `ui_cap`
  - `ui_lens_attach(container)` (scroll handler)
  - `ui_pole`
- **Not used:**
  - `lv_menu`, `lv_list`, `lv_win`: deprecated in 9.6
  - `lv_roller`: its INFINITE mode multiplies options
  - `lv_tileview`: full-screen swipes conflict with scrolling and talking
  - `lv_lottie`
  - `lv_keyboard`: §4.4. Leave `LV_USE_KEYBOARD` enabled only if a classic grid is added later.

### 7.2 Configuration (deltas from `lv_conf_template.h` 9.5)

```
LV_DEF_REFR_PERIOD 16            LV_USE_STDLIB_MALLOC LV_STDLIB_CUSTOM (heap_caps PSRAM-first, below)
LV_USE_ARCLABEL 1  LV_USE_QRCODE 1  LV_USE_OBSERVER 1  LV_USE_TRANSLATION 1
LV_USE_SNAPSHOT 1 (dev-console screenshots)   LV_USE_SYSMON 1 + LV_USE_PERF_MONITOR 1 (dev builds only)
LV_USE_DRAW_SW_COMPLEX_GRADIENTS 0 (only simple vertical gradients are used)
LV_DRAW_SW_CIRCLE_CACHE_SIZE 8   LV_DRAW_LAYER_MAX_MEMORY (96*1024)  /* hard cap: catches accidental layers */
LV_ATTRIBUTE_FAST_MEM IRAM_ATTR  (CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM)   LV_USE_FLOAT 1 (arclabel/orb trig)
```

- **Allocator.** `lv_malloc_core` = `heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)`, and likewise for `realloc`, plus `free`. The default `malloc` would put LVGL's many small allocations below `SPIRAM_MALLOC_ALWAYSINTERNAL` into scarce internal RAM.
- **Draw buffers.** esp_lvgl_port 2 × 360×40 with `buff_dma = 1, buff_spiram = 0, double_buffer = 1, swap_bytes = 1`. Render mode PARTIAL. The LVGL task is pinned to core 1 at priority 4. Wi-Fi and audio run on core 0.

### 7.3 Style sharing

All styles are `static lv_style_t` and are initialised once in `ui_theme_init()`:

`st_bg, st_text1, st_text2, st_text3, st_pill, st_pill_focus, st_bubble_user, st_bubble_ai, st_key, st_key_pr, st_key_ctrl, st_bead, st_bead_pr, st_cap, st_arc_track, st_arc_ind_{idle,listen,think,speak,err,warn}, st_pole_a, st_pole_b`

- Widgets use `lv_obj_add_style`.
- Local `lv_obj_set_style_*` calls are allowed only for animated properties (translate_y, text_opa, bg_opa, width) and only on objects that animate.
- No theme: `lv_display_set_theme(disp, NULL)` or the simple theme, so default theme styles are not applied and then overridden.

### 7.4 State binding (observer), wrapped for 9.6

```c
/* ui_subject.h — the only place that knows the LVGL version */
static inline lv_subject_t *ui_subject_int(int32_t v, int32_t min, int32_t max) {
#if LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR >= 6
    lv_subject_t *s = lv_subject_create(LV_SUBJECT_TYPE_INT);   /* starts neutral */
#else
    lv_subject_t *s = lv_malloc(sizeof(lv_subject_t));
    lv_subject_init_int(s, v);
#endif
    lv_subject_set_min_value_int(s, min); lv_subject_set_max_value_int(s, max);
    lv_subject_set_int(s, v);                                   /* 9.6: limits before value */
    return s;
}
/* bindings: always via lv_subject_add_observer_obj(s, cb, obj, ud) — works on 9.5 and 9.6;
   never lv_obj_bind_flag_if_eq / bind_state_if_eq (deprecated in 9.6).
   lv_label_bind_text(label, s, fmt) takes the pointer in both versions. */
```

Subjects (all `lv_subject_t *`, created in `ui_model_init()`):

| Subject | Type / range | Drives |
|---|---|---|
| `S_MODE` | int 0–3 (ASSIST, TRANSL, HISTORY, SETTINGS) | screen loads, Mode Ring checked segment |
| `S_VSTATE` | int 0–6 (IDLE, LISTEN, UPLOAD, THINK, SPEAK, REVEAL, ERROR) | orb timelines, captions, activity ring colour, cancel bead |
| `S_LATCH` | int 0/1 | cancel bead, caption text |
| `S_VOICE_OUT` | int 0/1 | status icon, Mode Ring segment, REVEAL vs SPEAK |
| `S_WIFI` | int (OFF, CONNECTING, OK, FAIL) + `S_RSSI` int | status, error card |
| `S_NETERR` | int (0, 401, 402, 403, 429, 500, TIMEOUT, MIC, EMPTY) | error card |
| `S_VOLUME` | int 0–100 | dial, audio gain |
| `S_BRIGHT` | int 5–100 | dial, LEDC |
| `S_UILANG` | int RU/EN | `lv_translation_set_language("ru"/"en")` |
| `S_TR_A`, `S_TR_B` | int (language index) | poles, picker |
| `S_TR_SRC` | int (-1, A, B) | pole glow, comet direction |
| `S_CLOCK_MIN` | int (minutes since midnight) | north status text |

### 7.5 i18n

- Static strings: `lv_translation_add_static(langs = {"ru","en",NULL}, tags, translations)` and `lv_label_set_translation_tag(label, "tag")`.
- Dynamic strings: `lv_tr("tag")`.
- On `LV_EVENT_TRANSLATION_LANGUAGE_CHANGED`, arclabels and buttonmatrix maps have their text reset by their owners. The 9.5 header shows no arclabel tag setter.

### 7.6 Thread-safety rule

- **Only the LVGL task touches LVGL objects or subjects.** Audio, Wi-Fi and HTTP tasks call `ui_post(ui_evt_t)` instead. It wraps `xQueueSend` to a 32-entry queue and never blocks.
- **Event pump.** An `lv_timer` every 20 ms in the LVGL task drains the queue and calls `lv_subject_set_int` and friends.
- **High-rate data** uses lock-free channels:
  - mic spectrum (16 bands as uint8, mirrored to 32 bars) or RMS
  - TTS level
  - upload bytes
  
  These go through a seqlock-protected struct written by the producer and read by the A2/A7 timers. Optional: 16 bands from esp-dsp `dsps_fft2r_fc32` on 512 samples every 40 ms on core 0. The fallback is RMS with fixed per-bar weights.
- **Exception path:** if non-LVGL code must call LVGL (for example the dev console injecting a screenshot request), it uses `if (lvgl_port_lock(100)) { …; lvgl_port_unlock(); }` and handles the timeout. Never call LVGL from ISRs. Never hold the lock across network I/O.

### 7.7 Memory per screen (LVGL heap in PSRAM unless noted)

| Item | Estimate |
|---|---|
| Draw buffers (internal DMA SRAM) | 2 × 28.8 KB |
| S2 Assistant (persistent, ≈ 30 objects + reply ≤ 1.2 k chars) | ≈ 12 KB |
| S4 Translator (persistent) | ≈ 12 KB |
| S3 History (20 turns, 40 bubbles × 2 objects + text) | ≈ 40 KB; deleted on exit |
| S5 / S5a / S5c / S5d lens lists (≤ 20 items × 4 objects) | ≈ 8–14 KB each; one alive at a time |
| S5c model JSON (transient parse buffer, `heap_caps` PSRAM) | ≤ 100 KB, freed after build |
| S5b keyboard (ring 1 object + core bm + textarea + bubble) | ≈ 3 KB + text |
| S5g QR (I1 canvas 192×192 + payload copy) | ≈ 5 KB |
| OV-MR Mode Ring (≈ 20 objects, created on open) | ≈ 5 KB |
| Partner-view rotated layer (optional) | ≈ 80 KB transient |
| Snapshot for the dev console (RGB565 full frame) | 259 KB transient |

Peak is under 150 KB plus transients. Fonts are in flash.

### 7.8 Verification hooks (requirement: UI checked without someone looking at the device)

- The dev console supports `shot` (`lv_snapshot_take` → PNG on the PC), `tap x y`, `drag x1 y1 x2 y2 ms` (a virtual pointer indev merged with the touch indev) and `state <S_VSTATE value>` to force orb states.
- Every geometry table above is a test vector. After each screen load, a debug overlay (dev builds only, drawn on `lv_layer_sys`) can paint the r = 168 circle and flag any clickable object whose corners exceed it.
- The same `ui/` code runs in the LVGL PC simulator at 360×360, with a circular mask drawn on the system layer for preview only.

---

## 8. Summary of key decisions

1. The **rim is the interface**: Mode Ring (tap/slide on the rim, or PWR), caps (top = back, bottom = primary action), arclabels, an activity ring, translator poles and the ring keyboard. Routing is decided by **where the touch starts** (r ≥ 150 → rim), so it cannot conflict with scrolling or hold-to-talk.
2. **Talk** is press = start for both the orb and BOOT. A press under 400 ms latches recording. A hold of 400 ms or more sends on release. Cancel by sliding out, the cancel bead or PWR. A press stops TTS immediately, and holding past 300 ms barges in.
3. **Translator** uses one auto-detect control, plus forced-language "hold a pole" for me/them.
4. **Keyboard** is a ring of 18 × 20° sectors (≥ 50.6 px), alphabetical halves А–П / Р–Я with Ё on long-press Е, plus a core `lv_buttonmatrix` (⇧ space ⌫ / ✕ RU ✓) and a 164-px field. It is our own widget with buttonmatrix-style maps and one shared VALUE_CHANGED handler.
5. **Performance:** no clip_corner, transforms, shadows, screen-load animations or `style_opa` containers. Continuous animations stay ≤ 45 k px of dirty area. The idle animation is quantised and then stops.
6. **Font:** Inter (OFL) in 7 generated sizes, about 195 KB, with FA5 icons merged, FULL glyph coverage for supported languages, a text sanitiser and romanisation for other scripts.
