// Battery voltage on GPIO8 (ADC1 channel 7) behind the board's x3 divider (docs/hardware.md), and the battery
// model v4 of docs/ui.md 3.16. The ETA6098 charger only drives an LED (no status pin and no VBUS reach the MCU) and
// the system load hangs on the cell, so only the USB host's packets and the cell voltage tell what the power does:
// - External power: the USB host (its start-of-frame packets), or a voltage step: a settled reading 35 mV off the
//   settled readings just before it, confirmed by the next two (taken 2 s apart). Plugging in makes the cell step up
//   by the charge current x its internal resistance (in the CV phase: to the charger's regulation level); a full cell
//   (the charger terminated or never started, its LED at "full") does not step. A step down without the host =
//   unplugged; the host's packets stopping without one = still plugged in (macOS suspends an idle CDC device: no
//   packets, VBUS still there).
// - v4 (field report: "the percent rises, no bolt"): a plug-in usually comes with a load step (the screen woken, a
//   turn) that breaks the run, and a charger's soft start spreads the step over several readings, so the step was
//   never measured. The settled level is also measured against the levels before it, across a load (step_base), and
//   calm minutes rising steadily are a charger in its CC phase (trend_slow: booted on it, a step missed); on battery
//   the percent never rises. v4 review: a rise only back to the levels of a few minutes before is no charger (a load
//   ended), a trend's charger without the charge's further rise is dropped (no_rise), the percent follows the cell
//   down in constant use too, and an unplug under use is seen within 1-2 min.
// - Charging or full: the plug-in step (under 15 mV near full = the charger did not start: FULL), then the settled
//   level: flat for 4 min near v_full (15 min anywhere >= 3.95 V) = FULL, whatever the absolute voltage (a FULL's
//   highest level is learned as v_full, the 100 % of this divider), rising again = CHARGING. With the host a calm
//   fall near v_full is the charger stopping (FULL), and FULL holds while the cell relaxes and drains towards the
//   recharge level after termination.
// - USB_DRAIN: the host there and the settled level falling below near-full or across a long session (a PC port
//   gives less than the loud speaker takes), or FULL's level under the recharge level.
// Readings under load are not taken (app_io.c grades them), leaving any state the percent glides to the curve rather
// than jumping, and every decision goes into the battery log (bat log).
#include "board.h"
#include "board_pins.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "battery";

#define N_READS     8
#define DIVIDER     3

#define POLL_MS         5000        // the reading period the model asks for (board_bat_t.poll_ms) ...
#define FAST_POLL_MS    2000        // ... and while a step or a plug-in is being measured,
#define FAST_WIN_MS     16000       // ... for this long after it began
#define MEDIAN_N        5           // readings in the level's median (25 s at 5 s, longer when some are skipped)

// Steps. The run: the settled raw readings in a row (none more than STEP_GAP_MS apart, no reading that was not
// settled between them: a load change is no step); its median is the level a step is measured from.
#define RUN_N           5
#define STEP_GAP_MS     30000
#define STEP_MV         35          // external power came or went (tunable: a backlight or radio change is < 25 mV)
#define STEP_SMALL_MV   15          // a reading this far off opens a candidate; a smaller confirmed step than STEP_MV
#define STEP_SMALL_GAP_MS 120000    // is only logged (evidence: what a full cell's plug-in makes), one this often
#define STEP_CONFIRM_N  3           // a reading that far off and the next two settled ones: their median
#define STEP_EDGE_MV    28          // v4: ... a step up also this far above the run's second highest reading, a step
                                    // down below its second lowest (Wi-Fi bursts read low and come in clusters)
#define DIP_MS          600000      // a step up back to within STEP_SMALL_MV of the level before a step down on
                                    // battery this recent: a load the app does not grade ended, not a charger. Only
                                    // a step down seen while settled arms it: comparing with older levels would
                                    // take a near-full cell's small charger step (it drained meanwhile) or a weak
                                    // charger after a long session for a load's end (the app grades the loads it
                                    // knows: the speaker, turns, the backlight, Wi-Fi and the portal)
#define DIP_OPEN_MS     1800000     // v4 review: the trends do not take a rise back to under BASE_MV above the level
#define DIP_BACK_MV     8           // before such a dip either, while the cell still reads low (at most this long
                                    // after the dip) and DIP_MS after a settled reading back within DIP_BACK_MV

// Plugged in with a USB host: the step against the run's level before it, from the first PEND_N settled readings
#define PEND_N          3
#define PEND_MS         15000       // ... at most this long, else CHARGING without a measurement
#define NOSTEP_MV       15          // a smaller step: the charger did not start (or stopped at once): FULL when the
#define NOSTEP_FULL_MV  120         // level is at least v_full - this (and FULL_MIN_MV), else CHARGING as by default

// The host's packets stopped: a step down within LOST_WATCH_MS = unplugged; none in LOST_SETTLED_N settled readings
// (or none to look in) = still plugged in (EXT_KEPT)
#define LOST_WATCH_MS   12000
#define LOST_SETTLED_N  2
#define USB_EV_GAP_MS   120000      // the host back while power was already seen: logged at most this often (a
                                    // flickering host would flood the log)

// External power without a host ends on a fall: a settled level this far under the highest one since it began (or
// since the host went), FULL_LOST_MV in FULL (after the charger's termination the cell relaxes by 30-50 mV and the
// load drains it to the recharge level: not unplugged), or any reading EXT_FALL_BUSY_MV under it
#define EXT_FALL_MV     35
#define FULL_LOST_MV    60
#define EXT_FALL_BUSY_MV 60
#define BL_MV_X10       3           // v4 review: the backlight's sag, 0.3 mV per % (at most BASE_BL_MV), allowed for
                                    // when it is higher than at the highest level (the screen woken to look)

// The wall-charger fallback (a step missed under load): calm readings rising TREND_UP_MV within TREND_N of them, all
// calm in a row (a history spanning a load compared with a reading taken while the cell was still low after an
// earlier session: v3's false chargers)
#define TREND_N         24          // 2 min of calm readings at 5 s
#define TREND_CALM_MS   120000      // calm = settled and this long after the last reading that was not (3 min after
                                    // a load in all): a cell recovers upwards for minutes after a long session
#define TREND_UP_MV     15
#define TREND_ENDS      12          // ... the newest 12 raw readings against the oldest 12 (upper quartiles),
#define TREND_HOLD_N    6           // ... on 6 readings in a row
#define CEIL_MIN_MS     180000      // v4 review: ... and above the settled levels of 3-12 min ago (their 90th
#define CEIL_MAX_MS     720000      // percentile) by CEIL_MV: on battery a level only comes back to where it was
#define CEIL_MV         8           // before (a load, even one too small to be seen as a step, ended)
#define TREND_EXIT_MS   1800000     // v4 review: a charger found by a trend (no step) whose first calm level this
#define TREND_EXIT_MV   9           // long after has not risen this much (below full_floor(), no long session
                                    // meanwhile: a CC charge rises 1-4 mV/min) was none (NO_RISE): the end of a load
                                    // the app does not grade, a cell warming up, noise

// v4, the baseline: a soft-starting charger raises the cell over 10-90 s (each reading less than a step above the
// last, so the run follows it), and a plug-in usually comes with a load step (the screen woken to look, a turn, the
// dimming): the run breaks and starts again at the charging level. So the settled level (the median of the last 3
// settled readings) is also measured against the levels before it: those of the 2 min before the newest one at least
// BASE_MIN_MS old, or, across a load, of the 2 min before that load (at most BASE_MAX_MS ago). On battery the cell
// cannot read higher than it did before at the same load: after a load it recovers towards that level from below.
// Levels taken with more backlight than now (it read lower then) count BASE_BL_MV higher (a backlight change is under
// 25 mV). Their lower quartile (a burst of Wi-Fi reads low) 35 mV under the level now, on two readings in a row = a
// charger (step_base).
#define BASE_N          40          // levels kept (one per settled reading: 3.3 min at 5 s)
#define BASE_MIN_MS     12000       // the newest level counted is this old (a ramp's top is not its own baseline)
#define BASE_WIN_MS     120000      // ... with the levels up to this much older than it
#define BASE_MAX_MS     1200000     // ... at most this old (a long session and the minute after it; meanwhile the
                                    // load drains the cell: an older level only makes the rise smaller)
#define BASE_MIN_N      3
#define BASE_MV         STEP_MV
#define BASE_HITS       2
#define BASE_BL_MV      25
#define BASE_PIN_MS     300000      // v4 review: the second hit within this of the first, against its baseline

// v4, the slow trend: a charger in its CC phase raises the cell steadily (1-4 mV/min) where the step was missed
// (booted on it, plugged in under a long session, a low-IR cell's small step). On battery the calm level can only
// rise while the cell recovers from a load (or warms up), and that rise decays. So the medians of whole calm minutes
// in a row: SLOW_UP_MV over SLOW_MIN_MIN-SLOW_N of them, the later half rising at least 3/4 as fast as the earlier
// half (and SLOW_HALF_MV; v4 review: half as fast took a warming cell's 25 mV in 15 min), the level now the highest,
// and the first minute SLOW_HEAVY_MS after a long session (its slow recovery) = a charger (trend_slow). The charging
// offset is then unknown (the step happened long before): 60 mV.
#define SLOW_N          16
#define SLOW_MIN_MIN    6
#define SLOW_UP_MV      12
#define SLOW_HALF_MV    4
#define SLOW_MIN_READS  6           // calm readings in a minute for its median (12 at 5 s)
#define SLOW_HEAVY_MS   600000
#define SLOW_BUCKET_N   32
#define SLOW_HOLD_N     2

// FULL: CHARGING whose calm settled level (the highest of a minute's settled readings, against the same of the
// minute that ended PLATEAU_MS ago) has not risen more than PLATEAU_RISE_MV (nor fallen more than PLATEAU_FALL_MV:
// that is a drain) for PLATEAU_HOLD_MS, at >= v_full - PLATEAU_BAND_MV (and >= FULL_MIN_MV). A slow charge at
// 2 mV/min (a big cell on a PC port) is only 8 mV in 4 min, so a lower plateau must last PLAT_LONG_MS (a charge
// rises far more in that time) before it counts, whatever v_full is: a false FULL would also teach a wrong v_full.
#define FULL_MIN_MV     3950
#define PLATEAU_MS      240000
#define PLATEAU_RISE_MV 5
#define PLATEAU_FALL_MV 15
#define PLATEAU_HOLD_MS 30000
#define PLATEAU_BAND_MV 100
#define PLAT_LONG_MS    900000
#define PLAT_LONG_RISE_MV 10
#define FULL_DWELL_MS   180000      // FULL: a slow rise ends it only after this long (a confirmed step at once):
#define FULL_RISE_MV    20          // ... a calm level this far above the lowest since FULL began and half of it
                                    // within PLATEAU_MS: the charger started again (a wobble is no rise)
#define FULL_HOLD_MV    160         // ... with a host, FULL holds down to v_full - this: after its termination the
                                    // charger lets the load drain the cell to its recharge level (~100-150 mV under
                                    // the CV level) and starts again (a step up); lower, it does not: USB_DRAIN.
                                    // Without a host the fall above ends the external power. A fall while CHARGING
                                    // with a host is the charger stopping too (terminated: the cell relaxes 30-80 mV
                                    // and the load moves onto it): FULL at >= full_floor(), else USB_DRAIN

// v_full: the divider's reading of a full cell on the charger (the CV level: the highest it reads on the charger);
// the curve is scaled by CURVE_FULL_MV / v_full. Learned once a FULL has lasted VFULL_AFTER_MS, from the highest
// settled level during it: lower only when that FULL began with a 4-min plateau near v_full (the CV level), by at
// most VFULL_DOWN_MV per charge (a slow charge's pause or a weak port that only holds the cell is no CV level: one
// such plateau cannot pull the scale far, and the next real charge's CV level lifts it back); higher from any FULL
// (no reading on the charger is above the CV level), also when it ends. Stored by the app (bat_vfull, one NVS
// write per change of at least VFULL_STEP_MV).
#define VFULL_DEF_MV    4150
#define VFULL_MIN_MV    4000
#define VFULL_MAX_MV    4250
#define VFULL_STEP_MV   8
#define VFULL_DOWN_MV   30
#define VFULL_AFTER_MS  900000
#define CURVE_FULL_MV   4200

// The window (USB_DRAIN, FULL): filtered readings with their time
#define WIN_N           256         // 21 min at 5 s (fast readings come in short bursts)
#define WIN_MS          180000      // the drain window: now against the newest reading at least this old
#define RECENT_MS       60000       // "now" = the highest of the last minute's settled readings (the cell has
#define RECENT_MIN      3           // recovered from the last load), at least this many of them spread over at
#define RECENT_SPAN_MS  30000       // least this: one low reading is no trend
#define DRAIN_MV        20          // USB host present and now this much below 3 min ago: USB_DRAIN; or, a slow
#define DRAIN_PEAK_MV   30          // drain (a weak port), this much below the highest level since charging began
                                    // (near full and with no heavy load in that time: the charger stopped, FULL)
#define HEAVY_MS        60000       // readings skipped this long in a row: a heavy load (a loud session, a long
                                    // answer) that can drain the cell on a weak port; not a backlight step's 3 s nor
                                    // a short turn (that coinciding with a charger's termination would read as one)
#define DRAIN_END_MV    12          // ... until a settled reading this far above the lowest one since,
#define DRAIN_MIN_MS    120000      // ... at the earliest this long after it began (a cell recovering from the load
                                    // rises too: no flapping, and the note stays long enough to be read)
#define RELAX_MS        180000      // unplugged: the cell's polarisation relaxes downwards for minutes (no reference
                                    // for the next plug-in's offset meanwhile)

// Charging offset: the cell reads higher while current flows into it (the charge current through its internal
// resistance, plus the polarisation that relaxes after the cable is pulled). The percent is read from the curve at
// mv - offset on external power (CHARGING, and USB_DRAIN too: its readings are taken between loads, when the port
// charges again), else the display would drop 5-10 points (more in the flat 3.80-3.95 V part) on unplugging.
// The offset is the step the plug-in itself made, against the settled level just before (the step detector's run,
// else the last settled level on battery at most 10 min old and taken once the last charge's polarisation had
// relaxed, RELAX_MS after unplugging), measured again until the polarisation has built up (the first settled reading
// 60 s after plugging in, or whatever was measured by 3 min: later readings would count the charge gained as
// offset); it differs per port (a weak one barely raises the cell, a 1 A one more than a PC port). A wall charger
// found by the trend is measured against the calm reading its rise started from. Plugged in again within RELAX_MS of
// unplugging, the last session's offset is kept (the cell still reads high from it: no fresh reference). Without a
// reference (booted on USB) CHG_OFS_MV is used: a PC port's ~0.35 A net into the cell x ~0.15 ohm plus the
// polarisation, an estimate for this board. The offset is not tapered towards full: in the CV phase the reading stops
// rising while the current falls, so the reading cannot tell how far the taper should go. FULL shows 100.
#define CHG_OFS_MV      60
#define CHG_OFS_MAX_MV  200         // a 1-1.5 A charger: ~1.2 A x 0.15 ohm plus the polarisation
#define OFS_SETTLE_MS   60000
#define OFS_WINDOW_MS   180000      // ... measured only this long after plugging in (then kept as it is)
#define OFS_REF_MAX_MS  600000
// v4: on BATTERY the percent never rises (a charger not seen yet must not show as a rising percent without the bolt,
// v3 let it rise 5 points at once on a calm reading). It follows the settled level (3 settled readings in a row) down
// on calm readings (a cell still recovering from a load reads low for minutes), or on any settled one after
// CAP_BUSY_MS without a calm one. v4 review: in constant use there may be no settled reading at all, so CAP_ENV_MS
// without a calm reading it also follows the highest raw reading of the last CAP_ENV_WIN_MS (the most recovered one;
// it errs low by what the cell has not recovered), 1 point per PCT_GLIDE_MS. Until the first settled level after
// boot (the first reading is taken under the boot's load) or a cell found again it follows the curve both ways. At
// <= LOW_PCT readings under load are taken (app_io.c, for the low-battery warnings) and shown when lower, back up to
// the settled value at most.
#define CAP_BUSY_MS     600000
#define CAP_ENV_MS      300000
#define CAP_ENV_WIN_MS  180000
#define ENV_N           36          // the raw readings kept for it (3 min at 5 s)
#define LOW_PCT         20
#define PCT_GLIDE_MS    15000       // entering BATTERY or USB_DRAIN from another state the percent falls to the
                                    // curve by 1 point this often, not at once (FULL showed 100 at any level, the
                                    // charging offset is an estimate): no jump on unplugging or a stop
#define NOBAT_SPREAD_MV 250         // settled readings this far apart: no cell, a charger output swinging on its own
#define NOBAT_CLEAR_MV  80          // ... back to a cell once they agree within this

#define LOG_MIN_MS      60000       // the log's minute record
#define EVQ_N           8           // events of one reading

typedef struct {
    uint32_t t;                     // ms since boot
    int16_t mv;
    bool settled;
} win_t;

typedef struct {
    uint32_t t;
    int16_t mv;                     // the settled level (median of 3)
    uint8_t bl;                     // the backlight then (%)
} base_t;

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
static bool s_tried, s_ok;

// The model (app task only); published as s_model under s_mux
static struct {
    bool started, usb_last;
    board_ext_t ext;
    int raw[MEDIAN_N], raw_n, raw_i;        // the level's median
    bool nobat;
    // step detector
    int run[RUN_N], run_n, run_i;
    int cand[STEP_CONFIRM_N], cand_n, cand_ref, cand_hi, cand_lo;  // the run's second highest and lowest
    uint32_t set_t;                         // the last settled reading (0 = none)
    uint32_t small_t;                       // the last small step logged
    uint32_t dip_t;                         // the last step down on battery (0 = none), and the level before it
    int dip_ref;
    bool dip_open;                          // ... the cell still reads low, else back since dip_end
    uint32_t dip_end;
    uint32_t usb_ev_t;                      // the host-back event last logged
    // plugged in with a host: the step being measured (pend_t 0 = not)
    uint32_t pend_t;
    int pend_ref, pend_v[PEND_N], pend_n;
    int force;                              // board_power_t the measurement decided, -1 = none
    // the host's packets stopped (lost_t 0 = not watching)
    uint32_t lost_t;
    int lost_n;
    bool lost_ok;                           // a level to compare with, and no load since
    int lost_level;                         // the last settled level before
    int set_mv;                             // the last settled level (-1 none)
    int ext_peak;                           // highest settled level of the external power without a host
    int ext_peak_bl;                        // ... the backlight then
    uint32_t plat_t;                        // CHARGING: the plateau holds since (0 = not)
    // wall-charger trend
    int hist[TREND_N], hist_n, hist_i;
    int trend_base;                         // the reading its rise was measured from
    int trend_hold;                         // ... the readings in a row it has held
    uint32_t trend_t;                       // a charger found by a trend, no rise seen since yet (0 = none) ...
    int trend_lvl;                          // ... the level then
    // v4: the last 3 settled readings in a row, the levels on battery and the rises seen against them in a row
    int s3[3], s3_n, s3_i;
    base_t base[BASE_N];
    int base_n, base_i, base_hit, base_b;   // ... and the first hit's baseline, its time and the hit's
    uint32_t base_b_t, base_hit_t;
    // v4: the slow trend's calm minutes (start, median) and the minute being collected
    uint32_t slow_t[SLOW_N];
    int slow_mv[SLOW_N], slow_n, slow_i, slow_hold;
    int sb[SLOW_BUCKET_N], sb_n;
    uint32_t sb_t;
    // v4: the percent on battery: the settled value it may not rise above, and whether it was taken since boot
    int cap, cap_to;                        // ... and the settled value it goes (or glides) down to (-1 none)
    bool synced;
    uint32_t env_t[ENV_N];                  // the last raw readings (constant use: CAP_ENV_MS)
    int env_mv[ENV_N], env_n, env_i;
    uint32_t calm_t;                        // the last calm reading (0 = none)
    uint32_t busy_t;                        // the last reading that was not SETTLED (skipped or loaded), 0 = none yet
    uint32_t skip_since;                    // the skipped readings in a row began (0 = not skipping), and the last
    uint32_t heavy_t;                       // reading of such a run HEAVY_MS long (0 = none)
    // window
    win_t win[WIN_N];
    int win_n, win_i;
    int level_peak;                         // highest settled level (the window's "now") since the state began
    int trough;                             // lowest filtered reading since USB_DRAIN began
    int full_lo, full_hi;                   // lowest calm / highest settled level since FULL began
    bool plat_p4;                           // decide(): the plateau that made FULL was the 4-min one near v_full
    bool full_plat, full_learned;           // this FULL began with it; v_full learned from this FULL
    uint32_t power_t;                       // the current power state began
    bool glide;                             // the percent falls to the curve 1 point per PCT_GLIDE_MS ...
    uint32_t glide_t;                       // ... the last point at
    bool drain_idle;                        // USB_DRAIN because the charger stopped (from FULL, a step down): no
                                            // charge current, so no charging offset either
    // charging offset
    int bat_ref;                            // last settled level on battery, and when
    uint32_t bat_ref_t;
    uint32_t unplug_t;                      // the external power ended (0 = not since boot)
    uint32_t plug_t;                        // ... and began
    int ofs_ref, ofs;
    bool ofs_final, ofs_usb;
    int vfull_learn;
    uint32_t fast_until;
    unsigned skipped;
    // log
    uint32_t log_next;
    int min_mv;                             // the minute's last settled level, -1 = none
    struct {
        uint8_t ev;
        int d;
    } evq[EVQ_N];
    int evq_n;
} s_m;

#define MODEL_INIT                                                                                                  \
    {                                                                                                               \
        .pct = -1, .mv = -1, .raw = -1, .ofs = CHG_OFS_MV, .vfull = VFULL_DEF_MV, .vfull_learn = -1,              \
        .poll_ms = POLL_MS, .why = BOARD_BEV_BOOT                                                                   \
    }

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static board_bat_t s_model = MODEL_INIT;
static int s_vfull = VFULL_DEF_MV;
static bool s_sim_on;
static int s_sim_pct;
static board_power_t s_sim_pwr;
// the log ring (PSRAM, allocated with the first record); seq numbers count from boot
static board_bat_log_t *s_log;
static bool s_log_failed;
static uint32_t s_log_seq, s_log_first;

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

// The percent of a reading of this divider: the curve scaled so that v_full reads as its 4.20 V (a gain error)
static int pct_of(int mv)
{
    if (mv < 2800 || mv > 4400) {
        return -1;
    }
    int v = (int)((int64_t)mv * CURVE_FULL_MV / s_vfull);
    return board_battery_pct(v > 4400 ? 4400 : v);
}

const char *board_power_name(board_power_t p)
{
    static const char *const n[] = { "battery", "charging", "full", "drain" };
    return (unsigned)p < BOARD_PWR__N ? n[p] : "?";
}

const char *board_ext_name(board_ext_t e)
{
    static const char *const n[] = { "none", "usb", "charger", "kept" };
    return (unsigned)e < BOARD_EXT__N ? n[e] : "?";
}

const char *board_bat_ev_name(board_bat_ev_t e)
{
    static const char *const n[] = {
        "min",       "boot",       "usb",      "step",     "no_step", "no_ref",    "usb_lost", "usb_lost_no_step",
        "step_up",   "step_down",  "step_small", "chg_stop", "trend_up", "fall",    "plateau",  "rise",
        "full_exit", "drain",      "drain_end", "nobat",    "cell",     "vfull",   "load_end", "step_base",
        "trend_slow", "no_rise",
    };
    _Static_assert(sizeof(n) / sizeof(n[0]) == BOARD_BEV__N, "event names");
    return (unsigned)e < BOARD_BEV__N ? n[e] : "?";
}

// Median of n (<= 8) values; *spread = their max - min
static int med_of(const int *a, int n, int *spread)
{
    int v[8] = { 0 };
    for (int i = 0; i < n; i++) {
        int x = a[i], j = i;
        for (; j > 0 && v[j - 1] > x; j--) {
            v[j] = v[j - 1];
        }
        v[j] = x;
    }
    if (spread) {
        *spread = v[n - 1] - v[0];
    }
    return v[n / 2];
}

// Sorts n values (insertion: n <= BASE_N)
static void sort_n(int *v, int n)
{
    for (int i = 1; i < n; i++) {
        int x = v[i], j = i;
        for (; j > 0 && v[j - 1] > x; j--) {
            v[j] = v[j - 1];
        }
        v[j] = x;
    }
}

static int16_t clamp16(int v)
{
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

static void fast(uint32_t now)
{
    s_m.fast_until = now + FAST_WIN_MS;
}

static void ev_add(board_bat_ev_t ev, int d)
{
    if (s_m.evq_n < EVQ_N) {
        s_m.evq[s_m.evq_n].ev = (uint8_t)ev;
        s_m.evq[s_m.evq_n].d = d;
        s_m.evq_n++;
    }
}

// ---- the log

static void log_put(const board_bat_log_t *r)
{
    if (!s_log && !s_log_failed) {
        s_log = heap_caps_calloc(BOARD_BAT_LOG_N, sizeof(*s_log), MALLOC_CAP_SPIRAM);
        s_log_failed = !s_log;
        if (!s_log) {
            ESP_LOGW(TAG, "no PSRAM for the battery log");
        }
    }
    if (!s_log) {
        return;
    }
    taskENTER_CRITICAL(&s_mux);
    s_log[s_log_seq % BOARD_BAT_LOG_N] = *r;
    s_log_seq++;
    if (s_log_seq - s_log_first > BOARD_BAT_LOG_N) {
        s_log_first = s_log_seq - BOARD_BAT_LOG_N;
    }
    taskEXIT_CRITICAL(&s_mux);
}

static void log_rec(uint32_t now, board_bat_ev_t ev, int mv, int d, const board_bat_t *m, board_power_t from)
{
    board_bat_log_t r = {
        .t_s = now / 1000, .mv = clamp16(mv), .d = clamp16(d), .pct = (int8_t)m->pct, .ev = (uint8_t)ev,
        .power = (uint8_t)m->power, .from = (uint8_t)from, .usb = m->usb, .ext = (uint8_t)m->ext,
    };
    log_put(&r);
}

uint32_t board_battery_log_range(uint32_t *first)
{
    taskENTER_CRITICAL(&s_mux);
    uint32_t f = s_log_first, n = s_log_seq;
    taskEXIT_CRITICAL(&s_mux);
    if (first) {
        *first = f;
    }
    return n;
}

bool board_battery_log_get(uint32_t seq, board_bat_log_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    bool ok = s_log && seq >= s_log_first && seq < s_log_seq;
    if (ok) {
        *out = s_log[seq % BOARD_BAT_LOG_N];
    }
    taskEXIT_CRITICAL(&s_mux);
    return ok;
}

void board_battery_log_clear(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_log_first = s_log_seq;
    taskEXIT_CRITICAL(&s_mux);
}

// ---- the step detector

static void run_push(int raw)
{
    s_m.run[s_m.run_i] = raw;
    s_m.run_i = (s_m.run_i + 1) % RUN_N;
    if (s_m.run_n < RUN_N) {
        s_m.run_n++;
    }
}

// A reading that was not settled (a load changed): no step is measured across it
static void run_break(void)
{
    s_m.run_n = s_m.run_i = s_m.cand_n = 0;
}

// The settled level before the next reading (a candidate's own reference while one is open), -1 = none fresh
static int run_level(uint32_t now)
{
    if (!s_m.run_n || now - s_m.set_t > STEP_GAP_MS) {
        return -1;
    }
    return s_m.cand_n ? s_m.cand_ref : med_of(s_m.run, s_m.run_n, NULL);
}

// A settled raw reading into the step detector. A reading STEP_SMALL_MV off the run's level opens a candidate (and
// faster readings); the median of it and the next two settled readings must be off the same way, else the first
// was a spike and the others join the run. Returns a confirmed step >= STEP_MV (0 = none; smaller ones are only
// logged) and *ref, the level it was measured from; the run then holds the new level.
static int step_feed(uint32_t now, int raw, int *ref)
{
    if (s_m.run_n && now - s_m.set_t > STEP_GAP_MS) {
        run_break();                        // too long since the last settled reading
    }
    s_m.set_t = now ? now : 1;
    if (s_m.cand_n) {
        s_m.cand[s_m.cand_n++] = raw;
        if (s_m.cand_n < STEP_CONFIRM_N) {
            return 0;
        }
        int n = s_m.cand_n, c0 = s_m.cand[0] - s_m.cand_ref;
        int d = med_of(s_m.cand, n, NULL) - s_m.cand_ref;
        s_m.cand_n = 0;
        if (d >= STEP_MV && d + s_m.cand_ref - s_m.cand_hi < STEP_EDGE_MV) {
            d = STEP_MV - 1;                // v4: a run pulled low by Wi-Fi bursts: no step up against its top
        } else if (d <= -STEP_MV && s_m.cand_lo - (d + s_m.cand_ref) < STEP_EDGE_MV) {
            d = 1 - STEP_MV;                // ... nor a cluster of them a step down against its bottom
        }
        if ((d >= STEP_SMALL_MV && c0 > 0) || (d <= -STEP_SMALL_MV && c0 < 0)) {
            *ref = s_m.cand_ref;
            s_m.run_n = s_m.run_i = 0;
            for (int i = 0; i < n; i++) {
                run_push(s_m.cand[i]);      // the new level
            }
            if (d >= STEP_MV || d <= -STEP_MV) {
                return d;
            }
            if (!s_m.small_t || now - s_m.small_t >= STEP_SMALL_GAP_MS) {
                s_m.small_t = now ? now : 1;
                ev_add(BOARD_BEV_STEP_SMALL, d);
            }
            if (d < 0 && s_m.ext == BOARD_EXT_NONE) {
                s_m.dip_t = now ? now : 1;  // v4: a small dip on battery too (an ungraded load, or a cluster of Wi-Fi
                s_m.dip_ref = s_m.cand_ref; // bursts the run now holds): back up to before it is no charger
                s_m.dip_open = true;
            }
            return 0;
        }
        for (int i = 1; i < n; i++) {
            run_push(s_m.cand[i]);
        }
        return 0;
    }
    if (s_m.run_n) {
        int lvl = med_of(s_m.run, s_m.run_n, NULL);
        if (raw - lvl >= STEP_SMALL_MV || lvl - raw >= STEP_SMALL_MV) {
            int v[RUN_N];
            memcpy(v, s_m.run, sizeof(int) * (size_t)s_m.run_n);
            sort_n(v, s_m.run_n);
            s_m.cand[0] = raw;
            s_m.cand_n = 1;
            s_m.cand_ref = lvl;
            s_m.cand_hi = v[s_m.run_n > 1 ? s_m.run_n - 2 : 0];
            s_m.cand_lo = v[s_m.run_n > 1 ? 1 : 0];
            fast(now);
            return 0;
        }
    }
    run_push(raw);
    return 0;
}

// ---- level, trend, window

static void raw_push(int raw)
{
    s_m.raw[s_m.raw_i] = raw;
    s_m.raw_i = (s_m.raw_i + 1) % MEDIAN_N;
    if (s_m.raw_n < MEDIAN_N) {
        s_m.raw_n++;
    }
}

// The wall-charger fallback, while there is no external power (a plug-in step missed under load): calm raw readings
// rising TREND_UP_MV within 2 min (24 calm readings) below v_full. A load step looks the same (the cell recovers
// after a turn's speech, for minutes after a long session, and when the backlight goes off), so only calm readings
// (settled, app_io.c, and TREND_CALM_MS after the last one that was not) enter the history, and only in a row.
// Returns the rise (0 none).
static bool dip_guard(uint32_t now, int lvl);
static int ceil_lvl(uint32_t now, uint32_t until);

static int trend_up(uint32_t now, int raw, bool calm)
{
    if (!calm) {
        s_m.hist_n = s_m.hist_i = s_m.trend_hold = 0;
        return 0;
    }
    s_m.hist[s_m.hist_i] = raw;
    s_m.hist_i = (s_m.hist_i + 1) % TREND_N;
    if (s_m.hist_n < TREND_N) {
        s_m.hist_n++;
        return 0;
    }
    // the newer half of the raw readings against the older half, each by its upper quartile (Wi-Fi bursts read low
    // and come in clusters), on TREND_HOLD_N readings in a row (a charger's rise stays, the noise's tail does not)
    int lo[TREND_ENDS], hi[TREND_ENDS];
    for (int i = 0; i < TREND_ENDS; i++) {
        lo[i] = s_m.hist[(s_m.hist_i + i) % TREND_N];
        hi[i] = s_m.hist[(s_m.hist_i - 1 - i + TREND_N) % TREND_N];
    }
    sort_n(lo, TREND_ENDS);
    sort_n(hi, TREND_ENDS);
    int old = lo[TREND_ENDS * 3 / 4 - 1], now_mv = hi[TREND_ENDS * 3 / 4 - 1];
    int top = -1;
    if (now_mv - old < TREND_UP_MV || now_mv >= s_vfull || dip_guard(now, now_mv) ||
        ((top = ceil_lvl(now, 0)) > 0 && now_mv - top < CEIL_MV)) {
        s_m.trend_hold = 0;
    } else if (++s_m.trend_hold >= TREND_HOLD_N) {
        s_m.trend_base = old;               // the charging offset's reference
        return now_mv - old;
    }
    return 0;
}

// ---- v4: the baseline and the slow trend (battery only)

// The last dip on battery still counts: DIP_OPEN_MS while the cell reads low, DIP_MS once it is back
static bool dip_recent(uint32_t now)
{
    return s_m.dip_t && (s_m.dip_open ? now - s_m.dip_t < DIP_OPEN_MS : now - s_m.dip_end < DIP_MS);
}

// ... and a rise to lvl is that load's end, not a charger
static bool dip_guard(uint32_t now, int lvl)
{
    return dip_recent(now) && lvl - s_m.dip_ref < BASE_MV;
}

static void base_clear(void)
{
    s_m.base_n = s_m.base_i = s_m.base_hit = 0;
}

static void slow_clear(void)
{
    s_m.slow_n = s_m.slow_i = s_m.sb_n = s_m.slow_hold = 0;
}

// The baseline for a level now at backlight bl: the lower quartile of the levels of the 2 min before the newest one
// at least BASE_MIN_MS old (at most BASE_MAX_MS: across a load, the levels before it), those taken with more
// backlight counted BASE_BL_MV higher; -1 = too few. *t_anchor: the newest level's time.
static int base_ref(uint32_t now, int bl, uint32_t *t_anchor)
{
    int v[BASE_N], n = 0;
    uint32_t t0 = 0;
    bool anchored = false;
    for (int k = 0; k < s_m.base_n; k++) {
        const base_t *e = &s_m.base[(s_m.base_i - 1 - k + BASE_N) % BASE_N];
        uint32_t age = now - e->t;
        if (age < BASE_MIN_MS) {
            continue;
        }
        if (age > BASE_MAX_MS) {
            break;
        }
        if (!anchored) {
            anchored = true;
            t0 = e->t;
        }
        if (t0 - e->t > BASE_WIN_MS && n >= BASE_MIN_N) {
            break;                          // (further back while there are too few: a short settled stretch)
        }
        v[n++] = e->mv + (e->bl > bl ? BASE_BL_MV : 0);
    }
    if (t_anchor) {
        *t_anchor = t0;
    }
    if (n < BASE_MIN_N) {
        return -1;
    }
    sort_n(v, n);
    return v[n / 4];
}

// A settled level lvl on battery (no step this reading): a rise against the baseline, twice in a row; the level is
// kept for the next ones. Returns the rise once confirmed (0 = none) and *ref, the baseline.
static int base_feed(uint32_t now, int lvl, int bl, int *ref)
{
    if (s_m.base_hit && now - s_m.base_hit_t > BASE_PIN_MS) {
        s_m.base_hit = 0;                   // the first hit too long ago: measured afresh
    }
    // v4 review: the next hit against the first hit's baseline (by then the newest levels may hold the first hit's
    // own, when the settled readings began with the rise)
    uint32_t t_b = s_m.base_hit ? s_m.base_b_t : 0;
    int b = s_m.base_hit ? s_m.base_b : base_ref(now, bl, &t_b), rise = 0;
    bool hit = b > 0 && lvl - b >= BASE_MV;
    if (hit && dip_guard(now, lvl) && (int32_t)(s_m.dip_t - t_b) <= 0) {
        hit = false;                        // the baseline is within a step down, and the level back near the one
    } else if (hit && !s_m.base_hit) {      // before it: that load ended
        int top = ceil_lvl(now, t_b);       // v4 review: nor back to the levels of the 9 min up to the baseline (a
        hit = top <= 0 || lvl - top >= CEIL_MV; // load too small to be seen as a step ended, the cell recovered)
    }
    if (!hit) {
        s_m.base_hit = 0;
    } else if (++s_m.base_hit >= BASE_HITS) {
        rise = lvl - b;
        *ref = b;
    } else {
        fast(now);                          // the confirmation 2 s later
        s_m.base_b = b;
        s_m.base_b_t = t_b;
        s_m.base_hit_t = now;
    }
    s_m.base[s_m.base_i] = (base_t){ now, clamp16(lvl), (uint8_t)bl };
    s_m.base_i = (s_m.base_i + 1) % BASE_N;
    if (s_m.base_n < BASE_N) {
        s_m.base_n++;
    }
    return rise;
}

// A reading on battery into the slow trend: calm ones are collected per minute, anything else starts it over.
// Returns the rise once the calm minutes show a charger (0 = none).
static int slow_feed(uint32_t now, int raw, bool calm)
{
    if (!calm || raw <= 0) {
        slow_clear();
        return 0;
    }
    int rise = 0;
    if (s_m.sb_n && now - s_m.sb_t >= 60000) {
        if (s_m.sb_n < SLOW_MIN_READS) {
            slow_clear();                   // a minute of few calm readings: not one in a row
        } else {
            sort_n(s_m.sb, s_m.sb_n);
            s_m.slow_t[s_m.slow_i] = s_m.sb_t;
            s_m.slow_mv[s_m.slow_i] = s_m.sb[s_m.sb_n / 2];
            s_m.slow_i = (s_m.slow_i + 1) % SLOW_N;
            if (s_m.slow_n < SLOW_N) {
                s_m.slow_n++;
            }
            s_m.sb_n = 0;
            // windows of k minutes ending with this one, oldest first: m[0] .. m[k-1]
            int m[SLOW_N], hi = -1;
            for (int i = 0; i < s_m.slow_n; i++) {
                m[i] = s_m.slow_mv[(s_m.slow_i - s_m.slow_n + i + SLOW_N) % SLOW_N];
            }
            int last = m[s_m.slow_n - 1], found = 0;
            for (int k = 1; k <= s_m.slow_n && !found; k++) {
                int first = m[s_m.slow_n - k], mid = m[s_m.slow_n - k + (k - 1) / 2];
                hi = first > hi ? first : hi;
                uint32_t t_first = s_m.slow_t[(s_m.slow_i - k + SLOW_N) % SLOW_N];
                if (k >= SLOW_MIN_MIN && last >= hi && last - first >= SLOW_UP_MV && last - mid >= SLOW_HALF_MV &&
                    4 * (last - mid) >= 3 * (mid - first) && !dip_guard(now, last) &&
                    (!s_m.heavy_t || (int32_t)(t_first - s_m.heavy_t) >= SLOW_HEAVY_MS)) {
                    found = last - first;
                }
            }
            s_m.slow_hold = found ? s_m.slow_hold + 1 : 0;
            rise = s_m.slow_hold >= SLOW_HOLD_N ? found : 0;   // v4 review: at two minute ends in a row (a decaying
        }                                                       // rise or noise's tail fails the next one)
    }
    if (!s_m.sb_n) {
        s_m.sb_t = now ? now : 1;
    }
    if (s_m.sb_n < SLOW_BUCKET_N) {
        s_m.sb[s_m.sb_n++] = raw;
    }
    return rise;
}

static void win_push(uint32_t now, int mv, bool settled)
{
    s_m.win[s_m.win_i] = (win_t){ now, clamp16(mv), settled };
    s_m.win_i = (s_m.win_i + 1) % WIN_N;
    if (s_m.win_n < WIN_N) {
        s_m.win_n++;
    }
}

// The window: *hi = the highest of the last minute's settled readings (at least RECENT_MIN of them over at least
// RECENT_SPAN_MS, so a fall must last and a cell still recovering from a load does not count; -1 = not enough), *ref =
// the newest reading of any kind at least 3 min old (older when readings were skipped meanwhile: a fall over a longer
// time is still a fall; a reference taken under load only makes a fall smaller; -1 = none), taken at *ref_t
static void win_scan(uint32_t now, int *hi, int *ref, uint32_t *ref_t)
{
    int n_recent = 0;
    uint32_t newest = 0, oldest = 0;
    *hi = *ref = -1;
    *ref_t = 0;
    for (int k = 0; k < s_m.win_n; k++) {
        const win_t *w = &s_m.win[(s_m.win_i - 1 - k + WIN_N) % WIN_N];     // newest first
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
            *ref_t = w->t;
            break;
        }
    }
    if (n_recent < RECENT_MIN || newest - oldest < RECENT_SPAN_MS) {
        *hi = -1;
    }
}

// The FULL plateau's reference: the highest settled reading of the minute that ended age ago (at least RECENT_MIN
// of them over RECENT_SPAN_MS, like "now", so both ends carry the same noise), when every reading since was settled
// (calm all the time: a load in between would hide a rise or make a fall), -1 = none
static int win_ref_calm(uint32_t now, uint32_t age)
{
    int n = 0, hi = -1;
    uint32_t newest = 0, oldest = 0;
    for (int k = 0; k < s_m.win_n; k++) {
        const win_t *w = &s_m.win[(s_m.win_i - 1 - k + WIN_N) % WIN_N];
        uint32_t a = now - w->t;
        if (a > age + RECENT_MS) {
            break;
        }
        if (!w->settled || (s_m.busy_t && w->t <= s_m.busy_t)) {
            return -1;
        }
        if (a >= age) {
            if (!n++) {
                newest = w->t;
            }
            oldest = w->t;
            hi = w->mv > hi ? w->mv : hi;
        }
    }
    return n >= RECENT_MIN && newest - oldest >= RECENT_SPAN_MS ? hi : -1;
}

// The settled levels of CEIL_MIN_MS-CEIL_MAX_MS ago (or of the same span up to until, at most CEIL_MAX_MS ago): their
// 90th percentile (-1 = fewer than 12; at most 64 of them are looked at, spread over all)
static int ceil_lvl(uint32_t now, uint32_t until)
{
    int n = 0, v[64], m = 0;
    uint32_t newest = until ? now - until : CEIL_MIN_MS, oldest = newest + (CEIL_MAX_MS - CEIL_MIN_MS);
    oldest = oldest > CEIL_MAX_MS ? CEIL_MAX_MS : oldest;
    for (int pass = 0; pass < 2; pass++) {
        int step = n / 64 + 1, j = 0;
        for (int k = 0; k < s_m.win_n; k++) {
            const win_t *w = &s_m.win[(s_m.win_i - 1 - k + WIN_N) % WIN_N];
            uint32_t age = now - w->t;
            if (age > oldest) {
                break;
            }
            if (age < newest || !w->settled) {
                continue;
            }
            if (!pass) {
                n++;
            } else if (j++ % step == 0 && m < 64) {
                v[m++] = w->mv;
            }
        }
    }
    if (m < 12) {
        return -1;
    }
    sort_n(v, m);
    return v[m * 9 / 10];
}

static void win_clear(void)
{
    s_m.win_n = s_m.win_i = 0;
    s_m.level_peak = -1;
}

// Plugged in or out, or a step: the median, the wall-charger trend and the window start over with readings of the
// new regime (the step detector's run goes on: it is what measures the step)
static void regime_reset(void)
{
    s_m.raw_n = s_m.raw_i = 0;
    s_m.hist_n = s_m.hist_i = s_m.trend_hold = 0;
    win_clear();
}

// ---- charging offset

// External power began; ref = the settled level just before (-1 none: the last relaxed one on battery is used)
static void offset_plug(uint32_t now, int ref, bool usb)
{
    s_m.plug_t = now;
    if (s_m.ofs_final && s_m.ofs_usb == usb && s_m.unplug_t && now - s_m.unplug_t < RELAX_MS) {
        return;     // back on the same kind of source before the cell relaxed: the last offset still holds
    }
    s_m.ofs = CHG_OFS_MV;
    s_m.ofs_final = false;
    s_m.ofs_usb = usb;
    s_m.ofs_ref = ref > 0 ? ref : s_m.bat_ref > 0 && now - s_m.bat_ref_t <= OFS_REF_MAX_MS ? s_m.bat_ref : -1;
}

// After the power state is decided for a reading
static void offset_track(uint32_t now, board_power_t pw, int mv, bool fresh, bool settled)
{
    if (s_m.ext == BOARD_EXT_NONE) {
        // a reference once the last charge's polarisation has relaxed (right after unplugging the cell still reads
        // high: a step measured from there would come out too small)
        if (fresh && settled && !s_m.nobat && (!s_m.unplug_t || now - s_m.unplug_t >= RELAX_MS)) {
            s_m.bat_ref = mv;
            s_m.bat_ref_t = now;
        }
        return;
    }
    if (pw == BOARD_PWR_CHARGING && fresh && s_m.ofs_ref > 0 && !s_m.ofs_final) {
        if (now - s_m.plug_t > OFS_WINDOW_MS) {
            s_m.ofs_final = true;   // no settled reading in time (the screen or turns kept loading the cell): later
            return;                 // readings would count the charge since as offset
        }
        int step = mv - s_m.ofs_ref;
        s_m.ofs = step < 0 ? 0 : step > CHG_OFS_MAX_MV ? CHG_OFS_MAX_MV : step;   // the ohmic step at once, the
                                                                                  // polarisation within a minute
        s_m.ofs_final = settled && now - s_m.plug_t >= OFS_SETTLE_MS;
    }
}

// ---- power state

// The lowest level a non-charging cell on the charger is taken as full at (the step on plugging in, or a stop)
static int full_floor(void)
{
    int f = s_vfull - NOSTEP_FULL_MV;
    return f > FULL_MIN_MV ? f : FULL_MIN_MV;
}

typedef struct {
    uint32_t now, ref3_t;
    board_power_t was;
    bool usb, set_now, calm;
    int mv, hi, ref3, ref4, ref15, step;
} dec_in_t;

// The power state after a reading (the external power is decided before; mv = the filtered level; hi / ref3 = the
// window, win_scan(); ref4 / ref15 = the plateau's references, win_ref_calm(); step = a step confirmed by this
// reading). *ev / *evd: why.
static board_power_t decide(const dec_in_t *in, board_bat_ev_t *ev, int *evd)
{
    int force = s_m.force;
    s_m.force = -1;
    if (s_m.nobat || in->mv < 0 || s_m.ext == BOARD_EXT_NONE) {
        return BOARD_PWR_BATTERY;
    }
    if (force >= 0) {
        return (board_power_t)force;        // the plug-in's step was measured (logged there)
    }
    if (s_m.pend_t || s_m.lost_t) {
        return in->was;                     // being measured: as shown
    }
    if (in->step < 0) {
        // a step down with the host still there (without it the step ended the external power): the charger stopped
        return in->mv >= full_floor() ? BOARD_PWR_FULL : BOARD_PWR_USB_DRAIN;
    }
    if (in->step > 0 && in->was != BOARD_PWR_CHARGING) {
        return BOARD_PWR_CHARGING;          // charging again (logged as STEP_UP)
    }
    bool d_ok = in->hi >= 0 && in->ref3 >= 0;
    int d = in->hi - in->ref3;
    bool d_heavy = d_ok && s_m.heavy_t && s_m.heavy_t >= in->ref3_t;  // a long session in the 3 min
    switch (in->was) {
    case BOARD_PWR_BATTERY:
        return BOARD_PWR_CHARGING;          // external power without a measured step (the trend, a host without a
                                            // reference): charging until shown not
    case BOARD_PWR_USB_DRAIN:
        // ends on a settled rise from the lowest point (the load stopped and the port catches up), or when the USB
        // host went (still plugged in: a drain is not told from unplugging any more)
        if (!in->usb) {
            *ev = BOARD_BEV_DRAIN_END;
            return BOARD_PWR_CHARGING;
        }
        if (in->now - s_m.power_t >= DRAIN_MIN_MS && in->set_now && in->mv >= s_m.trough + DRAIN_END_MV) {
            *ev = BOARD_BEV_DRAIN_END;
            *evd = in->mv - s_m.trough;
            return BOARD_PWR_CHARGING;
        }
        return BOARD_PWR_USB_DRAIN;
    case BOARD_PWR_FULL:
        // a fall alone is no USB_DRAIN: after its termination the charger leaves the load on the cell, which relaxes
        // and drains towards the recharge level; a fall across a long session (HEAVY_MS) is (the port gave less than
        // the load took), and so is a level the charger would have topped up by now
        if (in->usb && d_heavy && d <= -DRAIN_MV) {
            *ev = BOARD_BEV_DRAIN;
            *evd = d;
            return BOARD_PWR_USB_DRAIN;
        }
        if (in->now - s_m.power_t >= FULL_DWELL_MS && in->calm && in->hi >= 0 && s_m.full_lo > 0 &&
            in->hi - s_m.full_lo >= FULL_RISE_MV && in->ref4 > 0 && in->hi - in->ref4 >= FULL_RISE_MV / 2) {
            *ev = BOARD_BEV_RISE;           // the charger started again (its recharge level, or it never stopped)
            *evd = in->hi - s_m.full_lo;
            return BOARD_PWR_CHARGING;
        }
        if (in->usb && in->hi >= 0 && in->hi < s_vfull - FULL_HOLD_MV) {
            *ev = BOARD_BEV_FULL_EXIT;      // the cell drains on USB and the charger does not take it back
            *evd = in->hi - s_m.full_hi;
            return BOARD_PWR_USB_DRAIN;
        }
        return BOARD_PWR_FULL;
    default:
        break;
    }
    bool d_fall = d_ok && d <= -DRAIN_MV, p_fall = in->hi >= 0 && s_m.level_peak - in->hi >= DRAIN_PEAK_MV;
    if (in->usb && (d_fall || p_fall)) {
        // without a host a fall ends the external power instead (unplugged or overloaded: unknown). With a long
        // session in the time measured the load took more than the port gives; else, near full, the charger stopped
        // (terminated: the cell relaxes and the load moves onto it)
        *evd = d_fall ? d : in->hi - s_m.level_peak;
        bool heavy = d_fall ? d_heavy : s_m.heavy_t && s_m.heavy_t >= s_m.power_t;
        if (!heavy && in->hi >= full_floor()) {
            *ev = BOARD_BEV_CHG_STOP;
            return BOARD_PWR_FULL;
        }
        *ev = BOARD_BEV_DRAIN;
        return BOARD_PWR_USB_DRAIN;
    }
    // the charger holds its CV level, or terminated: full for this charger
    int band = s_vfull - PLATEAU_BAND_MV > FULL_MIN_MV ? s_vfull - PLATEAU_BAND_MV : FULL_MIN_MV;
    bool p4 = in->hi >= band && in->ref4 > 0 && in->hi - in->ref4 <= PLATEAU_RISE_MV &&
              in->ref4 - in->hi <= PLATEAU_FALL_MV;
    bool p15 = in->hi >= FULL_MIN_MV && in->ref15 > 0 && in->hi - in->ref15 <= PLAT_LONG_RISE_MV &&
               in->ref15 - in->hi <= PLATEAU_FALL_MV;
    if (!p4 && !p15) {
        s_m.plat_t = 0;
    } else if (!s_m.plat_t) {
        s_m.plat_t = in->now ? in->now : 1;
    } else if (in->now - s_m.plat_t >= PLATEAU_HOLD_MS) {
        *ev = BOARD_BEV_PLATEAU;
        *evd = p4 ? in->hi - in->ref4 : in->hi - in->ref15;
        s_m.plat_p4 = p4;
        return BOARD_PWR_FULL;
    }
    return BOARD_PWR_CHARGING;
}

// v = the highest settled level of a FULL (this divider's reading of a full cell on the charger): the curve's 100 %
// from now on. Lower only when down (that FULL began with the plateau near v_full), by VFULL_DOWN_MV at most.
static void vfull_learn(int v, bool down)
{
    if (v <= 0 || (v < s_vfull && !down)) {
        return;
    }
    v = v < s_vfull - VFULL_DOWN_MV ? s_vfull - VFULL_DOWN_MV : v;
    v = v < VFULL_MIN_MV ? VFULL_MIN_MV : v > VFULL_MAX_MV ? VFULL_MAX_MV : v;
    if (abs(v - s_vfull) < VFULL_STEP_MV) {
        return;
    }
    s_vfull = v;
    s_m.vfull_learn = v;
    ev_add(BOARD_BEV_VFULL, v);
}

// Plugged in with a USB host (no external power before): the step is measured against ref, the settled level before
// (-1 none: CHARGING at once, as without a measurement)
static void usb_plugged(uint32_t now, int ref)
{
    ev_add(BOARD_BEV_USB, ref);
    regime_reset();
    offset_plug(now, ref, true);
    if (ref > 0) {
        s_m.pend_t = now ? now : 1;
        s_m.pend_ref = ref;
        s_m.pend_n = 0;
        fast(now);
    } else {
        ev_add(BOARD_BEV_NO_REF, 0);
    }
}

// The plug-in's step from the readings so far (step > 0: a step the detector confirmed)
static void pend_track(uint32_t now, int raw, bool set_now, bool settled, bool usb, int step)
{
    int res = -1;
    if (s_m.ext != BOARD_EXT_USB) {
        s_m.pend_t = 0;                     // unplugged again before it was measured
        return;
    }
    if (step > 0) {
        res = BOARD_PWR_CHARGING;           // logged as STEP_UP
    } else {
        if (set_now && s_m.pend_n < PEND_N) {
            s_m.pend_v[s_m.pend_n++] = raw;
        }
        if (s_m.pend_n >= PEND_N || !settled || !usb || now - s_m.pend_t >= PEND_MS) {
            if (!s_m.pend_n) {
                ev_add(BOARD_BEV_NO_REF, 0);    // a load came first: unmeasured
                res = BOARD_PWR_CHARGING;
            } else {
                int lvl = med_of(s_m.pend_v, s_m.pend_n, NULL), d = lvl - s_m.pend_ref;
                if (d >= NOSTEP_MV) {
                    ev_add(BOARD_BEV_STEP, d);
                    res = BOARD_PWR_CHARGING;
                } else {
                    ev_add(BOARD_BEV_NO_STEP, d);
                    res = lvl >= full_floor() ? BOARD_PWR_FULL : BOARD_PWR_CHARGING;
                }
            }
        }
    }
    if (res >= 0) {
        s_m.force = res;
        s_m.pend_t = 0;
    }
}

// The highest raw reading of the last CAP_ENV_WIN_MS, -1 = none
static int env_hi(uint32_t now)
{
    int hi = -1;
    for (int k = 0; k < s_m.env_n; k++) {
        int i = (s_m.env_i - 1 - k + ENV_N) % ENV_N;
        if (now - s_m.env_t[i] > CAP_ENV_WIN_MS) {
            break;
        }
        hi = s_m.env_mv[i] > hi ? s_m.env_mv[i] : hi;
    }
    return hi;
}

// The percent on BATTERY (CAP_BUSY_MS above): mv = the median, set_now = a settled reading
static int bat_pct(uint32_t now, int mv, bool set_now, bool calm)
{
    int curve = pct_of(mv), set = -1;
    if (set_now && s_m.s3_n == 3) {
        set = calm ? curve : pct_of(med_of(s_m.s3, 3, NULL));   // calm: the median holds settled readings only
    }
    if (!s_m.synced || s_m.cap < 0) {
        if (set >= 0) {
            s_m.synced = true;
            s_m.cap = s_m.cap_to = set;
            return set;
        }
        return curve;                       // booting: both ways
    }
    if (set >= 0 && (calm || now - s_m.calm_t >= CAP_BUSY_MS)) {
        s_m.cap_to = set;
    } else if (now - s_m.calm_t >= CAP_ENV_MS) {
        int env = pct_of(env_hi(now));      // constant use: no settled level, or none trusted yet
        if (env >= 0 && env < s_m.cap) {
            s_m.cap_to = env;
        }
    }
    // down 1 point per PCT_GLIDE_MS (v4 review: always, not only after FULL or the charger, whose glide the first
    // v4 cancelled on a reading that was not calm; readings far apart in use then jumped 2 points and more)
    if (s_m.cap_to >= 0 && s_m.cap_to < s_m.cap && now - s_m.glide_t >= PCT_GLIDE_MS) {
        s_m.cap--;
        s_m.glide_t = now;
    }
    return s_m.cap <= LOW_PCT && curve >= 0 && curve < s_m.cap ? curve : s_m.cap;
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

static void model_reset(void)
{
    memset(&s_m, 0, sizeof(s_m));
    s_m.trend_base = s_m.level_peak = s_m.bat_ref = s_m.ofs_ref = s_m.ext_peak = s_m.set_mv = s_m.lost_level = -1;
    s_m.vfull_learn = s_m.min_mv = s_m.force = s_m.cap = s_m.cap_to = -1;
    s_m.ofs = CHG_OFS_MV;
    board_bat_t m = MODEL_INIT;
    m.vfull = s_vfull;
    taskENTER_CRITICAL(&s_mux);
    s_model = m;
    taskEXIT_CRITICAL(&s_mux);
}

bool board_battery_sample(board_bat_q_t q)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!s_m.started) {
        model_reset();
        s_m.started = true;
        s_m.log_next = now + LOG_MIN_MS;
        ev_add(BOARD_BEV_BOOT, 0);
    }
    bool usb = usb_serial_jtag_is_connected();
    bool usb_on = usb && !s_m.usb_last, usb_off = !usb && s_m.usb_last;
    s_m.usb_last = usb;
    if (q != BOARD_BAT_SETTLED) {
        s_m.busy_t = now ? now : 1;         // the calm clock (wall-charger trend, FULL plateau)
    }
    if (q != BOARD_BAT_SKIP) {
        s_m.skip_since = 0;
    } else if (!s_m.skip_since) {
        s_m.skip_since = now ? now : 1;
    } else if (now - s_m.skip_since >= HEAVY_MS) {
        s_m.heavy_t = now ? now : 1;        // a long session (a fall across it is a drain)
    }
    // s_model is written only here (this task): no lock needed to read it
    const board_bat_t *p = &s_model;
    board_bat_t m = *p;                     // a skipped reading keeps the level
    m.usb = usb;
    bool fresh = false, settled = q == BOARD_BAT_SETTLED;
    bool calm = settled && (!s_m.busy_t || now - s_m.busy_t >= TREND_CALM_MS);
    int raw = -1;
    if (q == BOARD_BAT_SKIP && p->mv >= 0) {
        // the speaker or a turn's Wi-Fi draws more than the port gives: the cell sags for the moment, and the
        // reading would show a drop the settled cell does not have (the first reading is taken whatever the load,
        // so the indicator appears at once)
        s_m.skipped++;
    } else {
        raw = board_battery_mv();
        m.raw = raw;
        fresh = raw > 0;
    }
    bool set_now = fresh && settled;
    int bl = board_backlight_get();
    if (calm) {
        s_m.calm_t = now ? now : 1;
    }
    if (!settled) {
        s_m.s3_n = s_m.s3_i = 0;            // the last 3 settled readings in a row
    } else if (set_now && !s_m.nobat) {
        s_m.s3[s_m.s3_i] = raw;
        s_m.s3_i = (s_m.s3_i + 1) % 3;
        s_m.s3_n += s_m.s3_n < 3;
    }
    board_ext_t ext0 = s_m.ext;
    int before = run_level(now);            // the settled level before this reading (-1 = none)

    // The USB host: there = external power (VBUS comes with it); gone = unplugged or only suspended (a step decides)
    if (usb_on) {
        s_m.lost_t = 0;
        if (s_m.ext == BOARD_EXT_NONE) {
            usb_plugged(now, before);
        } else if (!s_m.usb_ev_t || now - s_m.usb_ev_t >= USB_EV_GAP_MS) {
            s_m.usb_ev_t = now ? now : 1;
            ev_add(BOARD_BEV_USB, before);  // back (it was suspended, or a charger came first)
        }
        s_m.ext = BOARD_EXT_USB;
    } else if (usb_off && s_m.ext == BOARD_EXT_USB) {
        s_m.lost_t = now ? now : 1;
        s_m.lost_n = 0;
        s_m.lost_ok = before > 0;
        s_m.lost_level = s_m.set_mv;
        fast(now);
    }

    // Steps: settled readings only, a reading under load restarts the run
    int step = 0, step_ref = -1;
    if (!settled) {
        run_break();
    } else if (fresh && !s_m.nobat) {
        step = step_feed(now, raw, &step_ref);
    }
    // a step up on battery back where it was before a dip (that load ended: the trends still guarded DIP_MS against
    // the rise it made), or (v4 review) not above the settled levels of 3-12 min ago (a load too small to be seen as
    // a step ended, or noise; the baseline is kept: a charger's rise that goes on still shows against it)
    bool back = step > 0 && s_m.ext == BOARD_EXT_NONE && dip_recent(now) &&
                abs(step_ref + step - s_m.dip_ref) <= STEP_SMALL_MV;
    int top = step > 0 && s_m.ext == BOARD_EXT_NONE && !back ? ceil_lvl(now, 0) : -1;
    bool under = top > 0 && step_ref + step - top < CEIL_MV;
    if (back || under) {
        ev_add(BOARD_BEV_LOAD_END, step);
        if (back) {
            s_m.dip_open = false;
            s_m.dip_end = now ? now : 1;
        }
    } else if (step > 0) {
        ev_add(BOARD_BEV_STEP_UP, step);
        if (s_m.ext == BOARD_EXT_NONE) {
            s_m.ext = BOARD_EXT_CHARGER;    // a charger without a host
            int b = base_ref(now, bl, NULL);    // v4: a soft start's earlier part went into the run as small steps
            offset_plug(now, b > 0 && b < step_ref ? b : step_ref, false);
        }
    } else if (step < 0) {
        if (s_m.ext == BOARD_EXT_NONE) {
            ev_add(BOARD_BEV_STEP_DOWN, step);      // on battery: a load the app does not grade
            s_m.dip_t = now ? now : 1;
            s_m.dip_ref = step_ref;
            s_m.dip_open = true;
        } else if (usb) {
            ev_add(BOARD_BEV_CHG_STOP, step);
        } else {
            ev_add(BOARD_BEV_STEP_DOWN, step);
            s_m.ext = BOARD_EXT_NONE;       // unplugged
        }
    }
    if (s_m.dip_open && set_now && (s_m.ext != BOARD_EXT_NONE || raw >= s_m.dip_ref - DIP_BACK_MV)) {
        s_m.dip_open = false;               // the dip's load ended (the guard holds DIP_MS more)
        s_m.dip_end = now ? now : 1;
    }
    // v4: on battery, the settled level against the levels before it (a soft start, a plug-in under a load)
    int base_rise = 0;
    if (s_m.ext == BOARD_EXT_NONE && !s_m.nobat) {
        if (step) {
            if (step < 0) {
                base_clear();               // a step down: a load the app does not grade (v4 review: kept at its
            }                               // end, LOAD_END: a charger's rise that goes on shows against it, the
            slow_clear();                   // load's end does not, dip_guard() and the levels before it)
        } else if (set_now && s_m.s3_n == 3) {
            int ref = -1;
            base_rise = base_feed(now, med_of(s_m.s3, 3, NULL), bl, &ref);
            if (base_rise) {
                ev_add(BOARD_BEV_STEP_BASE, base_rise);
                s_m.ext = BOARD_EXT_CHARGER;
                offset_plug(now, ref, false);
                s_m.run_n = s_m.run_i = s_m.cand_n = 0;
                for (int i = 0; i < 3; i++) {
                    run_push(s_m.s3[(s_m.s3_i + i) % 3]);   // the run holds the new level
                }
            }
        }
    }
    if (step || base_rise) {
        if (under) {
            s_m.raw_n = s_m.raw_i = 0;      // ... the window kept (its levels are what the next step is held to)
        } else {
            regime_reset();                 // the level starts over from the confirming readings
        }
        for (int i = 0; i < s_m.run_n; i++) {
            raw_push(s_m.run[(s_m.run_i - s_m.run_n + i + RUN_N) % RUN_N]);
        }
    }

    if (fresh) {
        if (!step && !base_rise) {
            raw_push(raw);
        }
        s_m.env_t[s_m.env_i] = now;
        s_m.env_mv[s_m.env_i] = raw;
        s_m.env_i = (s_m.env_i + 1) % ENV_N;
        s_m.env_n += s_m.env_n < ENV_N;
        int spread;
        int mv = med_of(s_m.raw, s_m.raw_n, &spread);
        if (settled && s_m.raw_n == MEDIAN_N) {
            // a resting cell's readings agree within a few tens of mV; with no cell the charger's output swings on
            // its own (no indicator rather than a false "charging")
            bool nb = spread > NOBAT_SPREAD_MV || (s_m.nobat && spread >= NOBAT_CLEAR_MV);
            if (nb != s_m.nobat) {
                ev_add(nb ? BOARD_BEV_NOBAT : BOARD_BEV_CELL, spread);
                base_clear();
                slow_clear();
                s_m.synced = false;         // a cell again: the percent from its first settled level
            }
            s_m.nobat = nb;
        }
        win_push(now, mv, settled);
        m.mv = mv;
        if (settled) {
            s_m.min_mv = s_m.set_mv = mv;
        }
        if (s_m.ext == BOARD_EXT_NONE && !s_m.nobat) {
            int rise = trend_up(now, raw, calm), slow = slow_feed(now, raw, calm);
            if (rise) {
                ev_add(BOARD_BEV_TREND_UP, rise);
                s_m.ext = BOARD_EXT_CHARGER;
                offset_plug(now, s_m.trend_base, false);
            } else if (slow) {
                ev_add(BOARD_BEV_TREND_SLOW, slow);
                s_m.ext = BOARD_EXT_CHARGER;
                offset_plug(now, -1, false);
                s_m.ofs_ref = -1;           // the step was long before: no reference (CHG_OFS_MV)
            }
            if (rise || slow) {
                int hi, ref;                // to be confirmed by the charge's rise (TREND_EXIT_MS), measured the
                uint32_t ref_t;             // same way: the highest settled level of the minute
                win_scan(now, &hi, &ref, &ref_t);
                s_m.trend_t = now ? now : 1;
                s_m.trend_lvl = hi >= 0 ? hi : mv;
            }
        }
    }

    // The host's packets stopped: unplugged when a step down followed (above), else still plugged in
    if (s_m.lost_t) {
        if (s_m.ext != BOARD_EXT_USB || usb) {
            s_m.lost_t = 0;
        } else {
            s_m.lost_n += set_now;
            s_m.lost_ok &= settled;
            if (now - s_m.lost_t >= LOST_WATCH_MS && !s_m.cand_n) {
                bool seen = s_m.lost_ok && s_m.lost_n >= LOST_SETTLED_N;
                ev_add(seen ? BOARD_BEV_USB_LOST_NO_STEP : BOARD_BEV_USB_LOST, s_m.lost_n);
                s_m.ext = BOARD_EXT_KEPT;
                s_m.lost_t = 0;
            }
        }
    }
    if (s_m.pend_t) {
        pend_track(now, raw, set_now, settled, usb, step);
    }

    // External power without a host ends on a fall (an unplug whose step was missed under load or too small),
    // measured from the level it began at (kept: the last settled level with the host). v4: the small fall within
    // SLOW_HEAVY_MS of a long session only on a calm reading (the cell reads low for minutes after it, on the charger
    // too), and less the backlight's sag when it is higher than at the peak (the screen woken reads lower, no unplug);
    // v4 review: the first v4 took it on calm readings only and less a flat BASE_BL_MV, also from the fall at any
    // reading: an unplug with the screen woken and turns every minute or two showed the bolt for up to an hour
    if (s_m.ext != ext0) {
        s_m.ext_peak = set_now ? m.mv : -1;
        s_m.ext_peak_bl = bl;
        if (s_m.ext == BOARD_EXT_KEPT && s_m.lost_level > s_m.ext_peak) {
            s_m.ext_peak = s_m.lost_level;
        }
    }
    if (fresh && (s_m.ext == BOARD_EXT_CHARGER || s_m.ext == BOARD_EXT_KEPT)) {
        if (settled && m.mv > s_m.ext_peak) {
            s_m.ext_peak = m.mv;
            s_m.ext_peak_bl = bl;
        }
        int lim = p->power == BOARD_PWR_FULL ? FULL_LOST_MV : EXT_FALL_MV, drop = s_m.ext_peak - m.mv;
        int bl_sag = bl > s_m.ext_peak_bl ? (bl - s_m.ext_peak_bl) * BL_MV_X10 / 10 : 0;
        bool rested = calm || (settled && (!s_m.heavy_t || now - s_m.heavy_t >= SLOW_HEAVY_MS));
        bl_sag = bl_sag > BASE_BL_MV ? BASE_BL_MV : bl_sag;
        if (s_m.ext_peak > 0 && ((rested && drop - bl_sag >= lim) || drop - bl_sag >= EXT_FALL_BUSY_MV)) {
            ev_add(BOARD_BEV_FALL, -drop);
            s_m.ext = BOARD_EXT_NONE;
        }
    }
    // v4 review: a charger found by a trend is confirmed by the charge's rise, else it was none
    if (!s_m.trend_t || ext0 == BOARD_EXT_NONE) {
        // none, or found by this reading
    } else if (s_m.ext != BOARD_EXT_CHARGER || p->power != BOARD_PWR_CHARGING ||
               (s_m.heavy_t && (int32_t)(s_m.heavy_t - s_m.trend_t) >= 0)) {
        s_m.trend_t = 0;                    // unplugged, FULL, or a long session meanwhile: no telling
    } else if (calm && now - s_m.trend_t >= TREND_EXIT_MS) {
        int hi, ref;
        uint32_t ref_t;
        win_scan(now, &hi, &ref, &ref_t);
        if (hi >= 0) {
            if (hi - s_m.trend_lvl < TREND_EXIT_MV && hi < full_floor()) {
                ev_add(BOARD_BEV_NO_RISE, hi - s_m.trend_lvl);
                s_m.ext = BOARD_EXT_NONE;
            }
            s_m.trend_t = 0;
        }
    }
    if ((s_m.ext != BOARD_EXT_NONE) != (ext0 != BOARD_EXT_NONE)) {
        s_m.hist_n = s_m.hist_i = s_m.trend_hold = 0;   // the trends and the baseline start over (of the other regime)
        base_clear();
        slow_clear();
        if (s_m.ext == BOARD_EXT_NONE) {
            s_m.trend_t = 0;
            s_m.unplug_t = now ? now : 1;
            s_m.pend_t = s_m.lost_t = 0;
        }
    }

    int hi, ref3;
    uint32_t ref3_t;
    win_scan(now, &hi, &ref3, &ref3_t);
    if (hi > s_m.level_peak) {
        s_m.level_peak = hi;
    }
    int ref4 = win_ref_calm(now, PLATEAU_MS), ref15 = win_ref_calm(now, PLAT_LONG_MS);
    m.d3_ok = hi >= 0 && ref3 >= 0;
    m.d3 = m.d3_ok ? hi - ref3 : 0;
    m.d4_ok = hi >= 0 && ref4 >= 0;
    m.d4 = m.d4_ok ? hi - ref4 : 0;
    if (p->power == BOARD_PWR_FULL && hi >= 0) {
        s_m.full_hi = hi > s_m.full_hi ? hi : s_m.full_hi;
        if (calm && hi < s_m.full_lo) {
            s_m.full_lo = hi;
        }
    }
    board_bat_ev_t ev = BOARD_BEV__N;
    int evd = 0;
    dec_in_t in = { .now = now, .ref3_t = ref3_t, .was = p->power, .usb = usb, .set_now = set_now, .calm = calm,
                    .mv = m.mv, .hi = hi, .ref3 = ref3, .ref4 = ref4, .ref15 = ref15, .step = step };
    m.power = decide(&in, &ev, &evd);
    if (ev != BOARD_BEV__N && m.power != p->power) {
        ev_add(ev, evd);
    }
    if (m.power != p->power) {
        if (p->power == BOARD_PWR_FULL) {
            vfull_learn(s_m.full_hi, false);    // a FULL that read higher than v_full: it is too low
        }
        // a new state starts its own window: a fall that began before it does not count again
        win_clear();
        m.d3_ok = m.d4_ok = false;
        s_m.trough = m.mv;
        s_m.power_t = now;
        s_m.plat_t = 0;
        s_m.glide = m.power == BOARD_PWR_BATTERY || m.power == BOARD_PWR_USB_DRAIN;
        s_m.glide_t = now - PCT_GLIDE_MS;
        if (m.power == BOARD_PWR_BATTERY && p->pct >= 0) {
            s_m.cap = p->pct;               // from here down only, gliding
            s_m.cap_to = -1;
            s_m.synced = true;
        }
        s_m.drain_idle = m.power == BOARD_PWR_USB_DRAIN && (p->power == BOARD_PWR_FULL || step < 0);
        if (m.power == BOARD_PWR_FULL) {
            s_m.full_lo = s_m.full_hi = hi >= 0 ? hi : m.mv;
            s_m.full_plat = ev == BOARD_BEV_PLATEAU && s_m.plat_p4;
            s_m.full_learned = false;
        }
    } else if (m.power == BOARD_PWR_USB_DRAIN && fresh && m.mv < s_m.trough) {
        s_m.trough = m.mv;
    }
    if (m.power == BOARD_PWR_FULL && !s_m.full_learned && now - s_m.power_t >= VFULL_AFTER_MS) {
        s_m.full_learned = true;            // held long enough to be the end of a charge, not its pause
        vfull_learn(s_m.full_hi, s_m.full_plat);
    }
    offset_track(now, m.power, m.mv, fresh, settled);
    m.ofs = s_m.ofs;
    if (s_m.nobat || m.mv < 0) {
        m.pct = -1;
    } else if (m.power == BOARD_PWR_FULL) {
        m.pct = 100;
    } else if ((s_m.pend_t || s_m.lost_t) && p->pct >= 0) {
        // being measured: as shown (the plug-in's readings already carry the charging offset)
    } else if (m.power == BOARD_PWR_BATTERY) {
        if (fresh || p->pct < 0) {
            m.pct = bat_pct(now, m.mv, set_now, calm);  // never up (a charger not seen yet, a recovering cell)
        }
    } else if (fresh || p->pct < 0) {
        bool fed = m.power == BOARD_PWR_CHARGING || (m.power == BOARD_PWR_USB_DRAIN && !s_m.drain_idle);
        int pct = pct_of(fed ? m.mv - s_m.ofs : m.mv);
        int was = p->pct;
        if (pct >= 0 && was >= 0) {
            if (m.power == BOARD_PWR_CHARGING) {
                pct = pct < was ? was : pct;        // never goes down while charging (a fall shows as USB_DRAIN)
            } else {
                pct = pct > was ? was : pct;        // nor up while the port cannot keep up
            }
            if (s_m.glide && m.power != BOARD_PWR_CHARGING) {
                // just left FULL (100 at any level) or the charger (an estimated offset): down to the curve
                if (pct >= was - 1) {
                    s_m.glide = false;
                } else if (now - s_m.glide_t >= PCT_GLIDE_MS) {
                    pct = was - 1;
                    s_m.glide_t = now;
                } else {
                    pct = was;
                }
            }
        }
        m.pct = pct;
    }
    // else: the power changed on a skipped reading (unplugged while the speaker plays): the level stays as shown
    m.skipped = s_m.skipped;
    m.ext = s_m.ext;
    m.vfull = s_vfull;
    m.vfull_learn = s_m.vfull_learn;
    m.poll_ms = (int32_t)(s_m.fast_until - now) > 0 ? FAST_POLL_MS : POLL_MS;
    for (int i = 0; i < s_m.evq_n; i++) {
        board_bat_ev_t e = (board_bat_ev_t)s_m.evq[i].ev;
        log_rec(now, e, m.mv, s_m.evq[i].d, &m, p->power);
        m.why = e;
        ESP_LOGI(TAG, "%s: %s -> %s, %d mV (%d), %d %%, ext %s", board_bat_ev_name(e), board_power_name(p->power),
                 board_power_name(m.power), m.mv, s_m.evq[i].d, m.pct, board_ext_name(m.ext));
    }
    s_m.evq_n = 0;
    if ((int32_t)(now - s_m.log_next) >= 0) {
        log_rec(now, BOARD_BEV_MINUTE, s_m.min_mv, m.raw, &m, m.power);
        s_m.min_mv = -1;
        s_m.log_next += LOG_MIN_MS;
        if ((int32_t)(now - s_m.log_next) >= 0) {
            s_m.log_next = now + LOG_MIN_MS;    // readings stopped for a while: no catching up
        }
    }
    taskENTER_CRITICAL(&s_mux);
    board_bat_t before_ui = effective();
    s_model = m;
    board_bat_t after_ui = effective();
    taskEXIT_CRITICAL(&s_mux);
    return !same(&before_ui, &after_ui);
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

void board_battery_set_vfull(int mv)
{
    int v = mv >= VFULL_MIN_MV && mv <= VFULL_MAX_MV ? mv : VFULL_DEF_MV;
    if (v != s_m.vfull_learn) {
        s_m.vfull_learn = -1;       // set by hand: what was learned before no longer stands (a new learning does)
    }
    taskENTER_CRITICAL(&s_mux);
    s_vfull = v;
    s_model.vfull = v;
    s_model.vfull_learn = s_m.vfull_learn;
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
