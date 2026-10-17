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
