#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "esp_io_expander.h"

// Must be the first call in app_main: keeps the board on when running from battery.
void board_power_latch_on(void);
// Releases the latch; on battery the board switches off.
void board_power_off(void);

// I2C buses, IO expander (LCD/touch reset pulse), backlight PWM, buttons.
esp_err_t board_init(void);

i2c_master_bus_handle_t board_i2c0(void);
i2c_master_bus_handle_t board_i2c1(void);
esp_io_expander_handle_t board_expander(void);

void board_backlight_set(uint8_t percent);
uint8_t board_backlight_get(void);

bool board_boot_key_pressed(void);
bool board_pwr_key_pressed(void);

// ---- Buttons (buttons.c, architecture 1.2): GPIO polled every 10 ms, 30 ms debounce.
// BOOT reports DOWN and UP (held_ms) only, so hold-to-talk can last any time. PWR reports DOWN, UP,
// CLICK (released before 600 ms) and LONG (3 s, once per press); its events are ignored until PWR has
// been seen released once after boot (on battery the user holds it to switch the board on).
typedef enum { BOARD_BTN_BOOT, BOARD_BTN_PWR, BOARD_BTN__N } board_btn_t;
typedef enum { BTN_EV_DOWN, BTN_EV_UP, BTN_EV_CLICK, BTN_EV_LONG } board_btn_ev_t;
// Runs in the esp_timer task: post, never block.
typedef void (*board_btn_cb_t)(board_btn_t b, board_btn_ev_t ev, uint32_t held_ms, void *ctx);

esp_err_t board_buttons_start(board_btn_cb_t cb, void *ctx);
// Console simulation: the button reads as pressed while set, through the same debounce and event logic.
void board_buttons_simulate(board_btn_t b, bool pressed);
// Debounced state (true = pressed).
bool board_button_down(board_btn_t b);
const char *board_btn_name(board_btn_t b);
const char *board_btn_ev_name(board_btn_ev_t ev);

// ---- Battery (battery.c): ADC1 channel 7 (GPIO8) behind a x3 divider, curve-fitting calibration.
// Battery voltage in mV (average of 8 reads), or -1 when the ADC is unavailable.
int board_battery_mv(void);
// Li-ion state of charge 0..100 for a resting voltage (discharge curve, not linear); -1 when mv is outside a
// single cell's range (no battery).
int board_battery_pct(int mv);

// Battery model v3 (docs/ui.md 3.16). The ETA6098 charger has no status pin (nor VBUS) to the MCU and the system load
// hangs on the cell, so external power is seen from the USB host's start-of-frame packets
// (usb_serial_jtag_is_connected()) or from a voltage step (a settled reading 35 mV above the settled readings just
// before it: a wall charger sends no packets), and it does not mean "charging": a PC port gives about 0.5 A and the
// speaker at full volume takes more, and a full cell takes nothing. The voltage decides:
typedef enum {
    BOARD_PWR_BATTERY,      // no external power seen (also while a plug-in's step is being measured, a few seconds)
    BOARD_PWR_CHARGING,     // external power, the settled voltage stepped up on plugging in or keeps rising
    BOARD_PWR_FULL,         // external power, the charger not charging: the settled level flat for 4 min near
                            // v_full (15 min anywhere >= 3.95 V), a plug-in step under 15 mV near v_full, or the
                            // charger stopping near v_full (a step down or a fall with the host); a new rise or step
                            // up ends it, with the host also a fall across a long session or a level under v_full -
                            // 160 mV (USB_DRAIN): the cell relaxing and the load draining it to the recharge level
                            // after termination is no exit
    BOARD_PWR_USB_DRAIN,    // USB host present but the settled voltage fell >= 20 mV within 3 min, below v_full -
                            // 120 mV or across a long session (readings skipped >= 60 s: the speaker): the load
                            // takes more than the port gives (or the charger stopped low); back to CHARGING on a
                            // settled rise of 12 mV
    BOARD_PWR__N
} board_power_t;

// Where the external power is known from
typedef enum {
    BOARD_EXT_NONE,
    BOARD_EXT_USB,          // the USB host's packets
    BOARD_EXT_CHARGER,      // a voltage step up (or a 2 min rise) without a host: a wall charger
    BOARD_EXT_KEPT,         // the host's packets stopped without a voltage step down: still plugged (macOS suspends
                            // an idle CDC device), until a step down or a fall
    BOARD_EXT__N
} board_ext_t;

// Why the model last changed its mind (the battery log's events, "bat log")
typedef enum {
    BOARD_BEV_MINUTE,       // not an event: the log's record of the minute
    BOARD_BEV_BOOT,         // the first reading
    BOARD_BEV_USB,          // the USB host appeared (d = the settled level before, -1 none; back while the power
                            // was still seen: at most one in 2 min)
    BOARD_BEV_STEP,         // ... the plug-in step measured >= 15 mV: CHARGING (d = the step)
    BOARD_BEV_NO_STEP,      // ... under 15 mV: the charger did not start, FULL near v_full (d = the step)
    BOARD_BEV_NO_REF,       // ... no settled level to measure it from (booted on USB, under load): CHARGING
    BOARD_BEV_USB_LOST,     // the host's packets stopped, no settled reading to look for a step: power kept
    BOARD_BEV_USB_LOST_NO_STEP, // ... and no step down in the settled readings after it: power kept (d = readings)
    BOARD_BEV_STEP_UP,      // a settled step up >= 35 mV: a charger plugged in, or charging again (d = the step)
    BOARD_BEV_STEP_DOWN,    // a settled step down >= 35 mV without a host: unplugged (d = the step); on battery
                            // (ext none): a load the app does not grade
    BOARD_BEV_STEP_SMALL,   // a confirmed step of 15-34 mV: only logged, at most one in 2 min (d = the step)
    BOARD_BEV_CHG_STOP,     // ... with the host still there, or CHARGING's settled level 20 mV under 3 min ago (30
                            // under its peak) with no long session meanwhile: the charger stopped (FULL at >= v_full
                            // - 120 mV, else USB_DRAIN)
    BOARD_BEV_TREND_UP,     // calm readings rose 15 mV within 2 min: a charger whose step was missed (d = the rise)
    BOARD_BEV_FALL,         // no host: the settled level 35 mV (FULL: 60) under its highest: power lost (d = fall)
    BOARD_BEV_PLATEAU,      // CHARGING: not risen > 5 mV in 4 min at >= v_full - 100 mV (> 10 mV in 15 min at
                            // >= 3.95 V): FULL (d = the change)
    BOARD_BEV_RISE,         // FULL: a calm level 20 mV above its lowest, rising: CHARGING again (d = the rise)
    BOARD_BEV_FULL_EXIT,    // FULL with a host, the level under v_full - 160 mV: the charger does not take it back
                            // up: USB_DRAIN (d = the fall from its highest)
    BOARD_BEV_DRAIN,        // the settled level fell with a host, under v_full - 120 mV or across a long session:
                            // USB_DRAIN (d = the fall)
    BOARD_BEV_DRAIN_END,    // ... rose 12 mV from its lowest, or the host went: CHARGING (d = the rise)
    BOARD_BEV_NOBAT,        // settled readings swing > 250 mV: no cell (d = the spread)
    BOARD_BEV_CELL,         // ... they agree again
    BOARD_BEV_VFULL,        // v_full learned (d = the new value, mV): a FULL's highest level once it held 15 min
                            // (lower only after the 4-min plateau, by 30 mV at most per charge), or higher when it
                            // ends; changes under 8 mV are not taken
    BOARD_BEV_LOAD_END,     // on battery, a step up back to the level before a step down of the last 10 min: a
                            // load the app does not grade ended, no charger (d = the step)
    BOARD_BEV__N
} board_bat_ev_t;

// How much a reading can be trusted (the app knows the load, app_io.c)
typedef enum {
    BOARD_BAT_SKIP,         // speaker playing or a turn's Wi-Fi lately: the cell sags, the reading is not taken (the
                            // model keeps its last good value; USB presence is still refreshed)
    BOARD_BAT_LOADED,       // a load ended lately: the cell still recovers; the reading sets the level only
    BOARD_BAT_SETTLED,      // no load step for 60 s: the reading counts for steps, USB_DRAIN and FULL; after 2 min
                            // more of them it may also start the wall-charger trend (a long session's recovery takes
                            // minutes)
} board_bat_q_t;

typedef struct {
    int pct;            // 0..100, -1 = no battery reading (or settled readings swinging: no cell on the charger)
    int mv;             // median of the last 5 readings, -1 = none
    board_power_t power;
    bool charging;      // power == CHARGING
    bool full;          // power == FULL
    bool usb;           // USB host connected
    bool sim;           // pct / power come from board_battery_sim()
    int raw;            // last reading taken (mV), -1 = none
    int d3;             // settled voltage now minus 3 min ago (mV), valid when d3_ok
    bool d3_ok;
    int d4;             // ... minus 4 min ago, calm all the time (the FULL plateau), valid when d4_ok
    bool d4_ok;
    int ofs;            // charging offset in use (mV): the plug-in step, 60 without a reference
    unsigned skipped;   // readings skipped for load since boot
    board_ext_t ext;    // external power and where it is known from (NONE while a plug-in is still measured)
    int vfull;          // v_full in use (mV): the divider's reading of a full cell on the charger = 100 %
    int vfull_learn;    // v_full learned since boot or since it was last set, -1 = none (the app stores it:
                        // bat_vfull)
    int poll_ms;        // when the model wants the next reading: 5000, 2000 while a step is being measured
    board_bat_ev_t why; // the last event
} board_bat_t;

// One reading into the model (median of 5, steps, trends); the app calls it every poll_ms. True when pct, power or
// usb changed.
bool board_battery_sample(board_bat_q_t q);
// Latest model state (any task).
void board_battery_state(board_bat_t *out);
// v_full (the setting bat_vfull, 4000-4250 mV; out of range = 4150): the reading that maps to 100 % (the curve is
// scaled by 4200 / v_full). A value other than the one learned clears vfull_learn. Call from the task that samples.
void board_battery_set_vfull(int mv);
// Console override for UI tests: pct 0..100 and the power state (FULL shows 100); pct < 0 ends it.
void board_battery_sim(int pct, board_power_t power);
// "battery", "charging", "full", "drain"
const char *board_power_name(board_power_t p);
// "none", "usb", "charger", "kept"
const char *board_ext_name(board_ext_t e);
// "min", "boot", "usb", "step", "no_step", ... (the enum's names in lower case)
const char *board_bat_ev_name(board_bat_ev_t e);

// Battery log (PSRAM ring, about 12 h): one record a minute and one per event, numbered from boot.
typedef struct {
    uint32_t t_s;       // uptime (s)
    int16_t mv;         // a minute: its last settled level (-1 = none: every reading skipped or under load);
                        // an event: the level then
    int16_t d;          // a minute: the last reading taken (-1 none); an event: the mV that decided it (see the
                        // board_bat_ev_t comments)
    int8_t pct;
    uint8_t ev;         // board_bat_ev_t (BOARD_BEV_MINUTE for the minute's record)
    uint8_t power;      // board_power_t (an event: after it)
    uint8_t from;       // an event: the power state before it
    uint8_t usb;        // USB host present
    uint8_t ext;        // board_ext_t
} board_bat_log_t;

#define BOARD_BAT_LOG_N 1536
// Records first..next-1 are kept; returns next (first = the oldest kept)
uint32_t board_battery_log_range(uint32_t *first);
// Record number seq; false when it is not kept (overwritten, cleared or not written yet)
bool board_battery_log_get(uint32_t seq, board_bat_log_t *out);
// Forgets the records so far (the numbering goes on)
void board_battery_log_clear(void);
