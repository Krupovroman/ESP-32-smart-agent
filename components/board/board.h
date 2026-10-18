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

// Battery model (docs/ui.md 3.16). The ETA6098 charger has no status pin and the system load hangs on the cell, so
// "external power" (the USB host's start-of-frame packets, usb_serial_jtag_is_connected(), or the settled voltage
// rising by 15 mV within 2 min: a plain wall charger sends no packets) does not mean "charging": a PC port gives
// about 0.5 A and the speaker at full volume takes more. The voltage decides:
typedef enum {
    BOARD_PWR_BATTERY,      // no external power seen
    BOARD_PWR_CHARGING,     // external power, the settled voltage not falling (the default right after plugging in)
    BOARD_PWR_FULL,         // external power, holding at >= 4.15 V for 3 min (left below 4.11 V)
    BOARD_PWR_USB_DRAIN,    // USB host present but the settled voltage fell >= 20 mV within 3 min: the load takes
                            // more than the port gives; back to CHARGING on a settled rise of 12 mV
    BOARD_PWR__N
} board_power_t;

// How much a reading can be trusted (the app knows the load, app_io.c)
typedef enum {
    BOARD_BAT_SKIP,         // speaker playing or a turn's Wi-Fi lately: the cell sags, the reading is not taken (the
                            // model keeps its last good value; USB presence is still refreshed)
    BOARD_BAT_LOADED,       // a load ended lately: the cell still recovers; the reading sets the level only
    BOARD_BAT_SETTLED,      // no load step for 60 s: the reading counts for USB_DRAIN and FULL; after 2 min more of
                            // them it may also start the wall-charger trend (a long session's recovery takes minutes)
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
    int ofs;            // charging offset in use (mV): the plug-in step, 60 without a reference
    unsigned skipped;   // readings skipped for load since boot
} board_bat_t;

// One reading into the model (median of 5, trends); the app calls it every 5 s. True when pct, power or usb changed.
bool board_battery_sample(board_bat_q_t q);
// Latest model state (any task).
void board_battery_state(board_bat_t *out);
// Console override for UI tests: pct 0..100 and the power state (FULL shows 100); pct < 0 ends it.
void board_battery_sim(int pct, board_power_t power);
// "battery", "charging", "full", "drain"
const char *board_power_name(board_power_t p);
