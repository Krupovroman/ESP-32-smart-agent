#pragma once
// One rim widget on lv_layer_top() for the recording time, the busy comet, the PWR-hold ring and the
// scroll thumb (docs/ui.md 3.1, 7.4). Every change invalidates only the arc spans that changed.
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

// Creates the 336x336 non-clickable object at (12,12) on the top layer.
void ui_rim_init(void);
// The rim object (for the tree / audit), or NULL before ui_rim_init().
lv_obj_t *ui_rim_obj(void);
// SCROLL channel: follow this container's LV_EVENT_SCROLL (NULL detaches and hides the thumb).
void ui_rim_attach_scroll(lv_obj_t *scroller);
// The container the SCROLL channel follows, or NULL.
lv_obj_t *ui_rim_scroller(void);
// RECORD channel: sweep for ms of recording out of 30 s, warning colour when warn; ms < 0 hides.
void ui_rim_record(int32_t ms, bool warn);
// BUSY channel: 40 deg comet, 1.2 s per turn.
void ui_rim_busy(bool on);
// POWER channel: 0..1000 permille of a full ring from 12 o'clock; < 0 hides.
void ui_rim_power(int32_t permille);
// Current POWER value (-1 when hidden).
int32_t ui_rim_power_get(void);
// Lock screen up: the rim moves above its overlay and draws only the POWER channel (the scroll thumb, comet and
// recording sweep of the screen below stay hidden); false draws every channel again.
void ui_rim_power_only(bool on);
