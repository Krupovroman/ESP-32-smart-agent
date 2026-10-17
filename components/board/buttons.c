// BOOT and PWR keys (architecture 1.2, docs/ui.md 2.3): polled from an esp_timer, debounced, turned into
// DOWN / UP / CLICK / LONG events. Raw GPIO polling instead of espressif/button: no extra managed
// component, and the PWR arming rule needs the state at boot anyway.
#include "board.h"
#include "board_pins.h"

#include <stdatomic.h>
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_timer.h"

static const char *TAG = "buttons";

#define POLL_US         10000       // 10 ms
#define DEBOUNCE_N      3           // 30 ms of agreeing samples
#define PWR_CLICK_MS    600         // ui.md 2.3: PWR short = released before 600 ms
#define PWR_LONG_MS     3000        // PWR hold 3 s = power off

typedef struct {
    gpio_num_t pin;
    uint32_t click_ms, long_ms;     // 0 = no CLICK / no LONG events
    bool armed;                     // events are reported (PWR: after its first release)
    bool down;                      // debounced state
    uint8_t cnt;                    // samples disagreeing with down
    bool long_sent;
    int64_t t_down_us;
    atomic_bool sim;
} btn_t;

static btn_t s_btn[BOARD_BTN__N] = {
    [BOARD_BTN_BOOT] = { .pin = BOARD_PIN_BOOT_KEY },
    [BOARD_BTN_PWR] = { .pin = BOARD_PIN_PWR_KEY, .click_ms = PWR_CLICK_MS, .long_ms = PWR_LONG_MS },
};
static board_btn_cb_t s_cb;
static void *s_ctx;
static esp_timer_handle_t s_timer;

static bool raw_pressed(btn_t *b)
{
    return gpio_get_level(b->pin) == 0 || atomic_load(&b->sim);
}

static void emit(board_btn_t id, board_btn_ev_t ev, uint32_t held)
{
    if (s_cb && s_btn[id].armed) {
        s_cb(id, ev, held, s_ctx);
    }
}

static void poll_cb(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < BOARD_BTN__N; i++) {
        btn_t *b = &s_btn[i];
        bool p = raw_pressed(b);
        if (p == b->down) {
            b->cnt = 0;
        } else if (++b->cnt >= DEBOUNCE_N) {
            b->cnt = 0;
            b->down = p;
            if (p) {
                b->t_down_us = now;
                b->long_sent = false;
                emit((board_btn_t)i, BTN_EV_DOWN, 0);
            } else {
                uint32_t held = (uint32_t)((now - b->t_down_us) / 1000);
                if (!b->armed) {
                    b->armed = true;        // first release after boot: from now on PWR counts
                    continue;
                }
                emit((board_btn_t)i, BTN_EV_UP, held);
                if (b->click_ms && held < b->click_ms && !b->long_sent) {
                    emit((board_btn_t)i, BTN_EV_CLICK, held);
                }
            }
        }
        if (b->down && b->long_ms && !b->long_sent && now - b->t_down_us >= (int64_t)b->long_ms * 1000) {
            b->long_sent = true;
            emit((board_btn_t)i, BTN_EV_LONG, b->long_ms);
        }
    }
}

esp_err_t board_buttons_start(board_btn_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(!s_timer, ESP_ERR_INVALID_STATE, TAG, "already started");
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOARD_PIN_BOOT_KEY) | (1ULL << BOARD_PIN_PWR_KEY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "gpio");
    for (int i = 0; i < BOARD_BTN__N; i++) {
        btn_t *b = &s_btn[i];
        b->down = gpio_get_level(b->pin) == 0;
        b->t_down_us = esp_timer_get_time();
        b->long_sent = b->down;     // a key held through boot never fires LONG for that press
        // BOOT is reported at once; PWR only after it has been seen released (battery power-on hold)
        b->armed = i == BOARD_BTN_BOOT || !b->down;
    }
    s_cb = cb;
    s_ctx = ctx;
    const esp_timer_create_args_t ta = { .callback = poll_cb, .dispatch_method = ESP_TIMER_TASK, .name = "buttons" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&ta, &s_timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_timer, POLL_US), TAG, "start");
    return ESP_OK;
}

void board_buttons_simulate(board_btn_t b, bool pressed)
{
    if ((unsigned)b < BOARD_BTN__N) {
        atomic_store(&s_btn[b].sim, pressed);
    }
}

bool board_button_down(board_btn_t b)
{
    return (unsigned)b < BOARD_BTN__N && s_btn[b].down;
}

const char *board_btn_name(board_btn_t b)
{
    return b == BOARD_BTN_BOOT ? "boot" : b == BOARD_BTN_PWR ? "pwr" : "?";
}

const char *board_btn_ev_name(board_btn_ev_t ev)
{
    static const char *const n[] = { "down", "up", "click", "long" };
    return (unsigned)ev < sizeof(n) / sizeof(n[0]) ? n[ev] : "?";
}
