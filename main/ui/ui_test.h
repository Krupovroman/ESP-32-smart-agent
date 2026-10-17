#pragma once
#include <stdint.h>
#include "lvgl.h"

// Bring-up screen: colour bars, mic level ring, rotating arc, touch tracker and a 5 s REC button.
// Built by the navigation stack ("ui goto test"), deleted when left.
lv_obj_t *ui_test_build(uint32_t arg);
