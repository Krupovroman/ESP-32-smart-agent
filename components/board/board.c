#include "board.h"
#include "board_pins.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_io_expander_tca9554.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

static i2c_master_bus_handle_t s_i2c0;
static i2c_master_bus_handle_t s_i2c1;
static esp_io_expander_handle_t s_expander;
static uint8_t s_backlight = 0;

#define BL_LEDC_TIMER   LEDC_TIMER_0
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_LEDC_RES     LEDC_TIMER_10_BIT

void board_power_latch_on(void)
{
    // level before direction: the bootloader hook already drives it high, never let it dip
    gpio_set_level(BOARD_PIN_PWR_LATCH, 1);
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_PIN_PWR_LATCH,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(BOARD_PIN_PWR_LATCH, 1);
    // keep the pad latched high through software, watchdog and panic resets (reboot, OTA,
    // rollback): on battery the board would otherwise lose power before the bootloader hook
    // drives GPIO7 again. A power-on reset clears the hold.
    gpio_hold_en(BOARD_PIN_PWR_LATCH);
}

void board_power_off(void)
{
    board_backlight_set(0);
    gpio_hold_dis(BOARD_PIN_PWR_LATCH);
    gpio_set_level(BOARD_PIN_PWR_LATCH, 0);
}

static esp_err_t init_i2c(void)
{
    i2c_master_bus_config_t bus0 = {
        .i2c_port = BOARD_I2C0_PORT,
        .sda_io_num = BOARD_PIN_I2C0_SDA,
        .scl_io_num = BOARD_PIN_I2C0_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus0, &s_i2c0), TAG, "i2c0");

    // The touch bus has no pull-ups on the PCB
    i2c_master_bus_config_t bus1 = {
        .i2c_port = BOARD_I2C1_PORT,
        .sda_io_num = BOARD_PIN_TP_SDA,
        .scl_io_num = BOARD_PIN_TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus1, &s_i2c1), TAG, "i2c1");
    return ESP_OK;
}

static esp_err_t init_expander(void)
{
    ESP_RETURN_ON_ERROR(esp_io_expander_new_i2c_tca9554(s_i2c0, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &s_expander),
                        TAG, "tca9554");
    const uint32_t outs = BOARD_EXIO_TP_RST | BOARD_EXIO_LCD_RST | BOARD_EXIO_SD_CS;
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(s_expander, outs, IO_EXPANDER_OUTPUT), TAG, "dir out");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(s_expander, BOARD_EXIO_IMU_INT1 | BOARD_EXIO_IMU_INT2, IO_EXPANDER_INPUT),
                        TAG, "dir in");
    esp_io_expander_set_level(s_expander, BOARD_EXIO_SD_CS, 1);

    // Reset LCD and touch together
    const uint32_t rst = BOARD_EXIO_TP_RST | BOARD_EXIO_LCD_RST;
    esp_io_expander_set_level(s_expander, rst, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_io_expander_set_level(s_expander, rst, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_io_expander_set_level(s_expander, rst, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

static esp_err_t init_backlight(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_LEDC_RES,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");
    ledc_channel_config_t ch = {
        .gpio_num = BOARD_PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_LEDC_CHANNEL,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
    };
    return ledc_channel_config(&ch);
}

static void init_keys(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOARD_PIN_BOOT_KEY) | (1ULL << BOARD_PIN_PWR_KEY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cfg);
}

esp_err_t board_init(void)
{
    init_keys();
    ESP_RETURN_ON_ERROR(init_i2c(), TAG, "i2c");
    ESP_RETURN_ON_ERROR(init_expander(), TAG, "expander");
    ESP_RETURN_ON_ERROR(init_backlight(), TAG, "backlight");
    return ESP_OK;
}

i2c_master_bus_handle_t board_i2c0(void) { return s_i2c0; }
i2c_master_bus_handle_t board_i2c1(void) { return s_i2c1; }
esp_io_expander_handle_t board_expander(void) { return s_expander; }

void board_backlight_set(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    s_backlight = percent;
    // perceptual curve: duty ~ percent^2
    uint32_t max = (1u << BL_LEDC_RES) - 1;
    uint32_t duty = (uint32_t)percent * percent * max / 10000;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL);
}

uint8_t board_backlight_get(void) { return s_backlight; }

bool board_boot_key_pressed(void) { return gpio_get_level(BOARD_PIN_BOOT_KEY) == 0; }
bool board_pwr_key_pressed(void) { return gpio_get_level(BOARD_PIN_PWR_KEY) == 0; }
