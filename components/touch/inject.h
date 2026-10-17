#pragma once
#include <stdbool.h>
#include "lvgl.h"

// A second LVGL pointer device driven from the dev console, so UI flows can be
// exercised from the PC without anyone touching the screen.
void inject_init(lv_display_t *disp);
void inject_set(int x, int y, bool pressed);
