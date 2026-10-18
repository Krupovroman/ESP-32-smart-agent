// Host stand-ins for what battery.c (and board.h) include: ADC one-shot + calibration, USB-Serial-JTAG presence,
// the clock, logging, PSRAM allocation and FreeRTOS critical sections. The test (t_battery.c) drives them.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1

// the clock: the test moves g_now_us forward
extern int64_t g_now_us;
static inline int64_t esp_timer_get_time(void)
{
    return g_now_us;
}

// the USB host's start-of-frame packets
extern bool g_usb;
static inline bool usb_serial_jtag_is_connected(void)
{
    return g_usb;
}

// the ADC returns the simulated cell (mV at the pin, before the x3 divider) for every read
int sim_adc_pin_mv(void);

typedef int gpio_num_t;
#define GPIO_NUM_0 0
#define GPIO_NUM_1 1
#define GPIO_NUM_2 2
#define GPIO_NUM_3 3
#define GPIO_NUM_4 4
#define GPIO_NUM_5 5
#define GPIO_NUM_6 6
#define GPIO_NUM_7 7
#define GPIO_NUM_8 8
#define GPIO_NUM_10 10
#define GPIO_NUM_11 11
#define GPIO_NUM_15 15
#define GPIO_NUM_18 18
#define GPIO_NUM_21 21
#define GPIO_NUM_38 38
#define GPIO_NUM_39 39
#define GPIO_NUM_40 40
#define GPIO_NUM_41 41
#define GPIO_NUM_42 42
#define GPIO_NUM_45 45
#define GPIO_NUM_46 46
#define GPIO_NUM_47 47
#define GPIO_NUM_48 48
#define SPI2_HOST 1

typedef enum { ADC_UNIT_1, ADC_UNIT_2 } adc_unit_t;
typedef int adc_channel_t;
typedef enum { ADC_ATTEN_DB_12 = 3 } adc_atten_t;
typedef enum { ADC_BITWIDTH_DEFAULT = 0 } adc_bitwidth_t;
typedef void *adc_oneshot_unit_handle_t;
typedef void *adc_cali_handle_t;
typedef struct {
    adc_unit_t unit_id;
} adc_oneshot_unit_init_cfg_t;
typedef struct {
    adc_atten_t atten;
    adc_bitwidth_t bitwidth;
} adc_oneshot_chan_cfg_t;
typedef struct {
    adc_unit_t unit_id;
    adc_channel_t chan;
    adc_atten_t atten;
    adc_bitwidth_t bitwidth;
} adc_cali_curve_fitting_config_t;

static inline esp_err_t adc_oneshot_io_to_channel(int io, adc_unit_t *unit, adc_channel_t *ch)
{
    *unit = ADC_UNIT_1;
    *ch = io - 1;
    return ESP_OK;
}
static inline esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *c, adc_oneshot_unit_handle_t *h)
{
    (void)c;
    *h = (void *)1;
    return ESP_OK;
}
static inline esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t h, adc_channel_t ch,
                                                   const adc_oneshot_chan_cfg_t *c)
{
    (void)h, (void)ch, (void)c;
    return ESP_OK;
}
static inline esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *c,
                                                             adc_cali_handle_t *h)
{
    (void)c;
    *h = (void *)1;
    return ESP_OK;
}
static inline esp_err_t adc_oneshot_get_calibrated_result(adc_oneshot_unit_handle_t h, adc_cali_handle_t c,
                                                          adc_channel_t ch, int *mv)
{
    (void)h, (void)c, (void)ch;
    *mv = sim_adc_pin_mv();
    return ESP_OK;
}
static inline esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t h, adc_channel_t ch, int *raw)
{
    (void)h, (void)ch;
    *raw = sim_adc_pin_mv() * 4095 / 3100;
    return ESP_OK;
}

#define MALLOC_CAP_SPIRAM 1
#define heap_caps_calloc(n, s, caps) calloc(n, s)

void host_log(const char *lvl, const char *tag, const char *fmt, ...);
#define ESP_LOGE(tag, ...) host_log("E", tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) host_log("W", tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) host_log("I", tag, __VA_ARGS__)

typedef struct {
    int x;
} portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }
#define taskENTER_CRITICAL(m) ((void)(m))
#define taskEXIT_CRITICAL(m) ((void)(m))

typedef void *i2c_master_bus_handle_t;
typedef void *esp_io_expander_handle_t;
