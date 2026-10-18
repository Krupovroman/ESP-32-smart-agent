// Battery voltage on GPIO8 (ADC1 channel 7) behind the board's x3 divider (docs/hardware.md).
#include "board.h"
#include "board_pins.h"

#include <stdbool.h>
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "battery";

#define N_READS     8
#define DIVIDER     3

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
static bool s_tried, s_ok;

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
    // resting LiPo curve, linear between the points
    static const struct {
        int mv, pct;
    } t[] = { { 3300, 0 }, { 3500, 5 }, { 3600, 12 }, { 3700, 30 }, { 3800, 50 }, { 3900, 65 }, { 4000, 78 },
              { 4100, 90 }, { 4200, 100 } };
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
