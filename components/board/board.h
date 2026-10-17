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
// Rough LiPo state of charge 0..100 for mv; -1 when mv is outside a single cell's range (no battery, USB).
int board_battery_pct(int mv);
