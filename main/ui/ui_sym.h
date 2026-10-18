#pragma once
// Font Awesome 5 Free Solid glyphs merged into ui_16/ui_20/ui_28 (docs/ui.md 1.4), as UTF-8.
// Which font carries which icon: tools/mkfonts.ps1 (I_16, I_FULL = ui_20, I_28).

#define SYM_CHECK       "\xEF\x80\x8C"  // F00C  ui_16 ui_20 ui_28
#define SYM_TIMES       "\xEF\x80\x8D"  // F00D  ui_20 ui_28
#define SYM_POWER       "\xEF\x80\x91"  // F011  ui_20
#define SYM_COG         "\xEF\x80\x93"  // F013  ui_20
#define SYM_LOCK        "\xEF\x80\xA3"  // F023  ui_16 ui_20
#define SYM_VOLUME      "\xEF\x80\xA8"  // F028  ui_20 ui_28
#define SYM_QRCODE      "\xEF\x80\xA9"  // F029  ui_20
#define SYM_PLAY        "\xEF\x81\x8B"  // F04B  ui_20
#define SYM_STOP        "\xEF\x81\x8D"  // F04D  ui_20 ui_28
#define SYM_LEFT        "\xEF\x81\x93"  // F053  ui_16 ui_20 ui_28
#define SYM_RIGHT       "\xEF\x81\x94"  // F054  ui_16 ui_20
#define SYM_INFO        "\xEF\x81\x9A"  // F05A  ui_20
#define SYM_EYE         "\xEF\x81\xAE"  // F06E  ui_20
#define SYM_EYE_SLASH   "\xEF\x81\xB0"  // F070  ui_20
#define SYM_WARN        "\xEF\x81\xB1"  // F071  ui_16 ui_20 ui_28
#define SYM_DOWN        "\xEF\x81\xB8"  // F078  ui_20
#define SYM_KEY         "\xEF\x82\x84"  // F084  ui_20
#define SYM_GLOBE       "\xEF\x82\xAC"  // F0AC  ui_20 ui_28
#define SYM_BOLT        "\xEF\x83\xA7"  // F0E7  ui_16 ui_20
#define SYM_KEYBOARD    "\xEF\x84\x9C"  // F11C  ui_20
#define SYM_MIC         "\xEF\x84\xB0"  // F130  ui_20 ui_28
#define SYM_MIC_SLASH   "\xEF\x84\xB1"  // F131  ui_20
#define SYM_SUN         "\xEF\x86\x85"  // F185  ui_20 ui_28
#define SYM_LANGUAGE    "\xEF\x86\xAB"  // F1AB  ui_20
#define SYM_HISTORY     "\xEF\x87\x9A"  // F1DA  ui_20
#define SYM_WIFI        "\xEF\x87\xAB"  // F1EB  ui_16 ui_20 ui_28
#define SYM_TRASH       "\xEF\x87\xB8"  // F1F8  ui_20
#define SYM_BAT_FULL    "\xEF\x89\x80"  // F240  ui_16 ui_20
#define SYM_BAT_3       "\xEF\x89\x81"  // F241
#define SYM_BAT_2       "\xEF\x89\x82"  // F242
#define SYM_BAT_1       "\xEF\x89\x83"  // F243
#define SYM_BAT_EMPTY   "\xEF\x89\x84"  // F244
#define SYM_SYNC        "\xEF\x8B\xB1"  // F2F1  ui_20
#define SYM_EXCHANGE    "\xEF\x8D\xA2"  // F362  ui_20
#define SYM_MOBILE      "\xEF\x8F\x8D"  // F3CD  ui_20
#define SYM_COMMENT     "\xEF\x92\xAD"  // F4AD  ui_20
#define SYM_ROBOT       "\xEF\x95\x84"  // F544  ui_20
#define SYM_BACKSPACE   "\xEF\x95\x9A"  // F55A  ui_20 ui_28
#define SYM_MUTE        "\xEF\x9A\xA9"  // F6A9  ui_16 ui_20 ui_28

// Typographic characters from the Inter ranges (U+2026, U+2014, U+2192, U+2022)
#define TXT_ELLIPSIS    "\xE2\x80\xA6"
#define TXT_EMDASH      "\xE2\x80\x94"
#define TXT_ARROW       "\xE2\x86\x92"
#define TXT_BULLET      "\xE2\x80\xA2"
#define TXT_MIDDOT      "\xC2\xB7"
#define TXT_RSAQUO      "\xE2\x80\xBA"  // "›" after fault texts
#define TXT_LAQUO       "\xC2\xAB"      // «
#define TXT_RAQUO       "\xC2\xBB"      // »
#define TXT_TIMES       "\xC3\x97"      // × (speech speed "1.25×")
#define TXT_UP          "\xE2\x86\x91"  // ↑ (upload time)
#define TXT_APPROX      "~"             // U+2248 is not in the Inter ranges of 1.3
