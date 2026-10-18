#pragma once
// CJK fallback fonts (docs/ui.md 1.3): Tiny TTF instances at 16, 20 and 28 px over the font of the storage asset
// pack (components/assets), chained behind ui_16 / ui_20 / ui_28 as font->fallback, so any label in those fonts
// draws hanzi, kana and Hangul. Without the pack the fallbacks find no glyph and the UI stays Latin / Cyrillic.
#include <stdbool.h>
#include "lvgl.h"

// The fallbacks the generated Inter fonts point at (main/ui/fonts/ui_16.c ...). They report "no glyph" until the
// pack is verified and attached; then each one forwards the code points the pack covers to its Tiny TTF font.
extern lv_font_t ui_cjk_16, ui_cjk_20, ui_cjk_28;

typedef enum {
    UI_CJK_ABSENT,      // no pack in storage (or not flashed yet)
    UI_CJK_CHECKING,    // pack mapped, CRC check running in the cjk_crc task (idle priority, under a second)
    UI_CJK_READY,       // fonts attached
    UI_CJK_BAD,         // CRC mismatch, unknown format, no font entry or out of memory (a stored CJK translator
                        // pair then falls back as without the pack)
} ui_cjk_state_t;

// Maps the pack and checks its header (no LVGL, no lock): call first in ui_init, before the stored translator
// languages are checked against ui_cjk_offered().
void ui_font_probe(void);
// With the LVGL lock (ui_init), before the first label is drawn: moves LVGL's font draw buffers (glyph cache, label
// scratch) from the LVGL pool to PSRAM and queues the CRC check; the fonts are attached when it passes.
void ui_font_init(void);
ui_cjk_state_t ui_cjk_state(void);
// True while CJK can be drawn or will be in a moment (pack present and not known bad): the translator offers
// zh / ja / ko then.
bool ui_cjk_offered(void);
// "font" dev-console command (status, bench).
void ui_font_console_register(void);
