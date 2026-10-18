// Z, ambient T_AMBIENT (docs/ui.md 3.13): clock, date and the dot orb. The clock label changes once a minute.
// While asleep the wake catcher swallows the touch; opened directly ("ui goto ambient") a tap goes home.
#include "ui_internal.h"
#include "ui_orb.h"
#include "ui_scr.h"
#include "ui_str.h"
#include "ui_theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// clock centre (180,166) w <= 160 (|(80,28)| = 84.8 + glyph box); date centre (180,212) w <= 220
#define CLOCK_W 160
#define DATE_W  220
#define TIME_OK 1700000000      // SNTP has set the clock

static lv_obj_t *s_scr, *s_clock, *s_date;
static lv_timer_t *s_timer;
static int s_last_min = -1;

static void refresh(void)
{
    static const char *const wd_ru[] = { "вс", "пн", "вт", "ср", "чт", "пт", "сб" };
    static const char *const wd_en[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const mon_ru[] = { "января", "февраля", "марта", "апреля", "мая", "июня", "июля",
                                          "августа", "сентября", "октября", "ноября", "декабря" };
    static const char *const mon_en[] = { "January", "February", "March", "April", "May", "June", "July",
                                          "August", "September", "October", "November", "December" };
    time_t now = time(NULL);
    if (now < TIME_OK) {
        if (s_last_min != -2) {
            s_last_min = -2;
            lv_label_set_text_static(s_clock, "--:--");
            lv_label_set_text_static(s_date, "");
        }
        return;
    }
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_min == s_last_min) {
        return;
    }
    s_last_min = t.tm_min;
    char buf[48];
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    lv_label_set_text(s_clock, buf);
    bool en = ui_str_lang() != 0;
    snprintf(buf, sizeof(buf), "%s, %d %s", en ? wd_en[t.tm_wday % 7] : wd_ru[t.tm_wday % 7], t.tm_mday,
             en ? mon_en[t.tm_mon % 12] : mon_ru[t.tm_mon % 12]);
    lv_label_set_text(s_date, buf);
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh();
}

static void press_cb(lv_event_t *e)
{
    (void)e;
    if (ui_asleep()) {
        ui_sleep_wake();
    } else if (ui_nav_current() == UI_SCR_AMBIENT) {
        ui_nav_back();
    }
}

static void delete_cb(lv_event_t *e)
{
    if (lv_event_get_current_target(e) != s_scr) {
        return;
    }
    s_scr = NULL;
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
}

static lv_obj_t *centred(const char *name, const lv_style_t *st, int32_t cy, int32_t w)
{
    lv_obj_t *l = lv_label_create(s_scr);
    lv_obj_set_name_static(l, name);
    lv_obj_add_style(l, (lv_style_t *)st, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    int32_t h = lv_font_get_line_height(lv_obj_get_style_text_font(l, LV_PART_MAIN));
    lv_obj_set_size(l, w, h);
    lv_obj_set_pos(l, 180 - w / 2, cy - h / 2);
    return l;
}

lv_obj_t *scr_ambient_build(uint32_t arg)
{
    (void)arg;
    if (!getenv("TZ")) {
        // the prompt builder sets it on the first turn; the clock may come first
        setenv("TZ", "MSK-3", 1);
        tzset();
    }
    s_scr = ui_chrome_screen();
    lv_obj_set_name_static(s_scr, "ambient");
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scr, press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_scr, delete_cb, LV_EVENT_DELETE, NULL);
    s_clock = centred("amb_clock", &st_text48, 166, CLOCK_W);
    s_date = centred("amb_date", &st_text16, 212, DATE_W);
    lv_obj_t *orb = ui_orb_create(s_scr, ORB_AMBIENT);
    lv_obj_set_name_static(orb, "amb_orb");
    lv_obj_remove_flag(orb, LV_OBJ_FLAG_CLICKABLE);
    ui_orb_set_mode(orb, ui_get(g_s_mode));
    ui_orb_set_face(orb, false);
    s_last_min = -1;
    refresh();
    s_timer = lv_timer_create(timer_cb, 1000, NULL);
    return s_scr;
}
