#pragma once
// Round-screen geometry (docs/ui.md 0.4). Origin top-left, centre C = (180,180), usable radius 168.
// "x a-b" ranges are left edge a, right edge b exclusive; LVGL inclusive coords convert with b = x2 + 1.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define UI_W        360
#define UI_H        360
#define UI_CX       180
#define UI_CY       180
#define UI_R        168     // nothing visible beyond this radius (12 px bezel margin)
#define UI_MIN_HIT  50      // minimum visible touch target (6.5 mm)

// Angles follow LVGL: 0 deg = 3 o'clock, clockwise (90 = 6 o'clock, 270 = 12 o'clock).

// Distance of (x, y) from the screen centre.
static inline float ui_geom_dist(float x, float y)
{
    return sqrtf((x - UI_CX) * (x - UI_CX) + (y - UI_CY) * (y - UI_CY));
}

// Usable chord width at vertical offset d from the centre: w(d) = 2*sqrt(168^2 - d^2).
static inline float ui_geom_chord(float d)
{
    float r2 = (float)UI_R * UI_R - d * d;
    return r2 > 0 ? 2.0f * sqrtf(r2) : 0.0f;
}

// Farthest-corner radius of the rectangle x a..b, y c..d (b, d exclusive).
static inline float ui_geom_rmax_rect(int32_t a, int32_t c, int32_t b, int32_t d)
{
    float dx = fmaxf(fabsf((float)a - UI_CX), fabsf((float)b - UI_CX));
    float dy = fmaxf(fabsf((float)c - UI_CY), fabsf((float)d - UI_CY));
    return sqrtf(dx * dx + dy * dy);
}

// Rounded rectangle / stadium: farthest corner-circle centre plus the corner radius.
static inline float ui_geom_rmax_rrect(int32_t a, int32_t c, int32_t b, int32_t d, int32_t rc)
{
    int32_t half = ((b - a) < (d - c) ? (b - a) : (d - c)) / 2;
    if (rc > half) {
        rc = half;
    }
    if (rc < 0) {
        rc = 0;
    }
    return ui_geom_rmax_rect(a + rc, c + rc, b - rc, d - rc) + (float)rc;
}

// Circle with centre (cx, cy) and radius r.
static inline float ui_geom_rmax_circle(float cx, float cy, float r)
{
    return ui_geom_dist(cx, cy) + r;
}

// True when an outline with this Rmax stays inside the usable circle (0.05 px rounding slack).
static inline bool ui_geom_fits(float rmax)
{
    return rmax <= (float)UI_R + 0.05f;
}
