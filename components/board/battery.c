// Battery voltage on GPIO8 (ADC1 channel 7) behind the board's x3 divider (docs/hardware.md), and the battery
// model of docs/ui.md 3.16: a median of 5 readings taken 5 s apart (readings under load are not taken), a Li-ion
// discharge curve, and the power state from the USB host and the direction of the voltage. The ETA6098 charger only
// drives an LED (no status pin reaches the MCU) and the system load hangs on the cell, so a PC port that gives less
// than the speaker takes drains the cell while it is "on the charger": only the voltage can tell.
#include "board.h"
#include "board_pins.h"

#include <stdbool.h>
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "battery";

#define N_READS     8
#define DIVIDER     3

#define MEDIAN_N        5           // readings in the median (25 s at the app's 5 s period, longer when some are skipped)
#define TREND_N         24          // 2 min of calm filtered readings
#define TREND_CALM_MS   120000      // calm = settled and this long after the last reading that was not (3 min after
                                    // a load in all): a cell recovers upwards for minutes after a long session
#define TREND_UP_MV     15          // a rise this big within 2 min below FULL_MV: a charger without a USB host
#define TREND_FALL_MV   10          // a fall this big within 2 min ends it
#define PEAK_DROP_MV    25          // ... and so does a settled reading this far below the highest one since (unplugged)
#define PEAK_DROP_BUSY_MV 60        // ... or any reading this far below it (unplugged while in use: a turn's sag on
                                    // the charger stays well under this)
#define WIN_N           64          // filtered readings with their time: 5.3 min at 5 s, longer with skipped readings
#define WIN_MS          180000      // the drain / hold window: now against the newest reading at least this old
#define RECENT_MS       60000       // "now" = the highest of the last minute's settled readings (the cell has
#define RECENT_MIN      3           // recovered from the last load), at least this many of them spread over at
#define RECENT_SPAN_MS  30000       // least this: one low reading is no trend
#define DRAIN_MV        20          // USB host present and now this much below 3 min ago: USB_DRAIN; or, a slow
#define DRAIN_PEAK_MV   30          // drain (a weak port), this much below the highest level since charging began
#define DRAIN_END_MV    12          // ... until a settled reading this far above the lowest one since,
#define DRAIN_MIN_MS    120000      // ... at the earliest this long after it began (a cell recovering from the load
                                    // rises too: no flapping, and the note stays long enough to be read)
#define RELAX_MS        180000      // unplugged: the cell's polarisation relaxes downwards for minutes, so the
                                    // percent does not rise at all for this long
#define HOLD_MV         8           // at >= FULL_MV and less than this above 3 min ago: holding (the CV phase): FULL
#define FULL_MV         4150
#define FULL_HYST_MV    40          // full is left only below FULL_MV - this (a CV-phase cell hovers at the threshold,
                                    // and the charger tops it up after its termination)
// Charging offset: the cell reads higher while current flows into it (the charge current through its internal
// resistance, plus the polarisation that relaxes after the cable is pulled). The percent is read from the curve at
// mv - offset on external power (CHARGING, and USB_DRAIN too: its readings are taken between loads, when the port
// charges again), else the display would drop 5-10 points (more in the flat 3.80-3.95 V part) on unplugging.
// The offset is the step the plug-in itself made: the first readings on USB against the last settled level on
// battery (at most 10 min old, and taken once the last charge's polarisation had relaxed, RELAX_MS after
// unplugging), measured again once the polarisation has built up (the first settled reading 60 s after plugging in,
// or whatever was measured by 3 min: later readings would count the charge gained as offset); it differs per port
// (a weak one barely raises the cell, a 1 A one more than a PC port). A wall charger is measured against the calm
// reading its trend's rise started from (2 min before it was found, before the step). Plugged in again within RELAX_MS
// of unplugging, the last session's offset is kept (the cell still reads high from it: no fresh reference).
// Without a reference (booted on USB) CHG_OFS_MV is used: a PC port's ~0.35 A net into the cell x ~0.15 ohm plus
// the polarisation, an estimate for this board (docs/hardware.md 10 says how to check it). The offset is not
// tapered towards 4.20 V: in the CV phase the reading stops rising while the current falls, so the reading cannot
// tell how far the taper should go, and the charger keeps feeding the board's own load there anyway; the CC phase
// up to 4.20 V needs the whole offset (else the percent runs ahead and drops on unplugging). FULL shows 100.
#define CHG_OFS_MV      60
#define CHG_OFS_MAX_MV  200         // a 1-1.5 A charger: ~1.2 A x 0.15 ohm plus the polarisation (the references are
                                    // taken before the step, so a measured step only errs on the small side)
#define OFS_SETTLE_MS   60000
#define OFS_WINDOW_MS   180000      // ... measured only this long after plugging in (then kept as it is)
#define OFS_REF_MAX_MS  600000
#define PCT_AGAINST     5           // on battery the percent rises only by this much at once and on a calm reading
                                    // (a cell recovering from a load reads higher for a while)
#define NOBAT_SPREAD_MV 250         // settled readings this far apart: no cell, a charger output swinging on its own
#define NOBAT_CLEAR_MV  80          // ... back to a cell once they agree within this

typedef struct {
    uint32_t t;                     // ms since boot
    int mv;
    bool settled;
} win_t;

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
static bool s_tried, s_ok;

// model (app task), published under s_mux
static int s_raw[MEDIAN_N], s_raw_n, s_raw_i;
static int s_hist[TREND_N], s_hist_n, s_hist_i;
static bool s_trend, s_nobat;
static int s_peak;
static int s_trend_base = -1;       // the reading the trend's rise was measured from (-1 = none)
static uint32_t s_busy_t;           // the last reading that was not SETTLED (skipped or loaded), 0 = none yet
static win_t s_win[WIN_N];
static int s_win_n, s_win_i;
static int s_trough;                // lowest filtered reading since USB_DRAIN began
static int s_level_peak = -1;       // highest settled level (the window's "now") since the power state began
static uint32_t s_power_t;          // the current power state began (ms since boot)
static int s_bat_ref = -1;          // last settled level on battery, and when
static uint32_t s_bat_ref_t;
static uint32_t s_unplug_t;         // the BATTERY state began after external power (0 = not since boot)
static uint32_t s_plug_t;           // ... and it ended (plugged in, or a wall charger found)
static int s_ofs_ref = -1;          // ... as the reference of this charging session's offset (-1 = none)
static int s_ofs = CHG_OFS_MV;      // charging offset in use
static bool s_ofs_final;            // measured after the polarisation built up
static bool s_ofs_usb;              // ... on a USB host (else a wall charger)
static bool s_started, s_usb_last;
static unsigned s_skipped;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static board_bat_t s_model = { .pct = -1, .mv = -1, .raw = -1, .ofs = CHG_OFS_MV };
static bool s_sim_on;
static int s_sim_pct;
static board_power_t s_sim_pwr;

static void bat_init(void)
{
    s_tried = true;
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(BOARD_PIN_BAT_ADC, &unit, &s_chan) != ESP_OK || unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "GPIO%d is not an ADC1 pin", BOARD_PIN_BAT_ADC);
        return;
    }
    adc_oneshot_unit_init_cfg_t u = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&u, &s_adc) != ESP_OK) {
        ESP_LOGE(TAG, "ADC1 unavailable");
        return;
    }
    adc_oneshot_chan_cfg_t c = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_oneshot_config_channel(s_adc, s_chan, &c) != ESP_OK) {
        ESP_LOGE(TAG, "channel config");
        return;
    }
    adc_cali_curve_fitting_config_t cc = {
        .unit_id = ADC_UNIT_1, .chan = s_chan, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cc, &s_cali) != ESP_OK) {
        s_cali = NULL;      // no calibration eFuses: the raw scale below is about +-10 %
        ESP_LOGW(TAG, "no ADC calibration, using the raw scale");
    }
    s_ok = true;
}

int board_battery_mv(void)
{
    if (!s_tried) {
        bat_init();
    }
    if (!s_ok) {
        return -1;
    }
    int sum = 0, n = 0;
    for (int i = 0; i < N_READS; i++) {
        int v = 0;
        esp_err_t e = s_cali ? adc_oneshot_get_calibrated_result(s_adc, s_cali, s_chan, &v)
                             : adc_oneshot_read(s_adc, s_chan, &v);
        if (e == ESP_OK) {
            if (!s_cali) {
                v = v * 3100 / 4095;    // 12 dB attenuation spans about 0..3.1 V
            }
            sum += v;
            n++;
        }
    }
    return n ? sum / n * DIVIDER : -1;
}

int board_battery_pct(int mv)
{
    // single Li-ion / LiPo cell at a light load (the board draws 0.1-0.3 A): flat between 3.75 and 3.95 V, steep at
    // both ends; linear only between these points
    static const struct {
        int mv, pct;
    } t[] = { { 3300, 0 },  { 3450, 2 },  { 3610, 5 },  { 3690, 10 }, { 3710, 15 }, { 3730, 20 }, { 3750, 25 },
              { 3770, 30 }, { 3790, 35 }, { 3800, 40 }, { 3820, 45 }, { 3840, 50 }, { 3850, 55 }, { 3870, 60 },
              { 3910, 65 }, { 3950, 70 }, { 3980, 75 }, { 4020, 80 }, { 4080, 85 }, { 4110, 90 }, { 4150, 95 },
              { 4200, 100 } };
    if (mv < 2800 || mv > 4400) {
        return -1;
    }
    if (mv <= t[0].mv) {
        return 0;
    }
    for (size_t i = 1; i < sizeof(t) / sizeof(t[0]); i++) {
        if (mv <= t[i].mv) {
            return t[i - 1].pct + (mv - t[i - 1].mv) * (t[i].pct - t[i - 1].pct) / (t[i].mv - t[i - 1].mv);
        }
    }
    return 100;
}

const char *board_power_name(board_power_t p)
{
    static const char *const n[] = { "battery", "charging", "full", "drain" };
    return (unsigned)p < BOARD_PWR__N ? n[p] : "?";
}

// Median of the raw readings; *spread = their max - min
static int median(int *spread)
{
    int v[MEDIAN_N] = { 0 };
    for (int i = 0; i < s_raw_n; i++) {
        int x = s_raw[i], j = i;
        for (; j > 0 && v[j - 1] > x; j--) {
            v[j] = v[j - 1];
        }
        v[j] = x;
    }
    *spread = v[s_raw_n - 1] - v[0];
    return v[s_raw_n / 2];
}

// Charging without a USB host (a wall charger): the filtered voltage rising. A load step looks the same (the cell
// recovers after a turn's speech, for minutes after a long session, and when the backlight goes off), so only calm
// readings (settled, app_io.c, and TREND_CALM_MS after the last one that was not) enter the 2 min history and may
// start it. Every reading may end it.
static void trend(int mv, bool settled, bool calm)
{
    if (!settled) {
        if (s_trend && s_peak - mv >= PEAK_DROP_BUSY_MV) {
            s_trend = false;
        }
        return;
    }
    if (calm) {
        // the calm reading 2 min (24 calm readings) ago is the one about to be overwritten
        int old = s_hist_n == TREND_N ? s_hist[s_hist_i] : -1;
        s_hist[s_hist_i] = mv;
        s_hist_i = (s_hist_i + 1) % TREND_N;
        if (s_hist_n < TREND_N) {
            s_hist_n++;
        }
        if (old > 0 && !s_trend && mv - old >= TREND_UP_MV && mv < FULL_MV) {
            s_trend = true;
            s_peak = mv;
            s_trend_base = old;         // the charging offset's reference (offset_track)
        } else if (old > 0 && s_trend && old - mv >= TREND_FALL_MV) {
            s_trend = false;
        }
    }
    if (s_trend && mv > s_peak) {
        s_peak = mv;
    } else if (s_trend && s_peak - mv >= PEAK_DROP_MV) {
        s_trend = false;
    }
}

static void win_push(uint32_t now, int mv, bool settled)
{
    s_win[s_win_i] = (win_t){ now, mv, settled };
    s_win_i = (s_win_i + 1) % WIN_N;
    if (s_win_n < WIN_N) {
        s_win_n++;
    }
}

// The window: *hi = the highest of the last minute's settled readings (at least RECENT_MIN of them over at least
// RECENT_SPAN_MS, so a fall must last and a cell still recovering from a load does not count; -1 = not enough), *ref =
// the newest reading of any kind at least 3 min old (older when readings were skipped meanwhile: a fall over a longer
// time is still a fall; a reference taken under load only makes a fall smaller; -1 = none)
static void win_scan(uint32_t now, int *hi, int *ref)
{
    int n_recent = 0;
    uint32_t newest = 0, oldest = 0;
    *hi = *ref = -1;
    for (int k = 0; k < s_win_n; k++) {
        const win_t *w = &s_win[(s_win_i - 1 - k + WIN_N) % WIN_N];     // newest first
        uint32_t age = now - w->t;
        if (age <= RECENT_MS) {
            if (!w->settled) {
                continue;
            }
            if (!n_recent++) {
                newest = w->t;
            }
            oldest = w->t;
            if (w->mv > *hi) {
                *hi = w->mv;
            }
        } else if (age >= WIN_MS) {
            *ref = w->mv;
            break;
        }
    }
    if (n_recent < RECENT_MIN || newest - oldest < RECENT_SPAN_MS) {
        *hi = -1;
    }
}

static void win_clear(void)
{
    s_win_n = s_win_i = 0;
    s_level_peak = -1;
}

// Plugged in or out: the voltage steps by the charge current's drop, so the median, the wall-charger trend and the
// window start over with readings of the new regime
static void regime_reset(void)
{
    s_raw_n = s_raw_i = 0;
    s_hist_n = s_hist_i = 0;
    s_trend = false;
    win_clear();
}

// The charging offset of this session (see CHG_OFS_MV): after the power state is decided for a reading
static void offset_track(uint32_t now, board_power_t was, board_power_t pw, int mv, bool fresh, bool settled)
{
    if (pw == BOARD_PWR_BATTERY) {
        if (was != BOARD_PWR_BATTERY) {
            s_unplug_t = now ? now : 1;
        }
        // a reference once the last charge's polarisation has relaxed (right after unplugging the cell still reads
        // high: a step measured from there would come out too small)
        if (fresh && settled && !s_nobat && (!s_unplug_t || now - s_unplug_t >= RELAX_MS)) {
            s_bat_ref = mv;
            s_bat_ref_t = now;
        }
        return;
    }
    if (was == BOARD_PWR_BATTERY) {
        // plugged in (or a wall charger found)
        if (s_ofs_final && s_ofs_usb == s_usb_last && s_unplug_t && now - s_unplug_t < RELAX_MS) {
            return;     // back on the same kind of source before the cell relaxed: the last offset still holds
        }
        s_ofs = CHG_OFS_MV;
        s_ofs_final = false;
        s_ofs_usb = s_usb_last;
        s_plug_t = now;
        if (s_usb_last) {
            // the step is measured against the level just before
            s_ofs_ref = s_bat_ref > 0 && now - s_bat_ref_t <= OFS_REF_MAX_MS ? s_bat_ref : -1;
        } else {
            // a wall charger: against the calm reading its rise was measured from (before the step; when that is
            // older, the drain since only makes the step come out smaller)
            s_ofs_ref = s_trend_base;
        }
    }
    if (pw == BOARD_PWR_CHARGING && fresh && s_ofs_ref > 0 && !s_ofs_final) {
        if (now - s_plug_t > OFS_WINDOW_MS) {
            s_ofs_final = true;     // no settled reading in time (the screen or turns kept loading the cell): later
            return;                 // readings would count the charge since as offset
        }
        int step = mv - s_ofs_ref;
        s_ofs = step < 0 ? 0 : step > CHG_OFS_MAX_MV ? CHG_OFS_MAX_MV : step;   // the ohmic step at once, the
                                                                                // polarisation within a minute
        s_ofs_final = settled && now - s_plug_t >= OFS_SETTLE_MS;
    }
}

// The power state after a reading (mv = the filtered level; hi / ref = the window, win_scan(); settled_now = this
// reading is a settled one)
static board_power_t decide(uint32_t now, board_power_t was, bool usb, int mv, int hi, int ref, bool settled_now)
{
    bool d_ok = hi >= 0 && ref >= 0;
    int d = hi - ref;
    if (s_nobat || mv < 0 || !(usb || s_trend)) {
        return BOARD_PWR_BATTERY;
    }
    switch (was) {
    case BOARD_PWR_BATTERY:
        return BOARD_PWR_CHARGING;      // just plugged in (or the wall-charger trend began): charging until shown not
    case BOARD_PWR_USB_DRAIN:
        // ends on a settled rise from the lowest point (the load stopped and the port catches up), or when the USB
        // host went away and the wall-charger trend found a charger
        if (!usb || (now - s_power_t >= DRAIN_MIN_MS && settled_now && mv >= s_trough + DRAIN_END_MV)) {
            return BOARD_PWR_CHARGING;
        }
        return BOARD_PWR_USB_DRAIN;
    case BOARD_PWR_FULL:
        return mv >= FULL_MV - FULL_HYST_MV ? BOARD_PWR_FULL : BOARD_PWR_CHARGING;
    default:
        break;
    }
    if (usb && ((d_ok && d <= -DRAIN_MV) || (hi >= 0 && s_level_peak - hi >= DRAIN_PEAK_MV))) {
        return BOARD_PWR_USB_DRAIN;     // a wall charger's fall ends its trend instead (unplugged or not: unknown)
    }
    if (mv >= FULL_MV && d_ok && d < HOLD_MV) {
        return BOARD_PWR_FULL;
    }
    return BOARD_PWR_CHARGING;
}

// What the UI sees: the model, or the console's override
static board_bat_t effective(void)
{
    board_bat_t b = s_model;
    if (s_sim_on) {
        b.power = s_sim_pwr;
        b.pct = s_sim_pwr == BOARD_PWR_FULL ? 100 : s_sim_pct;
        b.sim = true;
    }
    b.charging = b.power == BOARD_PWR_CHARGING;
    b.full = b.power == BOARD_PWR_FULL;
    return b;
}

static bool same(const board_bat_t *a, const board_bat_t *b)
{
    return a->pct == b->pct && a->power == b->power && a->usb == b->usb;
}

bool board_battery_sample(board_bat_q_t q)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    bool usb = usb_serial_jtag_is_connected();
    if (s_started && usb != s_usb_last) {
        regime_reset();
    }
    s_started = true;
    s_usb_last = usb;
    if (q != BOARD_BAT_SETTLED) {
        s_busy_t = now ? now : 1;       // the calm clock of the wall-charger trend
    }
    // s_model is written only here (this task): no lock needed to read it
    const board_bat_t *p = &s_model;
    board_bat_t m = *p;             // a skipped reading keeps the level
    m.usb = usb;
    bool fresh = false, settled = q == BOARD_BAT_SETTLED;
    bool calm = settled && (!s_busy_t || now - s_busy_t >= TREND_CALM_MS);
    if (q == BOARD_BAT_SKIP && p->mv >= 0) {
        // the speaker or a turn's Wi-Fi draws more than the port gives: the cell sags for the moment, and the
        // reading would show a drop the settled cell does not have (the first reading is taken whatever the load,
        // so the indicator appears at once)
        s_skipped++;
    } else {
        int raw = board_battery_mv();
        m.raw = raw;
        if (raw > 0) {
            s_raw[s_raw_i] = raw;
            s_raw_i = (s_raw_i + 1) % MEDIAN_N;
            if (s_raw_n < MEDIAN_N) {
                s_raw_n++;
            }
            int spread;
            int mv = median(&spread);
            if (settled && s_raw_n == MEDIAN_N) {
                // a resting cell's readings agree within a few tens of mV; with no cell the charger's output swings
                // on its own (no indicator rather than a false "charging")
                s_nobat = spread > NOBAT_SPREAD_MV || (s_nobat && spread >= NOBAT_CLEAR_MV);
            }
            trend(mv, settled && !s_nobat, calm && !s_nobat);
            win_push(now, mv, settled);
            m.mv = mv;
            fresh = true;
        }
    }
    int hi, ref;
    win_scan(now, &hi, &ref);
    if (hi > s_level_peak) {
        s_level_peak = hi;
    }
    m.d3_ok = hi >= 0 && ref >= 0;
    m.d3 = m.d3_ok ? hi - ref : 0;
    m.power = decide(now, p->power, usb, m.mv, hi, ref, fresh && settled);
    if (m.power != p->power) {
        // a new state starts its own window: a fall that began before it does not count again
        win_clear();
        m.d3_ok = false;
        s_trough = m.mv;
        s_power_t = now;
    } else if (m.power == BOARD_PWR_USB_DRAIN && fresh && m.mv < s_trough) {
        s_trough = m.mv;
    }
    offset_track(now, p->power, m.power, m.mv, fresh, settled);
    m.ofs = s_ofs;
    if (s_nobat || m.mv < 0) {
        m.pct = -1;
    } else if (m.power == BOARD_PWR_FULL) {
        m.pct = 100;
    } else if (fresh || p->pct < 0) {
        bool fed = m.power == BOARD_PWR_CHARGING || m.power == BOARD_PWR_USB_DRAIN;
        int pct = board_battery_pct(fed ? m.mv - s_ofs : m.mv);
        int was = p->pct;
        if (pct >= 0 && was >= 0) {
            if (m.power == BOARD_PWR_CHARGING) {
                pct = pct < was ? was : pct;        // never goes down while charging (a fall shows as USB_DRAIN)
            } else if (m.power == BOARD_PWR_USB_DRAIN || now - s_power_t < RELAX_MS) {
                pct = pct > was ? was : pct;        // nor up while the port cannot keep up, or right after unplugging
            } else if (pct > was && (pct - was < PCT_AGAINST || !calm)) {
                pct = was;                          // on battery: a recovering cell (for minutes after a session),
            }                                       // or a wall charger the trend has not seen yet
        }
        m.pct = pct;
    }
    // else: the power changed on a skipped reading (unplugged while the speaker plays): the level stays as shown
    m.skipped = s_skipped;
    taskENTER_CRITICAL(&s_mux);
    board_bat_t before = effective();
    s_model = m;
    board_bat_t after = effective();
    taskEXIT_CRITICAL(&s_mux);
    return !same(&before, &after);
}

void board_battery_state(board_bat_t *out)
{
    if (!out) {
        return;
    }
    taskENTER_CRITICAL(&s_mux);
    *out = effective();
    taskEXIT_CRITICAL(&s_mux);
}

void board_battery_sim(int pct, board_power_t power)
{
    taskENTER_CRITICAL(&s_mux);
    s_sim_on = pct >= 0;
    s_sim_pct = pct > 100 ? 100 : pct;
    s_sim_pwr = (unsigned)power < BOARD_PWR__N ? power : BOARD_PWR_BATTERY;
    taskEXIT_CRITICAL(&s_mux);
}
