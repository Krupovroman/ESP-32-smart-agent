#include "ui_subj.h"

ui_subj_t ui_subj_int(int32_t init, int32_t min, int32_t max)
{
#if LV_VERSION_CHECK(9, 6, 0)
    lv_subject_t *s = lv_subject_create(LV_SUBJECT_TYPE_INT);   // 9.6: LVGL-owned, starts at 0
    lv_subject_set_min_value_int(s, min);                       // limits before the first value
    lv_subject_set_max_value_int(s, max);
    lv_subject_set_int(s, init);
#else
    lv_subject_t *s = lv_malloc_zeroed(sizeof(lv_subject_t));   // 9.5: caller-owned, on the LVGL heap
    LV_ASSERT_MALLOC(s);
    lv_subject_init_int(s, init);
    lv_subject_set_min_value_int(s, min);
    lv_subject_set_max_value_int(s, max);
#endif
    return s;
}
