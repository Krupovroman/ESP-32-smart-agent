// Battery model v3 tests (components/board/battery.c, docs/ui.md 3.16): battery.c compiled into this TU (to reach and
// reset its statics) on a simulated cell, charger and USB host. Time runs in 100 ms ticks. The app's grading of the
// readings (app_io.c bat_quality) is mimicked: SKIP while the speaker plays and 15 s after it, LOADED until 60 s
// after a load, SETTLED otherwise; readings are taken every poll_ms, as app_io.c does.
#include "battery.c"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>

int64_t g_now_us;
bool g_usb;
static bool s_verbose, s_trace;
static uint32_t s_seed_n, s_seed_r;     // -n / -r: the noise's and the usage schedules' seeds (0 = the defaults)

void host_log(const char *lvl, const char *tag, const char *fmt, ...)
{
    if (!s_verbose) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    printf("    [%s %s %.1f s] ", lvl, tag, (double)g_now_us / 1e6);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static int s_checks, s_fail;
#define CHECK(c)                                                              \
    do {                                                                      \
        s_checks++;                                                           \
        if (!(c)) {                                                           \
            s_fail++;                                                         \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                     \
    } while (0)

// ---- the simulated cell, as this divider reads it (mV at the cell, the ADC sees a third of it)

#define POL_TAU_S   40.0

static struct {
    double rest;            // the cell's resting reading
    double rate;            // its drift, mV per minute (charging +, the load draining -)
    double ohm;             // the net charge current x the internal resistance: at once ...
    double ohm_to, ramp;    // ... or, with a soft start, rising to ohm_to at ramp mV/s
    double pol, pol_to;     // the polarisation, relaxing towards pol_to
    double cv;              // the charger's CV level as read (0 = none): the cell is held there
    double sag;             // a load's sag (the speaker): at once
    double dep, dep2;       // a load's after-effect: the cell reads this low, recovering with DEP_TAU_S / DEP2_TAU_S
    double bl_mv;           // the backlight's sag per % of it (mV)
    double wave, wave_min;  // a slow wobble of the rest level: amplitude (mV) and period (min)
    double noise;           // +- per ADC read (mV at the cell)
    double jitter;          // +- per reading (all its ADC reads: the load of that moment)
    double burst_p, burst;  // a Wi-Fi burst: a reading this likely reads up to burst mV low
    double rd;              // this reading's jitter and burst
    int spike, spike_reads; // a spike on the next reads
    uint32_t rng;
} c;

#define DEP_TAU_S   40.0
#define DEP2_TAU_S  480.0

uint8_t g_bl;               // the backlight (board_backlight_get(), the cell sags c.bl_mv per %)

uint8_t board_backlight_get(void)
{
    return g_bl;
}

static double urand(void)   // 0..1
{
    c.rng = c.rng * 1664525u + 1013904223u;
    return (double)((c.rng >> 8) & 0xFFFF) / 65536.0;
}

int sim_adc_pin_mv(void)
{
    double n = (urand() * 2.0 - 1.0) * c.noise;
    double w = c.wave_min > 0 ? c.wave * sin((double)g_now_us / 60e6 / c.wave_min * 6.2831853) : 0;
    double v = c.rest + w + c.ohm + c.pol;
    if (c.cv > 0 && v > c.cv) {
        v = c.cv;
    }
    v -= c.sag + c.dep + c.dep2 + c.bl_mv * g_bl + c.rd;
    if (c.spike_reads > 0) {
        c.spike_reads--;
        v += c.spike;
    }
    return (int)((v + n) / 3.0 + 0.5);
}

// ---- the app's side: grading and the poll period

static struct {
    uint32_t audio_until;   // the speaker plays until (0 = not)
    uint32_t audio_end;     // ... it stopped at
    double audio_dep, audio_dep2;   // ... and leaves the cell this low (recovering)
    uint32_t turn_until;    // a turn (its Wi-Fi) runs until (0 = not)
    uint32_t turn_end;      // ... it ended at
    uint32_t screen_t;      // the backlight last changed
    uint32_t load_t;        // the last load step
    bool mono;              // watch the percent on battery: never up
    int bat_rise;           // ... the largest rise from one tick to the next while BATTERY
    int cap_last;           // ... the model's settled value at the last tick
    uint32_t t_bat;
    bool polled;
    int samples;
    // watched while the test runs
    int chg_run, chg_max;   // CHARGING: the current and the longest stretch (ms)
    int changes;            // power state changes
    int pct_min, pct_max;
    int drain_pct_max;      // the highest percent shown in USB_DRAIN
    int pct_last, drop_max; // the percent shown, and its largest fall from one reading to the next
    int full_n;             // times FULL was entered (the lock screen's glow, A39)
    board_power_t last;
} a;

static uint32_t now_ms(void)
{
    return (uint32_t)(g_now_us / 1000);
}

static board_bat_t st(void)
{
    board_bat_t b;
    board_battery_state(&b);
    return b;
}

static board_bat_q_t quality(uint32_t now)
{
    bool busy = a.audio_until || (a.audio_end && now - a.audio_end < 15000) || a.turn_until ||
                (a.turn_end && now - a.turn_end < 20000) ||
                (a.screen_t && now - a.screen_t < 3000);    // a backlight fade and its first recovery
    if (busy) {
        // at <= 20 % on battery the reading is taken under the load (for the low-battery warnings, app_io.c)
        board_bat_t b = st();
        return !b.sim && b.power == BOARD_PWR_BATTERY && b.pct >= 0 && b.pct <= 20 ? BOARD_BAT_LOADED : BOARD_BAT_SKIP;
    }
    return now - a.load_t >= 60000 ? BOARD_BAT_SETTLED : BOARD_BAT_LOADED;
}

static void watch_reset(void)
{
    board_bat_t b = st();
    a.chg_run = a.chg_max = a.changes = 0;
    a.pct_min = a.pct_max = a.pct_last = b.pct;
    a.drain_pct_max = -1;
    a.drop_max = a.full_n = a.bat_rise = 0;
    a.last = b.power;
}

static void tick(void)
{
    g_now_us += 100000;
    uint32_t now = now_ms();
    c.rest += c.rate / 600.0;
    c.pol += (c.pol_to - c.pol) * (0.1 / POL_TAU_S);
    c.dep -= c.dep * (0.1 / DEP_TAU_S);
    c.dep2 -= c.dep2 * (0.1 / DEP2_TAU_S);
    if (c.ramp > 0 && c.ohm < c.ohm_to) {
        c.ohm = c.ohm + c.ramp * 0.1 > c.ohm_to ? c.ohm_to : c.ohm + c.ramp * 0.1;
    }
    if (a.audio_until && now >= a.audio_until) {
        a.audio_until = 0;
        a.audio_end = a.load_t = now;
        c.sag = 0;
        c.dep = c.dep + a.audio_dep > 40 ? 40 : c.dep + a.audio_dep;
        c.dep2 = c.dep2 + a.audio_dep2 > 25 ? 25 : c.dep2 + a.audio_dep2;
    }
    if (a.turn_until && now >= a.turn_until) {
        a.turn_until = 0;
        a.turn_end = a.load_t = now;
    }
    board_bat_t b = st();
    uint32_t poll = b.poll_ms > 0 ? (uint32_t)b.poll_ms : 5000;
    if (!a.polled || now - a.t_bat >= poll) {
        a.polled = true;
        a.t_bat = now;
        a.samples++;
        c.rd = 0;
        if (c.jitter > 0) {
            c.rd += (urand() * 2.0 - 1.0) * c.jitter;
        }
        if (c.burst_p > 0 && urand() < c.burst_p) {
            c.rd += urand() * c.burst;
        }
        board_battery_sample(quality(now));
        b = st();
        if (s_trace) {
            printf("    %8.1f q%d raw %d mv %d %s ext %s d3 %d%s d4 %d%s pct %d peak %d base %d/%d/%d | rest %.0f dep "
                   "%.0f/%.0f bl %d rd %.0f ceil %d\n",
                   now / 1000.0, (int)quality(now), b.raw, b.mv, board_power_name(b.power), board_ext_name(b.ext),
                   b.d3, b.d3_ok ? "" : "x", b.d4, b.d4_ok ? "" : "x", b.pct, s_m.level_peak, base_ref(now, g_bl, NULL),
                   s_m.base_n, s_m.base_hit, c.rest, c.dep, c.dep2, g_bl, c.rd, ceil_lvl(now, 0));
        }
    }
    // on battery the percent never rises; at <= LOW_PCT a dip under load (taken for the warnings) comes back up to
    // the settled value (s_m.cap) at most, and that never rises
    bool dip_back = b.pct <= s_m.cap && s_m.cap <= LOW_PCT && s_m.cap <= a.cap_last;
    if (a.mono && b.power == BOARD_PWR_BATTERY && a.last == BOARD_PWR_BATTERY && a.pct_last >= 0 && !dip_back &&
        b.pct - a.pct_last > a.bat_rise) {
        a.bat_rise = b.pct - a.pct_last;
        if (s_verbose) {
            printf("    [percent up on battery at %.1f s: %d -> %d, %d mV]\n", now / 1000.0, a.pct_last, b.pct, b.mv);
        }
    }
    a.cap_last = b.power == BOARD_PWR_BATTERY ? s_m.cap : 1000;
    if (b.power != a.last) {
        a.changes++;
        a.full_n += b.power == BOARD_PWR_FULL;
        a.last = b.power;
    }
    if (b.pct >= 0 && a.pct_last >= 0 && a.pct_last - b.pct > a.drop_max) {
        a.drop_max = a.pct_last - b.pct;
    }
    a.pct_last = b.pct;
    a.chg_run = b.power == BOARD_PWR_CHARGING ? a.chg_run + 100 : 0;
    a.chg_max = a.chg_run > a.chg_max ? a.chg_run : a.chg_max;
    if (b.pct >= 0) {
        a.pct_min = a.pct_min < 0 || b.pct < a.pct_min ? b.pct : a.pct_min;
        a.pct_max = b.pct > a.pct_max ? b.pct : a.pct_max;
    }
    if (b.power == BOARD_PWR_USB_DRAIN && b.pct > a.drain_pct_max) {
        a.drain_pct_max = b.pct;
    }
}

static void run(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 100) {
        tick();
    }
}

// Runs until the power state is p (at most ms); the time it took, -1 if it did not happen
static int until_power(board_power_t p, uint32_t ms)
{
    for (uint32_t t = 0; t <= ms; t += 100) {
        if (st().power == p) {
            return (int)t;
        }
        tick();
    }
    return -1;
}

static void speaker(uint32_t ms, double sag)
{
    a.audio_until = now_ms() + ms;
    a.load_t = now_ms();
    a.audio_dep = a.audio_dep2 = 0;
    c.sag = sag;
}

// ... leaving the cell dep (recovering within a minute) and dep2 (over ~10 min) low when it ends
static void speaker_d(uint32_t ms, double sag, double dep, double dep2)
{
    speaker(ms, sag);
    a.audio_dep = dep;
    a.audio_dep2 = dep2;
}

// A turn without sound (its Wi-Fi / TLS): skipped readings while it runs and 20 s after, loaded for 60 s after it
static void turn(uint32_t ms)
{
    a.turn_until = now_ms() + ms;
    a.load_t = now_ms();
}

// A new boot on a cell resting at rest (mV as read), drifting by rate mV/min, the USB host as given; boot_chg: on a
// wall charger already (ohm and pol built up)
static void boot_chg(double rest, double rate, bool usb, double ohm, double pol)
{
    memset(&s_m, 0, sizeof(s_m));           // the next reading starts the model afresh
    s_vfull = VFULL_DEF_MV;
    s_sim_on = false;
    memset(&c, 0, sizeof(c));
    c.rest = rest;
    c.rate = rate;
    c.ohm = c.ohm_to = ohm;
    c.pol = c.pol_to = pol;
    c.noise = 12;                           // +-12 mV per ADC read at the cell: about 3 mV on an 8-read average
    c.rng = 12345 + s_seed_n * 104729u;
    g_usb = usb;
    g_bl = 0;
    g_now_us += 1000000;
    memset(&a, 0, sizeof(a));
    a.load_t = now_ms();                    // booting is a load (Wi-Fi, TLS): settled after a minute
    board_battery_log_clear();
    tick();
    watch_reset();
}

static void boot(double rest, double rate, bool usb)
{
    boot_chg(rest, rate, usb, 0, 0);
}

// The charger: plugged in (ohm at once, pol within a minute, the cell rising at rate) or out
static void charger(double ohm, double pol, double rate, double cv)
{
    c.ohm = c.ohm_to = ohm;
    c.ramp = 0;
    c.pol_to = pol;
    c.rate = rate;
    c.cv = cv;
}

// ... with a soft start: the charge current (ohm) ramps in over ramp_s seconds
static void charger_soft(double ohm, double pol, double rate, double cv, double ramp_s)
{
    charger(0, pol, rate, cv);
    c.ohm_to = ohm;
    c.ramp = ohm / ramp_s;
}

// The backlight set to bl %: a load step whoever makes it (app_io.c: 3 s skipped, loaded for a minute)
static void screen(int bl)
{
    if (g_bl != bl) {
        g_bl = (uint8_t)bl;
        a.load_t = a.screen_t = now_ms();
    }
}

// Was there an event e among the log's records since seq?
static bool logged(board_bat_ev_t e, uint32_t since)
{
    uint32_t first, next = board_battery_log_range(&first);
    board_bat_log_t r;
    for (uint32_t i = since > first ? since : first; i < next; i++) {
        if (board_battery_log_get(i, &r) && r.ev == e) {
            return true;
        }
    }
    return false;
}

static uint32_t log_next(void)
{
    return board_battery_log_range(NULL);
}

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

// ---- tests

// (a) a wall charger (no host) into a 60 % cell: the plug-in step, CHARGING within 10 s; (d) unplugged: the step
// down, BATTERY within 10 s, the percent where it was
static void t_wall_plug_unplug(void)
{
    printf("t_wall_plug_unplug\n");
    boot(3830, -0.3, false);
    run(5 * 60000);
    board_bat_t b = st();
    CHECK(b.power == BOARD_PWR_BATTERY && b.ext == BOARD_EXT_NONE && b.pct >= 57 && b.pct <= 63);
    CHECK(b.poll_ms == 5000);
    int pct0 = b.pct;
    uint32_t seq = log_next();
    charger(60, 25, 2, 0);
    int t = until_power(BOARD_PWR_CHARGING, 15000);
    CHECK(t >= 0 && t <= 10000);
    b = st();
    CHECK(b.ext == BOARD_EXT_CHARGER && b.why == BOARD_BEV_STEP_UP && logged(BOARD_BEV_STEP_UP, seq));
    CHECK(iabs(b.pct - pct0) <= 1);
    // charging for 20 min: no FULL below 3.95 V, the percent never down, the offset measured from the step
    watch_reset();
    run(20 * 60000);
    b = st();
    CHECK(b.power == BOARD_PWR_CHARGING && a.changes == 0 && a.pct_min >= pct0);
    CHECK(b.ofs >= 65 && b.ofs <= 90);
    CHECK(b.poll_ms == 5000);
    int pct1 = b.pct;
    CHECK(pct1 >= pct0 + 2);                // 40 mV gained
    seq = log_next();
    charger(0, 0, -0.3, 0);
    t = until_power(BOARD_PWR_BATTERY, 15000);
    CHECK(t >= 0 && t <= 10000);
    b = st();
    CHECK(b.ext == BOARD_EXT_NONE && logged(BOARD_BEV_STEP_DOWN, seq));
    CHECK(iabs(b.pct - pct1) <= 2);
    // the polarisation relaxes: no jump later either
    watch_reset();
    run(5 * 60000);
    CHECK(st().power == BOARD_PWR_BATTERY && a.pct_min >= pct1 - 3 && a.pct_max <= pct1);
}

// (b) a PC port (host) into a nearly full cell whose charger does not start (its LED at "full"): no step, FULL within
// seconds, the bolt never shown
static void t_usb_full_no_step(void)
{
    printf("t_usb_full_no_step\n");
    boot(4120, -0.2, false);
    run(5 * 60000);
    board_bat_t b = st();
    CHECK(b.power == BOARD_PWR_BATTERY && b.pct >= 94 && b.pct < 100);
    uint32_t seq = log_next();
    watch_reset();
    g_usb = true;                           // the cable: packets, nothing into the cell
    int t = until_power(BOARD_PWR_FULL, 20000);
    CHECK(t >= 0 && t <= 15000);
    CHECK(a.chg_max == 0);
    b = st();
    CHECK(b.pct == 100 && b.full && !b.charging && b.ext == BOARD_EXT_USB && b.why == BOARD_BEV_NO_STEP);
    CHECK(logged(BOARD_BEV_USB, seq) && logged(BOARD_BEV_NO_STEP, seq));
    CHECK(b.vfull_learn == -1);             // not a plateau: nothing learned
    // it holds for half an hour (the load drains the cell a little)
    watch_reset();
    run(30 * 60000);
    CHECK(st().power == BOARD_PWR_FULL && a.changes == 0 && a.chg_max == 0);
    // macOS suspends the idle port: packets gone, no step: still on the charger
    g_usb = false;
    run(60000);
    b = st();
    CHECK(b.power == BOARD_PWR_FULL && b.ext == BOARD_EXT_KEPT);
}

// The user's report with this unit's numbers: 4083 mV at rest (v2 showed 87 %, then 80 % "charging"), the LED green.
// A PC port: the cell barely moves (+3 mV): FULL in seconds, no bolt. A MacBook: the charger runs for ~2 s and
// stops, then macOS suspends the port: FULL, and it stays on external power.
static void t_user_report(void)
{
    printf("t_user_report\n");
    boot(4083, -0.2, false);
    run(5 * 60000);
    CHECK(st().pct >= 90 && st().pct <= 93);    // the default v_full already reads it closer to full
    watch_reset();
    g_usb = true;
    charger(3, 0, 0, 0);
    int t = until_power(BOARD_PWR_FULL, 20000);
    CHECK(t >= 0 && t <= 15000 && a.chg_max == 0 && st().why == BOARD_BEV_NO_STEP);
    // the MacBook
    boot(4083, -0.2, false);
    run(5 * 60000);
    watch_reset();
    g_usb = true;
    charger(30, 0, 0, 0);
    run(2000);
    charger(0, 0, -0.2, 0);                 // the charger terminated: the cell is full
    run(30000);
    g_usb = false;                          // the idle CDC port suspended
    run(60000);
    board_bat_t b = st();
    CHECK(b.power == BOARD_PWR_FULL && b.ext == BOARD_EXT_KEPT && a.chg_max <= 4000);
    run(30 * 60000);
    CHECK(st().power == BOARD_PWR_FULL && st().ext == BOARD_EXT_KEPT);
}

// (b) ... the charger tops the cell up in its CV phase (a small step, then flat): CHARGING, then FULL by the plateau
// within ~5 min; the plateau's level is learned as v_full
static void t_usb_cv_plateau(void)
{
    printf("t_usb_cv_plateau\n");
    boot(4090, -0.2, false);
    run(5 * 60000);
    uint32_t seq = log_next();
    g_usb = true;
    charger(40, 10, 0.3, 4122);
    int t = until_power(BOARD_PWR_CHARGING, 15000);
    CHECK(t >= 0 && t <= 10000);
    CHECK(st().why == BOARD_BEV_STEP);
    t = until_power(BOARD_PWR_FULL, 8 * 60000);
    CHECK(t >= 0 && t <= 6 * 60000);        // flat over 4 min (the minute 4-5 min ago against the last one), 30 s
    board_bat_t b = st();
    CHECK(b.pct == 100 && logged(BOARD_BEV_PLATEAU, seq) && b.vfull_learn == -1);   // a pause is no end yet
    run(VFULL_AFTER_MS);                    // still flat: the end of the charge
    b = st();
    CHECK(b.power == BOARD_PWR_FULL && logged(BOARD_BEV_VFULL, seq));
    CHECK(b.vfull_learn >= 4120 && b.vfull_learn <= 4132 && b.vfull == b.vfull_learn);
    // the charger terminates (a 30 mV drop with the host there: under a step): still FULL for 20 min
    c.rest = 4100;
    charger(0, 0, -0.2, 0);
    watch_reset();
    run(20 * 60000);
    CHECK(st().power == BOARD_PWR_FULL && a.changes == 0);
    // a wobble of +-6 mV for half an hour: no flapping
    c.wave = 6;
    c.wave_min = 10;
    watch_reset();
    run(30 * 60000);
    CHECK(st().power == BOARD_PWR_FULL && a.changes == 0);
    c.wave = 0;
    // unplugged from the PC: the host goes, the cell barely moves (nothing flowed): the power is kept, as for a
    // suspended port, until the cell has drained 60 mV under its level (a known limit without a VBUS pin)
    g_usb = false;
    run(60000);
    CHECK(st().ext == BOARD_EXT_KEPT && st().power == BOARD_PWR_FULL);
    c.rate = -3;
    t = until_power(BOARD_PWR_BATTERY, 40 * 60000);
    CHECK(t > 0);
    b = st();
    CHECK(b.ext == BOARD_EXT_NONE && b.why == BOARD_BEV_FALL);
    // the learned v_full: a full cell at rest reads close to 100 %
    CHECK(pct_of(s_vfull - 30) >= 96 && pct_of(s_vfull) == 100);
}

// A slow charge near full (2 mV/min: a big cell on a PC port) is no plateau; the CV level is
static void t_slow_charge(void)
{
    printf("t_slow_charge\n");
    boot(4040, -0.2, false);
    run(5 * 60000);
    g_usb = true;
    charger(40, 10, 2, 4140);               // ~4090 rising 2 mV/min, held at 4140 from ~25 min
    int t = until_power(BOARD_PWR_CHARGING, 15000);
    CHECK(t >= 0 && t <= 10000);
    watch_reset();
    run(23 * 60000);
    CHECK(st().power == BOARD_PWR_CHARGING && a.changes == 0);
    t = until_power(BOARD_PWR_FULL, 15 * 60000);
    CHECK(t >= 0 && t <= 10 * 60000);
    run(VFULL_AFTER_MS);
    CHECK(s_vfull >= 4130 && s_vfull <= 4150);  // learned, or already within VFULL_STEP_MV of it
}

// (b) booted on USB with a full cell: no reference for a step, CHARGING by default, FULL by the plateau
static void t_usb_boot_full(void)
{
    printf("t_usb_boot_full\n");
    boot(4110, 0, true);
    board_bat_t b = st();
    CHECK(b.power == BOARD_PWR_CHARGING && b.ext == BOARD_EXT_USB && b.why == BOARD_BEV_NO_REF);
    int t = until_power(BOARD_PWR_FULL, 9 * 60000);
    CHECK(t >= 0 && t <= 7 * 60000);        // a minute to settle, then as above
}

// (c) a MacBook: the host's packets stop (macOS suspends the port) without a voltage step: still on external power;
// then the cable is pulled: the step down, BATTERY
static void t_macbook(void)
{
    printf("t_macbook\n");
    boot(3870, -0.3, false);
    run(5 * 60000);
    g_usb = true;
    charger(70, 25, 2, 0);
    int t = until_power(BOARD_PWR_CHARGING, 15000);
    CHECK(t >= 0 && t <= 10000);
    run(2 * 60000);
    uint32_t seq = log_next();
    watch_reset();
    g_usb = false;
    run(60000);
    board_bat_t b = st();
    CHECK(b.power == BOARD_PWR_CHARGING && b.ext == BOARD_EXT_KEPT && !b.usb);
    CHECK(logged(BOARD_BEV_USB_LOST_NO_STEP, seq));
    run(20 * 60000);
    b = st();
    CHECK(b.power == BOARD_PWR_CHARGING && b.ext == BOARD_EXT_KEPT && a.changes == 0 && a.pct_min >= 0);
    // back (a terminal opened the port), and suspended again
    g_usb = true;
    run(10000);
    CHECK(st().ext == BOARD_EXT_USB && st().power == BOARD_PWR_CHARGING);
    g_usb = false;
    run(2 * 60000);
    CHECK(st().ext == BOARD_EXT_KEPT);
    int pct1 = st().pct;
    seq = log_next();
    charger(0, 0, -0.3, 0);
    t = until_power(BOARD_PWR_BATTERY, 15000);
    CHECK(t >= 0 && t <= 10000);
    CHECK(logged(BOARD_BEV_STEP_DOWN, seq) && iabs(st().pct - pct1) <= 2);
    // the packets stopped while the speaker played (no settled reading to look for a step in): kept, unverified
    boot(3870, -0.3, false);
    run(5 * 60000);
    g_usb = true;
    charger(70, 25, 2, 0);
    run(3 * 60000);
    seq = log_next();
    speaker(30000, 120);
    g_usb = false;                          // unplugged during it: the step is hidden under the load
    charger(0, 0, -0.3, 0);
    run(20000);
    CHECK(logged(BOARD_BEV_USB_LOST, seq) && st().ext == BOARD_EXT_KEPT);
    // once the readings settle the level is ~95 mV under the charging one: the fall ends the external power
    t = until_power(BOARD_PWR_BATTERY, 4 * 60000);
    CHECK(t >= 0 && logged(BOARD_BEV_FALL, seq));
}

// (e) loud audio on a weak port: the cell drains under the speaker (readings skipped), USB_DRAIN shortly after it
// ends, the percent never up meanwhile; back to CHARGING once the port catches up
static void t_weak_port_drain(void)
{
    printf("t_weak_port_drain\n");
    boot(3900, -0.3, false);
    run(5 * 60000);
    g_usb = true;
    charger(30, 10, 0.5, 0);
    int t = until_power(BOARD_PWR_CHARGING, 15000);
    CHECK(t >= 0 && t <= 10000);
    run(6 * 60000);
    // 4 min of loud speech: the load takes more than the port gives
    uint32_t seq = log_next();
    speaker(4 * 60000, 150);
    charger(0, -20, -15, 0);
    watch_reset();
    run(4 * 60000);
    CHECK(st().power == BOARD_PWR_CHARGING && a.changes == 0);   // nothing measured under the speaker
    charger(30, 10, 0.5, 0);
    t = until_power(BOARD_PWR_USB_DRAIN, 4 * 60000);
    CHECK(t >= 0 && t <= 3 * 60000);
    board_bat_t b = st();
    CHECK(b.why == BOARD_BEV_DRAIN && logged(BOARD_BEV_DRAIN, seq) && b.ext == BOARD_EXT_USB);
    watch_reset();
    int pct_d = b.pct;
    t = until_power(BOARD_PWR_CHARGING, 40 * 60000);
    CHECK(t >= 2 * 60000);                  // the minimum dwell, then the port's slow rise
    CHECK(a.drain_pct_max == pct_d);        // never up while draining
    CHECK(st().why == BOARD_BEV_DRAIN_END);
    // ... near full too (this unit's case: a PC port, the speaker at volume 100): the fall is across the speaker, so
    // it is the port, not the charger stopping (FULL)
    boot(4060, -0.2, false);
    run(5 * 60000);
    g_usb = true;
    charger(30, 10, 0.3, 0);
    CHECK(until_power(BOARD_PWR_CHARGING, 15000) >= 0);
    run(6 * 60000);
    speaker(4 * 60000, 150);
    charger(0, -20, -15, 0);
    run(4 * 60000);
    charger(30, 10, 0.3, 0);
    watch_reset();
    t = until_power(BOARD_PWR_USB_DRAIN, 4 * 60000);
    CHECK(t >= 0 && a.full_n == 0 && st().why == BOARD_BEV_DRAIN && st().mv >= full_floor());
}

// A wall charger from 75 % to its CV level: FULL by the plateau, v_full learned; the charger terminates (the cell
// relaxes 25 mV): still FULL; a new charge (the recharge level) restarts CHARGING
static void t_wall_full(void)
{
    printf("t_wall_full\n");
    boot(4000, -0.3, false);
    run(5 * 60000);
    charger(80, 20, 4, 4140);
    int t = until_power(BOARD_PWR_CHARGING, 15000);
    CHECK(t >= 0 && t <= 10000);
    uint32_t seq = log_next();
    t = until_power(BOARD_PWR_FULL, 25 * 60000);
    CHECK(t > 0);
    board_bat_t b = st();
    CHECK(b.ext == BOARD_EXT_CHARGER && b.pct == 100 && logged(BOARD_BEV_PLATEAU, seq));
    c.rest = 4115;
    charger(0, 0, -0.3, 0);
    watch_reset();
    run(20 * 60000);
    CHECK(st().power == BOARD_PWR_FULL && a.changes == 0 && st().ext == BOARD_EXT_CHARGER);
    CHECK(st().vfull_learn >= 4130 && st().vfull_learn <= 4150);    // the plateau's level, not the relaxed one
    // the charger starts again at its recharge level: a step up
    seq = log_next();
    charger(50, 10, 1, 4150);
    t = until_power(BOARD_PWR_CHARGING, 15000);
    CHECK(t >= 0 && t <= 10000 && logged(BOARD_BEV_STEP_UP, seq));
    CHECK(st().pct == 100);                 // a top-up: the percent stays
}

// Readings that are not a charger: a spike, a load the app does not grade (a step down and back), noise
static void t_not_a_charger(void)
{
    printf("t_not_a_charger\n");
    boot(3900, -0.3, false);
    run(5 * 60000);
    watch_reset();
    uint32_t seq = log_next();
    c.spike = 60;
    c.spike_reads = 8;                      // one reading 60 mV high
    run(5100);
    CHECK(st().poll_ms == 2000);            // measured faster
    run(30000);
    CHECK(st().poll_ms == 5000 && st().ext == BOARD_EXT_NONE && !logged(BOARD_BEV_STEP_UP, seq));
    // an ungraded load (OTA, the portal's radio): 45 mV down for 3 min, then back
    c.sag = 45;
    run(3 * 60000);
    CHECK(logged(BOARD_BEV_STEP_DOWN, seq) && st().power == BOARD_PWR_BATTERY);
    c.sag = 0;
    run(60000);
    board_bat_t b = st();
    CHECK(logged(BOARD_BEV_LOAD_END, seq) && b.ext == BOARD_EXT_NONE && b.power == BOARD_PWR_BATTERY);
    CHECK(a.chg_max == 0);
    // an hour of noise twice the usual: nothing
    c.noise = 12;
    watch_reset();
    run(60 * 60000);
    CHECK(a.changes == 0 && st().ext == BOARD_EXT_NONE);
}

// The battery log: a record a minute (-1 for a minute without a settled reading), events, the ring's limits, clear
static void t_log(void)
{
    printf("t_log\n");
    boot(3900, -0.2, false);
    uint32_t first, next = board_battery_log_range(&first);
    CHECK(next - first == 1);               // the boot event
    board_bat_log_t r;
    CHECK(board_battery_log_get(first, &r) && r.ev == BOARD_BEV_BOOT && !board_battery_log_get(next, &r));
    run(10 * 60000 + 500);
    next = board_battery_log_range(&first);
    int minutes = 0, none = 0;
    for (uint32_t i = first; i < next; i++) {
        if (board_battery_log_get(i, &r) && r.ev == BOARD_BEV_MINUTE) {
            minutes++;
            none += r.mv < 0;
            CHECK(r.power == BOARD_PWR_BATTERY && r.usb == 0 && r.ext == BOARD_EXT_NONE && r.d > 3800);
        }
    }
    CHECK(minutes == 10 && none <= 1);      // the first minute: booting, hardly a settled reading
    // a minute all under the speaker: no settled level in it
    speaker(3 * 60000, 100);
    run(3 * 60000);
    CHECK(board_battery_log_get(board_battery_log_range(NULL) - 1, &r) && r.ev == BOARD_BEV_MINUTE && r.mv == -1);
    // names
    for (int e = 0; e < BOARD_BEV__N; e++) {
        CHECK(strcmp(board_bat_ev_name((board_bat_ev_t)e), "?") != 0);
    }
    CHECK(!strcmp(board_ext_name(BOARD_EXT_KEPT), "kept") && !strcmp(board_bat_ev_name(BOARD_BEV_LOAD_END), "load_end"));
    // v4's reasons come after v3's (the records' ev numbers keep their meaning), the record's size is the same
    CHECK(BOARD_BEV_LOAD_END == 22 && BOARD_BEV_STEP_BASE == 23 && BOARD_BEV_TREND_SLOW == 24 &&
          BOARD_BEV_NO_RISE == 25 && sizeof(r) == 16);
    CHECK(!strcmp(board_bat_ev_name(BOARD_BEV_STEP_BASE), "step_base") &&
          !strcmp(board_bat_ev_name(BOARD_BEV_TREND_SLOW), "trend_slow") &&
          !strcmp(board_bat_ev_name(BOARD_BEV_NO_RISE), "no_rise"));
    // 27 h: the ring keeps the last BOARD_BAT_LOG_N records
    for (int i = 0; i < 27 * 60; i++) {
        run(60000);
    }
    next = board_battery_log_range(&first);
    CHECK(next - first == BOARD_BAT_LOG_N && board_battery_log_get(first, &r) && !board_battery_log_get(first - 1, &r));
    board_battery_log_clear();
    next = board_battery_log_range(&first);
    CHECK(next == first && !board_battery_log_get(next - 1, &r));
    run(61000);
    CHECK(board_battery_log_range(&first) - first == 1);
}

// v_full: the curve's scale, the setting's range
static void t_vfull(void)
{
    printf("t_vfull\n");
    boot(4000, 0, false);
    board_battery_set_vfull(4086);
    CHECK(s_vfull == 4086 && st().vfull == 4086);
    CHECK(pct_of(4086) == 100 && pct_of(4056) >= 96 && pct_of(4056) <= 98);
    board_battery_set_vfull(3000);
    CHECK(s_vfull == VFULL_DEF_MV);
    board_battery_set_vfull(4300);
    CHECK(s_vfull == VFULL_DEF_MV);
    CHECK(pct_of(4150) == 100 && pct_of(2700) == -1);
    // the user's PC case: 4083 mV on battery read 87 % unscaled; with the default v_full 92 %
    CHECK(board_battery_pct(4083) == 85 && pct_of(4083) >= 91 && pct_of(4083) <= 93);
}

// No cell: a charger's output swinging on its own reports no reading
static void t_nobat(void)
{
    printf("t_nobat\n");
    boot(4000, 0, true);
    for (int i = 0; i < 60; i++) {
        c.rest = i % 2 ? 4300 : 3900;
        run(5000);
    }
    CHECK(st().pct == -1);
}

// ---- review: the cases the first version got wrong

// The end of a charge on a PC port: the charger terminates in its CV phase, the load moves onto the cell (a drop at
// once, under or over a step) and the polarisation relaxes over minutes: FULL holds at 100 %, no USB_DRAIN ("USB: не
// хватает тока"), the full glow once
static void t_end_of_charge(void)
{
    printf("t_end_of_charge\n");
    static const struct {
        double now, relax;
    } k[] = { { 20, 30 }, { 30, 25 }, { 10, 45 }, { 40, 30 } };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        boot(4060, -0.2, false);
        run(5 * 60000);
        g_usb = true;
        charger(40, 10, 0.5, 4122);
        watch_reset();
        CHECK(until_power(BOARD_PWR_FULL, 12 * 60000) >= 0);
        run(10 * 60000);                    // still in CV
        c.rest = 4122 - k[i].now - k[i].relax;
        c.ohm = c.cv = 0;
        c.rate = -0.3;
        c.pol = k[i].relax;
        c.pol_to = 0;
        if (i == 3) {
            turn(15000);                    // a short turn just then: no session that drained it
        }
        run(30 * 60000);
        board_bat_t b = st();
        CHECK(b.power == BOARD_PWR_FULL && b.pct == 100 && a.full_n == 1 && a.changes == 2);
    }
}

// Wall chargers that must be seen even though the cell was higher a while ago (why a step up is compared only with a
// step down seen just before it, not with older levels): a near-full cell drained idle (-0.8 mV/min) for 20 min, then
// a charger in its CV phase (a small step, 20 mV: the trend); after a 10-min loud session (the cell 35 mV lower), a
// weak charger (40 mV); shortly after a turn
static void t_charger_seen(void)
{
    printf("t_charger_seen\n");
    boot(4100, -0.8, false);
    run(20 * 60000);
    charger(20, 0, 0.2, 0);
    int t = until_power(BOARD_PWR_CHARGING, 4 * 60000);
    CHECK(t >= 0 && st().ext == BOARD_EXT_CHARGER);
    boot(3950, -0.3, false);
    run(10 * 60000);
    speaker(10 * 60000, 120);
    c.rate = -3.5;
    run(10 * 60000);
    c.rate = -0.3;
    run(3 * 60000);
    charger(40, 10, 1.5, 0);
    t = until_power(BOARD_PWR_CHARGING, 15000);
    CHECK(t >= 0 && t <= 10000 && st().ext == BOARD_EXT_CHARGER);
    boot(3900, -0.3, false);
    run(6 * 60000);
    turn(60000);
    run(2 * 60000 + 30000);
    charger(40, 10, 1.5, 0);                // a weak one: a 40 mV step
    t = until_power(BOARD_PWR_CHARGING, 15000);
    CHECK(t >= 0 && t <= 10000 && st().ext == BOARD_EXT_CHARGER);
}

// Leaving FULL the percent glides down to the curve (1 point at a time) instead of jumping: a FULL taken at the
// plug-in at 4.04 V (the charger did not start), the host gone, the cell drained 60 mV: BATTERY
static void t_full_glide(void)
{
    printf("t_full_glide\n");
    boot(4040, -0.2, false);
    run(5 * 60000);
    g_usb = true;
    CHECK(until_power(BOARD_PWR_FULL, 20000) >= 0 && st().pct == 100);
    g_usb = false;
    run(60000);
    CHECK(st().ext == BOARD_EXT_KEPT);
    c.rate = -3;
    watch_reset();
    int t = until_power(BOARD_PWR_BATTERY, 40 * 60000);
    CHECK(t > 0);
    run(15 * 60000);
    board_bat_t b = st();
    CHECK(a.drop_max <= 1 && b.pct == pct_of(b.mv));     // there now, and no jump on the way
}

// v_full is learned only from a CV plateau that lasts: not from a port that only holds the cell (flat 15 min at
// 4.02 V: FULL as the spec says, nothing learned), at most VFULL_DOWN_MV lower per charge (a plateau that was not the
// CV level cannot pull the scale far), and back up from a FULL that read higher. A slow charge near full (1.2 mV/min,
// under the plateau's 5 mV in 4 min) shows FULL early (the spec's rule) and stays FULL while it tops up: no flapping;
// the CV level it reaches restores v_full when it ends
static void t_vfull_guard(void)
{
    printf("t_vfull_guard\n");
    boot(4020, -0.2, false);
    run(5 * 60000);
    g_usb = true;
    charger(15, 0, 0, 0);                   // a weak port: as much in as the load takes
    CHECK(until_power(BOARD_PWR_FULL, 25 * 60000) >= 0);
    run(VFULL_AFTER_MS + 60000);
    CHECK(st().vfull_learn == -1 && s_vfull == VFULL_DEF_MV);
    // a CV level 90 mV under the default: learned in steps
    boot(4000, -0.2, false);
    run(5 * 60000);
    g_usb = true;
    charger(40, 10, 1, 4060);
    CHECK(until_power(BOARD_PWR_FULL, 25 * 60000) >= 0);
    run(VFULL_AFTER_MS + 60000);
    CHECK(st().vfull_learn == VFULL_DEF_MV - VFULL_DOWN_MV && s_vfull == st().vfull_learn);
    // a slow charge into a CV level at the default
    boot(4060, -0.2, false);
    run(5 * 60000);
    g_usb = true;
    charger(40, 10, 1.2, 4150);
    CHECK(until_power(BOARD_PWR_CHARGING, 15000) >= 0);
    watch_reset();
    run(60 * 60000);
    CHECK(a.full_n <= 1 && a.changes <= 2 && s_vfull >= VFULL_DEF_MV - VFULL_DOWN_MV);
    g_usb = false;                          // unplugged at its CV level: a step down
    c.rest = 4100;
    charger(0, 0, -0.3, 0);
    CHECK(until_power(BOARD_PWR_BATTERY, 15000) >= 0);
    CHECK(s_vfull >= VFULL_DEF_MV - 5);
}

// The MacBook's other reading: the charger runs ~20 s on a nearly full cell and terminates (a 30 mV drop, under a
// step): CHARGING for a few minutes, then FULL as the level stays down, never USB_DRAIN; then the port suspends: kept
static void t_short_charge(void)
{
    printf("t_short_charge\n");
    boot(4083, -0.2, false);
    run(5 * 60000);
    g_usb = true;
    charger(30, 5, 0, 0);
    watch_reset();
    run(20000);
    charger(0, 0, -0.2, 0);
    int t = until_power(BOARD_PWR_FULL, 8 * 60000);
    CHECK(t >= 0 && a.drain_pct_max < 0 && st().pct == 100);
    g_usb = false;
    run(60000);
    CHECK(st().power == BOARD_PWR_FULL && st().ext == BOARD_EXT_KEPT);
}

// A charger that never takes the cell back up (a fault, its safety timer) with the host there: FULL from the plug-in
// (no step), the load drains the cell: FULL down to v_full - FULL_HOLD_MV, then USB_DRAIN, the percent gliding down
// to the curve of the uncharged cell (no charging offset: no current flows)
static void t_charger_idle(void)
{
    printf("t_charger_idle\n");
    boot(4083, -0.2, false);
    run(5 * 60000);
    g_usb = true;
    CHECK(until_power(BOARD_PWR_FULL, 20000) >= 0);
    c.rate = -1.5;
    watch_reset();
    int t = until_power(BOARD_PWR_USB_DRAIN, 90 * 60000);
    CHECK(t > 30 * 60000 && st().why == BOARD_BEV_FULL_EXIT);
    run(10 * 60000);
    board_bat_t b = st();
    CHECK(b.power == BOARD_PWR_USB_DRAIN && a.changes == 1 && a.drop_max <= 1);
    CHECK(iabs(b.pct - pct_of(b.mv)) <= 2);
}

// The charger's own cycle after its termination: the load drains the cell to the recharge level (~120 mV under the
// CV level), the charger starts again (a step up), CV, termination: FULL - CHARGING (a top-up) - FULL, 100 % all
// along, never USB_DRAIN
static void t_recharge_cycle(void)
{
    printf("t_recharge_cycle\n");
    boot(4060, -0.2, false);
    run(5 * 60000);
    g_usb = true;
    charger(40, 10, 0.5, 4122);
    CHECK(until_power(BOARD_PWR_FULL, 12 * 60000) >= 0);
    run(VFULL_AFTER_MS);
    watch_reset();
    uint32_t seq = log_next();
    for (int i = 0; i < 2; i++) {
        c.rest = 4122 - 20 - 30;            // terminated: the load on the cell, the polarisation relaxing
        c.ohm = c.cv = 0;
        c.pol = 30;
        c.pol_to = 0;
        c.rate = -1.5;
        while (st().mv > 4122 - 120) {
            run(5000);
        }
        charger(100, 20, 2, 4122);          // the recharge: the full current, at its CV level at once
        CHECK(until_power(BOARD_PWR_CHARGING, 15000) >= 0);
        CHECK(until_power(BOARD_PWR_FULL, 15 * 60000) >= 0);
        run(5 * 60000);
    }
    CHECK(a.pct_min == 100 && a.drain_pct_max < 0 && a.full_n == 2 && a.changes == 4);
    CHECK(s_vfull >= 4115 && s_vfull <= 4132);  // learned once ...
    CHECK(!logged(BOARD_BEV_VFULL, seq));       // ... and no further NVS write for the same level
}

// The host's packets flickering (a hub, a waking MacBook) every 3 s for 20 min: the state holds and the log is not
// flooded with "usb" events
static void t_usb_flicker(void)
{
    printf("t_usb_flicker\n");
    boot(3900, -0.3, false);
    run(5 * 60000);
    g_usb = true;
    charger(60, 20, 2, 0);
    CHECK(until_power(BOARD_PWR_CHARGING, 15000) >= 0);
    run(60000);
    uint32_t seq = log_next();
    watch_reset();
    for (int i = 0; i < 400; i++) {
        g_usb = !g_usb;
        run(3000);
    }
    g_usb = true;
    run(10000);
    CHECK(a.changes == 0 && st().power == BOARD_PWR_CHARGING);
    CHECK(log_next() - seq <= 40);          // the minute records and a few events
}

// ---- v4: plug-ins the way they really happen (the field report: the percent rose, no bolt)

// Was external power seen through any of the voltage paths since seq?
static bool ext_logged(uint32_t since)
{
    return logged(BOARD_BEV_STEP_UP, since) || logged(BOARD_BEV_STEP_BASE, since) ||
           logged(BOARD_BEV_TREND_UP, since) || logged(BOARD_BEV_TREND_SLOW, since);
}

// The first of the events es[n] among the log's records since seq (BOARD_BEV__N = none)
static board_bat_ev_t first_of(uint32_t since, const board_bat_ev_t *es, int n)
{
    uint32_t first, next = board_battery_log_range(&first);
    board_bat_log_t r;
    for (uint32_t i = since > first ? since : first; i < next; i++) {
        for (int k = 0; k < n && board_battery_log_get(i, &r); k++) {
            if (r.ev == es[k]) {
                return es[k];
            }
        }
    }
    return BOARD_BEV__N;
}

static const board_bat_ev_t k_ext_on[] = { BOARD_BEV_STEP_UP, BOARD_BEV_STEP_BASE, BOARD_BEV_TREND_UP,
                                           BOARD_BEV_TREND_SLOW };
static const board_bat_ev_t k_ext_off[] = { BOARD_BEV_STEP_DOWN, BOARD_BEV_FALL, BOARD_BEV_NO_RISE };

// The field log (v0.7-rc2, field_log_plugin.jsonl): v_full 4122, 4002 mV at boot (84 %), 3993 four minutes later;
// a wall charger plugged in about then (+108 mV a minute later, 4113 rising 3 mV/min), the screen woken with it:
// v3 never saw a step (no settled reading across the plug-in) nor a trend (3 mV/min is under 15 mV in 2 min), and
// let the percent rise 83 -> 98 on a calm reading without the bolt
static void t_field_report(void)
{
    printf("t_field_report\n");
    boot(4002, -1.5, false);
    board_battery_set_vfull(4122);
    g_bl = 80;                              // on at boot, dimmed after a minute, off 2 min later
    run(60000);
    screen(25);
    run(2 * 60000);
    screen(0);
    run(90000);
    board_bat_t b = st();
    CHECK(b.power == BOARD_PWR_BATTERY && b.pct >= 82 && b.pct <= 85);
    int pct0 = b.pct;
    uint32_t seq = log_next();
    watch_reset();
    a.mono = true;
    charger_soft(75, 30, 3, 0, 5);          // plugged in, and the screen woken to look at it
    run(1000);
    screen(80);
    int t = until_power(BOARD_PWR_CHARGING, 4 * 60000);
    CHECK(t >= 0 && t <= 2 * 60000 + 30000);    // the first settled readings after the wake (60 s) and 2 more
    b = st();
    CHECK(b.ext == BOARD_EXT_CHARGER && logged(BOARD_BEV_STEP_BASE, seq));
    CHECK(a.bat_rise == 0 && iabs(b.pct - pct0) <= 2);
    CHECK(b.ofs >= 80 && b.ofs <= 120);     // measured against the level before the plug-in
    run(10 * 60000);
    b = st();
    CHECK(b.power == BOARD_PWR_CHARGING && b.pct >= pct0);
    int pct1 = b.pct;
    charger(0, 0, -1, 0);
    CHECK(until_power(BOARD_PWR_BATTERY, 15000) >= 0 && iabs(st().pct - pct1) <= 2);
}

// A soft start: the charge current ramps in over 10-90 s (the reading rises over several readings, each step under
// 35 mV): the rise against the level before it. (v4 review: v3 sees these too, by its run or its trend; a regression
// test for v4's guards. The soft start that v3 missed came with the screen woken: t_field_report, t_plug_busy)
static void t_plug_soft_start(void)
{
    printf("t_plug_soft_start\n");
    static const double ramp[] = { 10, 30, 60, 90 };
    for (size_t i = 0; i < sizeof(ramp) / sizeof(ramp[0]); i++) {
        boot(3850, -0.3, false);
        run(5 * 60000);
        int pct0 = st().pct;
        watch_reset();
        a.mono = true;
        charger_soft(60, 20, 2, 0, ramp[i]);
        int t = until_power(BOARD_PWR_CHARGING, 4 * 60000);
        CHECK(t >= 0 && t <= (int)(ramp[i] * 1000) + 60000);
        CHECK(st().ext == BOARD_EXT_CHARGER && a.bat_rise == 0 && iabs(st().pct - pct0) <= 1);
        // the offset: the whole rise (60 + 20), not the part after the run or the trend caught it
        run(5 * 60000);
        CHECK(st().ofs >= 65 && st().ofs <= 95);
        // and unplugged again: the step down
        charger(0, 0, -0.3, 0);
        CHECK(until_power(BOARD_PWR_BATTERY, 15000) >= 0);
    }
}

// The rise split across two readings 5 s apart (25 + 25 mV), and the polarisation after it (v3 sees it too: a
// regression test)
static void t_plug_split(void)
{
    printf("t_plug_split\n");
    boot(3880, -0.3, false);
    run(5 * 60000);
    watch_reset();
    a.mono = true;
    charger(25, 0, 2, 0);
    run(5000);
    charger(50, 15, 2, 0);
    int t = until_power(BOARD_PWR_CHARGING, 3 * 60000);
    CHECK(t >= 0 && t <= 60000 && a.bat_rise == 0);
}

// Plugged in while the readings are not settled: during a turn, during a spoken answer (the cell recovering after
// it), with the screen woken just after, while the screen dims
static void t_plug_busy(void)
{
    printf("t_plug_busy\n");
    for (int k = 0; k < 4; k++) {
        boot(3870, -0.3, false);
        c.bl_mv = 0.25;                     // the backlight at 80 % reads 20 mV lower
        g_bl = 80;
        run(6 * 60000);
        if (k != 3) {
            screen(0);
            run(4 * 60000);
        }
        int pct0 = st().pct;
        uint32_t seq = log_next();
        watch_reset();
        a.mono = true;
        uint32_t t0;
        switch (k) {
        case 0:                             // during a turn
            turn(15000);
            run(5000);
            charger_soft(55, 20, 2, 0, 20);
            run(10000);
            break;
        case 1:                             // during a spoken answer, which leaves the cell low for a while
            speaker_d(40000, 100, 15, 4);
            run(10000);
            charger_soft(55, 20, 2, 0, 20);
            run(30000);
            break;
        case 2:                             // screen asleep, woken 3 s after, dimmed a minute later
            charger_soft(50, 15, 2, 0, 10);
            run(3000);
            screen(80);
            run(60000);
            screen(25);
            break;
        default:                            // the screen dims just as it is plugged in
            charger_soft(50, 15, 2, 0, 10);
            screen(25);
            break;
        }
        t0 = now_ms();
        int t = until_power(BOARD_PWR_CHARGING, 6 * 60000);
        CHECK(t >= 0 && t <= 3 * 60000);
        if (t < 0 || t > 3 * 60000) {
            printf("    case %d: %d ms after %u\n", k, t, (unsigned)t0);
        }
        CHECK(ext_logged(seq) && a.bat_rise == 0 && st().pct >= pct0 - 1);
    }
}

// Wi-Fi bursts (a reading in three up to 20 mV low) and +-6 mV per reading: no charger for an hour, then a 45 mV
// soft-started step is seen
static void t_plug_noisy(void)
{
    printf("t_plug_noisy\n");
    boot(3900, -0.3, false);
    c.jitter = 6;
    c.burst_p = 0.33;
    c.burst = 20;
    run(5 * 60000);
    uint32_t seq = log_next();
    watch_reset();
    a.mono = true;
    run(60 * 60000);
    CHECK(!ext_logged(seq) && a.chg_max == 0 && a.bat_rise == 0);
    charger_soft(45, 15, 2, 0, 15);
    int t = until_power(BOARD_PWR_CHARGING, 5 * 60000);
    CHECK(t >= 0 && t <= 2 * 60000);
}

// A modest charger on a low-IR cell: 20 mV at once, 10 more within a minute, 1.5 mV/min: no step, seen by a trend;
// the percent never up on battery meanwhile. Near full: into a CV level 40 mV up (seen), 22 mV up (seen by the
// trend), and 10 mV up (flat at once, as on battery: the percent must not rise). v3 sees the first three too (its
// trend): a regression test
static void t_plug_modest(void)
{
    printf("t_plug_modest\n");
    boot(3900, -0.3, false);
    run(5 * 60000);
    uint32_t seq = log_next();
    watch_reset();
    a.mono = true;
    charger(20, 10, 1.5, 0);
    int t = until_power(BOARD_PWR_CHARGING, 30 * 60000);
    CHECK(t >= 0 && t <= 20 * 60000 && a.bat_rise == 0);
    CHECK(logged(BOARD_BEV_TREND_SLOW, seq) || logged(BOARD_BEV_TREND_UP, seq) || logged(BOARD_BEV_STEP_BASE, seq));
    charger(0, 0, -1, 0);
    CHECK(until_power(BOARD_PWR_BATTERY, 20 * 60000) >= 0);
    // near full, a CV level 40 mV above the cell
    boot(4085, -0.3, false);
    run(5 * 60000);
    watch_reset();
    a.mono = true;
    charger_soft(60, 10, 0.5, 4125, 20);
    t = until_power(BOARD_PWR_CHARGING, 4 * 60000);
    CHECK(t >= 0 && t <= 2 * 60000 && a.bat_rise == 0);
    // 22 mV on a calm cell: the trend
    boot(4100, -0.3, false);
    run(5 * 60000);
    watch_reset();
    a.mono = true;
    charger_soft(60, 10, 0.5, 4122, 20);
    t = until_power(BOARD_PWR_CHARGING, 4 * 60000);
    CHECK(t >= 0 && t <= 3 * 60000 && a.bat_rise == 0);
    // 10 mV: not seen (flat at once, as a full cell on battery), and the percent stays; or (v4 review: on some noise
    // seeds the 10 mV clear the trend) seen, which is no false charger: it is plugged in
    boot(4112, -0.3, false);
    run(5 * 60000);
    seq = log_next();
    watch_reset();
    a.mono = true;
    int pct0 = st().pct;
    charger_soft(60, 10, 0.5, 4122, 20);
    run(30 * 60000);
    CHECK(a.bat_rise == 0 && (st().power == BOARD_PWR_BATTERY ? a.pct_max <= pct0 : ext_logged(seq)));
}

// Booted on a wall charger (no reference, no host): the screen dims and goes off, the CC rise (1.5 and 3 mV/min) is
// seen by the slow trend; the percent never rises after the first settled level; unplugged: the step down
static void t_boot_on_charger(void)
{
    printf("t_boot_on_charger\n");
    static const double rate[] = { 1.5, 3 };
    for (size_t i = 0; i < 2; i++) {
        boot_chg(3850, rate[i], false, 60, 20);
        g_bl = 80;
        run(60000);
        screen(25);
        run(2 * 60000);
        screen(0);
        watch_reset();
        a.mono = true;
        int t = until_power(BOARD_PWR_CHARGING, 30 * 60000);
        CHECK(t >= 0 && t <= (i ? 12 : 20) * 60000 && a.bat_rise == 0);
        CHECK(st().ext == BOARD_EXT_CHARGER && st().why == BOARD_BEV_TREND_SLOW);
        run(5 * 60000);
        int pct1 = st().pct;
        charger(0, 0, -0.3, 0);
        CHECK(until_power(BOARD_PWR_BATTERY, 15000) >= 0 && iabs(st().pct - pct1) <= 2);
    }
}

static uint32_t s_r;
static int rnd(int lo, int hi)
{
    s_r = s_r * 1103515245u + 12345u;
    return lo + (int)((s_r >> 8) % (uint32_t)(hi - lo + 1));
}

// Real use for ms: idle gaps (mostly 20 s - 7 min, one in five 10-25 min; the screen dims a minute after the last
// touch and goes off 2 min later), touches, turns with spoken answers (the cell sags under them and recovers for
// minutes after: c.dep, c.dep2), sessions of 3-8 turns, Wi-Fi reconnects; the cell drifts by idle_rate mV/min, by
// busy during a turn
static void real_use(uint32_t ms, double idle_rate, double busy)
{
    uint32_t end = now_ms() + ms, act = now_ms();
    c.rate = idle_rate;
    while ((int32_t)(end - now_ms()) > 0) {
        int idle = rnd(0, 4) ? rnd(20, 420) : rnd(600, 1500);
        for (int s = 0; s < idle && (int32_t)(end - now_ms()) > 0; s++) {
            uint32_t q = now_ms() - act;
            if (q >= 180000 && g_bl == 25) {
                screen(0);
            } else if (q >= 60000 && g_bl == 80) {
                screen(25);
            }
            run(1000);
        }
        act = now_ms();
        screen(80);
        int k = rnd(0, 9), n = k == 9 ? rnd(3, 8) : k >= 3 ? 1 : 0;
        if (k == 0) {
            a.load_t = now_ms();            // Wi-Fi joined again
        }
        for (int i = 0; i < n; i++) {
            int tl = rnd(3, 15), al = rnd(5, 60);
            c.rate = busy;
            turn((uint32_t)tl * 1000);
            c.sag = 40;
            run((uint32_t)tl * 1000);
            speaker_d((uint32_t)al * 1000, rnd(60, 150), al * 0.4, al * 0.15);
            run((uint32_t)(al + rnd(1, 10)) * 1000);
            c.rate = idle_rate;
        }
        act = now_ms();
    }
}

// Two hours of real use at five levels, with Wi-Fi bursts and jitter: never a charger, the percent never up after
// the first settled level; then plugged in at a random moment while it is used on: seen, and not lost again. v4
// review: over REAL_SEEDS noise and schedule seeds (the first v4 held for its one seed only); a cell that reaches
// FULL by v3's 15-min plateau while charging in use counts as seen (that rule is not this test's)
#define REAL_SEEDS 6
static void t_real_use(void)
{
    printf("t_real_use\n");
    static const struct {
        double mv, drain;
        uint32_t seed;
    } k[] = { { 3760, -0.4, 1 }, { 3880, -0.3, 2 }, { 4060, -0.6, 3 }, { 3950, -0.2, 4 }, { 3820, -0.5, 5 } };
    int false_n = 0, rise_n = 0, missed = 0, lost_n = 0;
    for (uint32_t sd = 0; sd < REAL_SEEDS; sd++) {
        for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
            boot(k[i].mv, k[i].drain, false);
            c.rng = 12345 + (s_seed_n + 1000 * sd) * 104729u;
            c.jitter = 6;
            c.burst_p = 0.25;
            c.burst = 20;
            c.bl_mv = 0.25;
            g_bl = 80;
            s_r = k[i].seed + (s_seed_r + sd) * 1000;
            run(4 * 60000);
            uint32_t seq = log_next();
            watch_reset();
            a.mono = true;
            real_use(2 * 3600000, k[i].drain, k[i].drain * 4);
            bool f = ext_logged(seq) || a.chg_max > 0 || st().ext != BOARD_EXT_NONE;
            false_n += f;
            rise_n += a.bat_rise > 0;
            if (f || a.bat_rise) {
                printf("    seed %u case %d: %s, charging %d s, rise %d\n", (unsigned)sd, (int)i,
                       board_bat_ev_name(first_of(seq, k_ext_on, 4)), a.chg_max / 1000, a.bat_rise);
            }
            real_use((uint32_t)rnd(1, 600) * 1000, k[i].drain, k[i].drain * 4);
            seq = log_next();
            charger_soft(70, 20, 2, 0, 20);
            uint32_t t0 = now_ms();
            real_use(45 * 60000, 2, 0.5);
            board_bat_ev_t off = first_of(seq, k_ext_off, 3);
            bool seen = ext_logged(seq) && st().ext != BOARD_EXT_NONE && st().power != BOARD_PWR_BATTERY;
            missed += !seen;
            lost_n += off != BOARD_BEV__N;
            rise_n += a.bat_rise > 0;
            if (!seen || off != BOARD_BEV__N) {
                printf("    seed %u case %d: plugged in at %u s: %s after 45 min%s%s\n", (unsigned)sd, (int)i,
                       (unsigned)t0 / 1000, board_power_name(st().power), off != BOARD_BEV__N ? ", lost: " : "",
                       off != BOARD_BEV__N ? board_bat_ev_name(off) : "");
            }
        }
    }
    CHECK(false_n == 0 && rise_n == 0);
    CHECK(missed == 0 && lost_n == 0);
}

// Run by name only (x_stress): 5 seeds x 40 x 2 h of real use, harsher than t_real_use (Wi-Fi bursts in two readings
// of five up to 25 mV, +-10 mV per reading, the backlight 24 mV at 80 %): the percent never up; the false chargers,
// the plug-ins not seen in 45 min of use and those lost again (a noise step down or fall while charging) counted.
// v4 review: over seeds (the first v4's "0 in 80 h" held for its one seed: 421 false in 4000 runs over 10 x 10 seeds,
// now 20; -n / -r move the seeds)
static void x_stress(void)
{
    printf("x_stress\n");
    int false_n = 0, rise_n = 0, missed = 0, unplugs = 0, runs = 0;
    for (uint32_t n = 0; n < 200; n++) {
        uint32_t i = n % 40;
        runs++;
        if (!i) {
            s_seed_n += n ? 1 : 0;
            s_seed_r += n ? 1 : 0;
        }
        double mv = 3720 + (double)(i * 37 % 380), drain = -0.2 - (double)(i % 5) * 0.15;
        boot(mv, drain, false);
        c.jitter = 10;
        c.burst_p = 0.4;
        c.burst = 25;
        c.bl_mv = 0.3;
        g_bl = 80;
        s_r = 1000 + i + s_seed_r * 1000;
        run(4 * 60000);
        uint32_t seq = log_next();
        watch_reset();
        a.mono = true;
        real_use(2 * 3600000, drain, drain * 4);
        false_n += ext_logged(seq) || a.chg_max > 0;
        rise_n += a.bat_rise > 0;
        if (ext_logged(seq) || a.bat_rise) {
            printf("    run %u at %.0f mV: %s, charging %d s, rise %d\n", (unsigned)i, mv,
                   board_bat_ev_name(first_of(seq, k_ext_on, 4)), a.chg_max / 1000, a.bat_rise);
        }
        seq = log_next();
        charger_soft(70, 20, 2, 0, 20);
        uint32_t t0 = now_ms();
        real_use(45 * 60000, 2, 0.5);
        board_bat_ev_t off = first_of(seq, k_ext_off, 3);
        bool lost = off != BOARD_BEV__N;
        if (!ext_logged(seq) || lost || st().ext == BOARD_EXT_NONE) {
            missed += !ext_logged(seq);
            unplugs += lost;
            printf("    run %u: plugged in at %u s: %s, %s after 45 min%s%s\n", (unsigned)i, (unsigned)t0 / 1000,
                   ext_logged(seq) ? "seen" : "not seen", board_power_name(st().power), lost ? ", lost: " : "",
                   lost ? board_bat_ev_name(off) : "");
        }
    }
    s_seed_n -= 4;
    s_seed_r -= 4;
    printf("    %d h: %d runs of %d with a false charger, %d with the percent up; plug-ins: %d not seen in 45 min of "
           "use, %d lost meanwhile\n",
           runs * 2, false_n, runs, rise_n, missed, unplugs);
    CHECK(rise_n == 0 && false_n * 50 <= runs && missed * 50 <= runs);     // at most 2 % each
}


// ---- v4 review: the cases the first v4 got wrong

// Constant use (a 5 s turn, a 25 s answer, 20 s idle, again and again for 38 min: no settled level at all) drains the
// cell 4 mV/min from 46 %: the percent follows it down (the most recovered level of the last 2 min, once 5 min passed
// without a calm reading), reaches the 15 % warning, and never rises
static void t_busy_drain(void)
{
    printf("t_busy_drain\n");
    boot(3790, -0.3, false);
    run(5 * 60000);
    int pct0 = st().pct;
    CHECK(pct0 >= 40 && pct0 <= 48);
    watch_reset();
    a.mono = true;
    c.rate = -4;
    int lag = 0;
    for (int i = 0; i < 38 * 60 / 50; i++) {
        turn(5000);
        run(5000);
        speaker_d(25000, 100, 10, 3);
        run(45000);
        int lvl = pct_of((int)c.rest);
        if (i * 50 >= 10 * 60 && st().pct - lvl > lag) {
            lag = st().pct - lvl;           // 10 min in: no more than a few points above the resting cell
        }
    }
    board_bat_t b = st();
    CHECK(lag <= 6 && a.pct_min <= 15 && a.bat_rise == 0 && b.power == BOARD_PWR_BATTERY);
    if (lag > 6 || a.pct_min > 15 || s_verbose) {
        printf("    lag %d, lowest %d %%, now %d %% at %.0f mV rest\n", lag, a.pct_min, b.pct, c.rest);
    }
    // the use stops: no jump down (it already followed), nor up
    watch_reset();
    a.mono = true;
    c.rate = -0.3;
    run(10 * 60000);
    CHECK(a.bat_rise == 0 && a.drop_max <= 3);
}

// t_full_glide when no reading is calm as FULL ends (the screen touched every 70 s): the glide still takes 1 point at
// a time, also once the first settled level comes
static void t_full_glide_busy(void)
{
    printf("t_full_glide_busy\n");
    boot(4040, -0.2, false);
    run(5 * 60000);
    g_usb = true;
    CHECK(until_power(BOARD_PWR_FULL, 20000) >= 0 && st().pct == 100);
    g_usb = false;
    run(60000);
    CHECK(st().ext == BOARD_EXT_KEPT);
    c.rate = -3;
    g_bl = 30;
    watch_reset();
    int t = -1;
    for (int i = 0; i < 40 * 60 / 70 && t < 0; i++) {
        screen(g_bl == 30 ? 31 : 30);
        for (int k = 0; k < 700 && t < 0; k++) {
            tick();
            t = st().power == BOARD_PWR_BATTERY ? i : -1;
        }
    }
    CHECK(t >= 0);
    for (int i = 0; i < 20 * 60 / 70; i++) {
        screen(g_bl == 30 ? 31 : 30);
        run(70000);
    }
    run(15 * 60000);
    board_bat_t b = st();
    CHECK(a.drop_max <= 1 && iabs(b.pct - pct_of(b.mv)) <= 1);
    if (a.drop_max > 1) {
        printf("    drop %d, now %d %% (curve %d)\n", a.drop_max, b.pct, pct_of(b.mv));
    }
}

// Unplugged and used on at once (the screen woken to look, dimmed after a minute, off 2 min later; or turns every
// 60-150 s): BATTERY within a minute or two for modest to strong chargers (the first v4: up to an hour). The wake and
// dim: the first settled reading 60 s after the dim (v3's 40 s took the backlight's own sag for part of the fall). A
// 30 mV charger (under the 35 mV fall) only as the cell drains, or by NO_RISE
static void t_unplug_busy(void)
{
    printf("t_unplug_busy\n");
    static const struct {
        double ohm, pol;
        bool turns;
        int max_s;
    } k[] = { { 35, 10, false, 150 }, { 60, 20, false, 60 }, { 20, 10, false, 900 },
              { 35, 10, true, 150 }, { 45, 15, true, 150 }, { 60, 20, true, 150 } };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        boot(3900, -0.3, false);
        c.bl_mv = 0.25;
        run(5 * 60000);
        charger_soft(k[i].ohm, k[i].pol, 1.5, 0, 10);
        CHECK(until_power(BOARD_PWR_CHARGING, 3 * 60000) >= 0);
        run(20 * 60000);
        uint32_t seq = log_next();
        charger(0, 0, -0.3, 0);
        screen(80);
        uint32_t t0 = now_ms(), next = t0 + 60000 + (uint32_t)rnd(0, 90) * 1000;
        int t = -1;
        while (t < 0 && now_ms() - t0 < 60 * 60000) {
            uint32_t q = now_ms() - t0;
            if (k[i].turns && now_ms() >= next) {
                turn(5000);
                run(5000);
                speaker_d(10000, 100, 4, 1.5);
                next = now_ms() + 60000 + (uint32_t)rnd(0, 90) * 1000;
            } else if (!k[i].turns && q >= 180000) {
                screen(0);
            } else if (!k[i].turns && q >= 60000) {
                screen(25);
            }
            tick();
            t = st().power == BOARD_PWR_BATTERY ? (int)(now_ms() - t0) : -1;
        }
        CHECK(t >= 0 && t <= k[i].max_s * 1000 && (logged(BOARD_BEV_FALL, seq) || logged(BOARD_BEV_STEP_DOWN, seq) || logged(BOARD_BEV_NO_RISE, seq)));
        if (t < 0 || t > k[i].max_s * 1000 || s_verbose) {
            printf("    case %d: BATTERY after %d s\n", (int)i, t / 1000);
        }
    }
}

// An ungraded load (OTA, a fetch on a weak link) while every reading is settled: the cell reads 16-30 mV low for
// 1-10 min, then back: the small step down arms the "load ended" guard, which the trends respect too
static void t_ungraded_load(void)
{
    printf("t_ungraded_load\n");
    static const struct {
        double mv;
        int s;
        double jitter, burst_p;
    } k[] = { { 20, 60, 0, 0 }, { 16, 120, 0, 0 }, { 25, 300, 0, 0 }, { 20, 120, 6, 0.25 }, { 30, 600, 0, 0 } };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        boot(3880, -0.3, false);
        c.jitter = k[i].jitter;
        c.burst_p = k[i].burst_p;
        c.burst = 20;
        run(10 * 60000);
        uint32_t seq = log_next();
        watch_reset();
        a.mono = true;
        c.sag = k[i].mv;
        run((uint32_t)k[i].s * 1000);
        c.sag = 0;
        run(60 * 60000);
        CHECK(!ext_logged(seq) && a.chg_max == 0 && a.bat_rise == 0);
        if (ext_logged(seq) || a.chg_max) {
            printf("    case %d: charging %d s\n", (int)i, a.chg_max / 1000);
        }
    }
}

// A decaying rise on battery that is no load's recovery: the cell warming up after the cold (its resistance under the
// idle load falls: +25 mV as 1 - exp(-t / 15 min), and harsher). Over the slow trend's 6-16 min it can pass for a CC
// charge (the ADC's 3 mV steps hide the slowing down); if it is taken for one, the missing rise ends that within
// TREND_EXIT_MS (NO_RISE), and the percent never rises
static void t_warm_up(void)
{
    printf("t_warm_up\n");
    static const double k[][2] = { { 25, 15 }, { 25, 10 }, { 30, 20 }, { 20, 8 } };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        boot(3850, -0.3, false);
        run(10 * 60000);
        uint32_t seq = log_next();
        watch_reset();
        a.mono = true;
        double w = 0;
        for (int s = 0; s < 2 * 3600; s++) {
            double w1 = k[i][0] * (1.0 - exp(-(s + 1) / (k[i][1] * 60.0)));
            c.rest += w1 - w;
            w = w1;
            run(1000);
        }
        bool ok = !ext_logged(seq) || logged(BOARD_BEV_NO_RISE, seq);
        CHECK(ok && a.chg_max <= TREND_EXIT_MS + 60000 && a.bat_rise == 0 && st().power == BOARD_PWR_BATTERY);
        if (!ok || a.chg_max > TREND_EXIT_MS + 60000) {
            printf("    case %d: charging %d s\n", (int)i, a.chg_max / 1000);
        }
    }
}

// battery_test.exe [-v [-t]] [-n seed] [-r seed] [name]: -v the model's log, -t every reading as well; -n / -r the
// noise's / the usage schedules' seed; name: only the tests whose name contains it (x_ tests run only by name)
int main(int argc, char **argv)
{
    int i = 1;
    s_verbose = i < argc && !strcmp(argv[i], "-v") && ++i;
    s_trace = i < argc && !strcmp(argv[i], "-t") && ++i;
    for (; i + 1 < argc && (!strcmp(argv[i], "-n") || !strcmp(argv[i], "-r")); i += 2) {
        *(argv[i][1] == 'n' ? &s_seed_n : &s_seed_r) = (uint32_t)strtoul(argv[i + 1], NULL, 10);
    }
    const char *only = i < argc ? argv[i] : NULL;
    g_now_us = 1000000;
    static const struct {
        const char *name;
        void (*fn)(void);
    } k_tests[] = {
#define T(f) { #f, f }
        T(t_wall_plug_unplug), T(t_usb_full_no_step), T(t_user_report),  T(t_usb_cv_plateau), T(t_slow_charge),
        T(t_usb_boot_full),    T(t_macbook),          T(t_weak_port_drain), T(t_wall_full),    T(t_not_a_charger),
        T(t_log),              T(t_vfull),            T(t_nobat),        T(t_end_of_charge),  T(t_charger_seen),
        T(t_full_glide),       T(t_vfull_guard),      T(t_usb_flicker),  T(t_short_charge),   T(t_charger_idle),
        T(t_recharge_cycle),   T(t_field_report),     T(t_plug_soft_start), T(t_plug_split),   T(t_plug_busy),
        T(t_plug_noisy),       T(t_plug_modest),      T(t_boot_on_charger), T(t_real_use),     T(t_busy_drain),
        T(t_full_glide_busy),  T(t_unplug_busy),      T(t_ungraded_load), T(t_warm_up),        T(x_stress),
#undef T
    };
    for (size_t k = 0; k < sizeof(k_tests) / sizeof(k_tests[0]); k++) {
        if (only ? strstr(k_tests[k].name, only) != NULL : k_tests[k].name[0] != 'x') {
            k_tests[k].fn();
        }
    }
    printf("%d checks, %d failures\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
