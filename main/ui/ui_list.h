#pragma once
// List template T_LIST (docs/ui.md 3.7): arc title, back zone, swipe-right back, a 252x270 vertical viewport of
// 252x64 rows with the curved-edge fade (opa 255 within 79 px of the centre line, 0 at 119 px; rows under
// opa 128 are not clickable) and the rim scroll thumb. taskLVGL only.
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "ui_theme.h"

#define UI_LIST_X       54      // viewport x 54-306, y 76-346
#define UI_LIST_Y       76
#define UI_LIST_W       252
#define UI_LIST_H       270
#define UI_ROW_H        64

typedef enum {
    UI_ROW_NONE,        // nothing in the right slot (actions)
    UI_ROW_CHEVRON,     // opens another screen
    UI_ROW_CHECK,       // radio: check mark in the mode accent when selected
    UI_ROW_TOGGLE,      // "Вкл" / "Выкл"
    UI_ROW_WIFI,        // 4 signal bars (+ lock)
} ui_row_right_t;

// New list screen named name with the arc title; *list receives the viewport (rows are added to it). The
// screen refreshes the fade after every layout change and plays the A24 entrance.
lv_obj_t *ui_list_screen(const char *name, const char *title, lv_obj_t **list);
// Clickable row (name copied; icon and sub may be NULL, the title is then centred). cb runs on CLICKED with ud.
lv_obj_t *ui_list_row(lv_obj_t *list, const char *name, const char *icon, const char *title, const char *sub,
                      ui_row_right_t right, lv_event_cb_t cb, void *ud);
// Info row: caption (ui_16 text-2) over a value (ui_20 text-1); never clickable.
lv_obj_t *ui_list_info(lv_obj_t *list, const char *name, const char *caption, const char *value);
// Small text-3 section caption between rows.
lv_obj_t *ui_list_section(lv_obj_t *list, const char *name, const char *text);

void ui_row_set_title(lv_obj_t *row, const char *title);
void ui_row_set_sub(lv_obj_t *row, const char *sub);
// Subtitle colour (accent top, e.g. warning for "истекает 20.10"); UI_ACC__N = default text-2.
void ui_row_set_sub_accent(lv_obj_t *row, ui_acc_t acc);
void ui_row_set_check(lv_obj_t *row, bool on);
void ui_row_set_toggle(lv_obj_t *row, bool on);
void ui_row_set_wifi(lv_obj_t *row, int rssi, bool locked);
// Disabled rows keep their fill, show text-3 and ignore taps.
void ui_row_set_enabled(lv_obj_t *row, bool en);
// Destructive rows: icon and title in the error colour.
void ui_row_set_danger(lv_obj_t *row);
void ui_info_set(lv_obj_t *row, const char *value);
// Recomputes the curved-edge fade of every row (after adding or removing rows).
void ui_list_refresh(lv_obj_t *list);
// Deletes the rows from index first on (lists rebuilt after a scan or a fetch).
void ui_list_trim(lv_obj_t *list, uint32_t first);
// Scrolls the row's list so the row sits on the centre line, without animation (console taps, pickers).
void ui_list_focus(lv_obj_t *row);
// True for objects made by ui_list_row / ui_list_info / ui_list_section.
bool ui_list_is_row(lv_obj_t *obj);
// Value passed as ud to ui_list_row.
void *ui_row_ud(lv_obj_t *row);
