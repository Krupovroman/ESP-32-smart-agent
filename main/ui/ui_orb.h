#pragma once
// The orb with its face (docs/ui.md 5.3, 6.2 A1-A15, 7.4): one transparent lv_obj drawn in
// LV_EVENT_DRAW_MAIN, no children. Its box always contains every pixel it draws.
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef enum { ORB_HERO, ORB_COMPACT, ORB_MINI, ORB_AMBIENT, ORB__N } orb_preset_t;
typedef enum { EXPR_NEUTRAL, EXPR_WIDE, EXPR_LOOK, EXPR_HAPPY, EXPR_SOFT, EXPR_SLEEPY, EXPR__N } orb_expr_t;

// Creates an orb at the preset's position on parent (a full-screen parent at (0,0) is assumed).
lv_obj_t *ui_orb_create(lv_obj_t *parent, orb_preset_t p);
// Moves/resizes to another preset; anim = 320 ms ease_in_out with every parameter scaled (A15 / A15r).
void ui_orb_set_preset(lv_obj_t *orb, orb_preset_t p, bool anim);
orb_preset_t ui_orb_get_preset(lv_obj_t *orb);
// ui_state_t from ui.h: starts the colour / stroke transitions and the 33 ms ticker where needed.
void ui_orb_set_state(lv_obj_t *orb, int32_t ui_state);
// 0 assistant, 1 translator: idle colours morph over 400 ms (A21).
void ui_orb_set_mode(lv_obj_t *orb, int mode);
// Expression override for hold_ms (0 = until the next call or state change).
void ui_orb_set_expr(lv_obj_t *orb, orb_expr_t e, uint32_t hold_ms);
// LLM mood (ui_mood_t) -> expression for 1.5 s.
void ui_orb_set_mood(lv_obj_t *orb, int32_t mood);
// Gaze toward an absolute screen point for hold_ms (touch feedback, new messages).
void ui_orb_look_at(lv_obj_t *orb, lv_point_t p, uint32_t hold_ms);
// Translator: while speaking look toward chip A (0) or B (1); -1 = straight.
void ui_orb_set_target_side(lv_obj_t *orb, int side);
// Face on/off ("Лицо шара").
void ui_orb_set_face(lv_obj_t *orb, bool on);
// UPLOADING progress arc, 0..100.
void ui_orb_set_upload(lv_obj_t *orb, int32_t pct);
// Short press-in pulse (busy-state tap feedback).
void ui_orb_pulse(lv_obj_t *orb);
// Absolute centre and the touch radii of the current preset (hit = core + 10; cancel arms beyond it).
void ui_orb_center(lv_obj_t *orb, lv_point_t *c);
int32_t ui_orb_hit_radius(lv_obj_t *orb);
int32_t ui_orb_cancel_radius(lv_obj_t *orb);
// Name of an expression for the console ("neutral", ...), or NULL.
const char *ui_orb_expr_name(orb_expr_t e);
