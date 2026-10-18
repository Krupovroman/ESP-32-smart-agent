// Battery voltage on GPIO8 (ADC1 channel 7) behind the board's x3 divider (docs/hardware.md), and the battery
// model of docs/ui.md 3.16: a median of 5 readings taken 5 s apart, a Li-ion discharge curve, and charging seen
// from the USB host or a rising voltage (the ETA6098 charger only drives an LED, no status pin reaches the MCU).
#include "board.h"
#include "board_pins.h"

#include <stdbool.h>
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "battery";

#define N_READS     8
#define DIVIDER     3

#define MEDIAN_N        5           // readings in the median (25 s at the app's 5 s period)
#define TREND_N         24          // 2 min of filtered readings
#define TREND_UP_MV     15          // a rise this big within 2 min below FULL_MV: a charger without a USB host
#define TREND_FALL_MV   10          // a fall this big within 2 min ends it
#define PEAK_DROP_MV    25          // ... and so does a reading this far below the highest one since (unplugged)
#define FULL_MV         4150
#define PCT_AGAINST     5           // the percent moves against the charge direction only by this much at once

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
static bool s_tried, s_ok;

// model (app task), published under s_mux
static int s_raw[MEDIAN_N], s_raw_n, s_raw_i;
static int s_hist[TREND_N], s_hist_n, s_hist_i;
static bool s_trend;
static int s_peak;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static board_bat_t s_model = { .pct = -1, .mv = -1 };
static bool s_sim_on, s_sim_chg;
static int s_sim_pct;

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

static int median(void)
{
    int v[MEDIAN_N] = { 0 };
    for (int i = 0; i < s_raw_n; i++) {
        int x = s_raw[i], j = i;
        for (; j > 0 && v[j - 1] > x; j--) {
            v[j] = v[j - 1];
        }
        v[j] = x;
    }
    return v[s_raw_n / 2];
}

// What the UI sees: the model, or the console's override
static board_bat_t effective(void)
{
    board_bat_t b = s_model;
    if (s_sim_on) {
        b.pct = s_sim_pct;
        b.full = s_sim_chg && s_sim_pct >= 100;
        b.charging = s_sim_chg && !b.full;
        b.sim = true;
    }
    return b;
}

static bool same(const board_bat_t *a, const board_bat_t *b)
{
    return a->pct == b->pct && a->charging == b->charging && a->full == b->full && a->usb == b->usb;
}

bool board_battery_sample(void)
{
    int raw = board_battery_mv();
    board_bat_t m = { .pct = -1, .mv = -1, .usb = usb_serial_jtag_is_connected() };
    if (raw > 0) {
        s_raw[s_raw_i] = raw;
        s_raw_i = (s_raw_i + 1) % MEDIAN_N;
        if (s_raw_n < MEDIAN_N) {
            s_raw_n++;
        }
        int mv = median();
        // the filtered reading 2 min ago is the one about to be overwritten
        int old = s_hist_n == TREND_N ? s_hist[s_hist_i] : -1;
        s_hist[s_hist_i] = mv;
        s_hist_i = (s_hist_i + 1) % TREND_N;
        if (s_hist_n < TREND_N) {
            s_hist_n++;
        }
        if (old > 0 && !s_trend && mv - old >= TREND_UP_MV && mv < FULL_MV) {
            s_trend = true;
            s_peak = mv;
        } else if (old > 0 && s_trend && old - mv >= TREND_FALL_MV) {
            s_trend = false;
        }
        if (s_trend && mv > s_peak) {
            s_peak = mv;
        } else if (s_trend && s_peak - mv >= PEAK_DROP_MV) {
            s_trend = false;
        }
        m.mv = mv;
        m.pct = board_battery_pct(mv);
        if (m.pct >= 0) {
            bool fed = m.usb || s_trend;
            m.full = fed && mv >= FULL_MV;
            m.charging = fed && !m.full;
        }
    }
    taskENTER_CRITICAL(&s_mux);
    board_bat_t before = effective();
    const board_bat_t *p = &s_model;
    if (m.pct >= 0 && p->pct >= 0 && m.charging == p->charging && m.full == p->full) {
        // the voltage sags under load (Wi-Fi bursts, the speaker): the percent follows the charge direction and
        // moves against it only when the change is large
        int d = m.pct - p->pct;
        bool rising = m.charging || m.full;
        if ((rising && d < 0 && d > -PCT_AGAINST) || (!rising && d > 0 && d < PCT_AGAINST)) {
            m.pct = p->pct;
        }
    }
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

void board_battery_sim(int pct, bool charging)
{
    taskENTER_CRITICAL(&s_mux);
    s_sim_on = pct >= 0;
    s_sim_pct = pct > 100 ? 100 : pct;
    s_sim_chg = charging;
    taskEXIT_CRITICAL(&s_mux);
}
