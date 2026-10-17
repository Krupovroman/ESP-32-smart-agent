#pragma once
// The only place that knows which observer API LVGL has (docs/ui.md 7.6): 9.5 caller-owned subjects
// with lv_subject_init_int(), 9.6 LVGL-owned lv_subject_create(). Subjects change only in taskLVGL.
#include "lvgl.h"

typedef lv_subject_t *ui_subj_t;

// Creates an int subject with limits; never freed (subjects live as long as the UI).
ui_subj_t ui_subj_int(int32_t init, int32_t min, int32_t max);

// Current value.
static inline int32_t ui_get(ui_subj_t s)
{
    return lv_subject_get_int(s);
}

// Sets and notifies only on a real change, so observers never run for a no-op.
static inline void ui_set(ui_subj_t s, int32_t v)
{
    if (lv_subject_get_int(s) != v) {
        lv_subject_set_int(s, v);
    }
}

// Observer bound to obj: removed automatically when obj is deleted. The callback runs once at once.
static inline lv_observer_t *ui_watch(ui_subj_t s, lv_observer_cb_t cb, lv_obj_t *obj, void *ud)
{
    return lv_subject_add_observer_obj(s, cb, obj, ud);
}
