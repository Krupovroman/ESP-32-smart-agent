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
    double ohm;             // the net charge current x the internal resistance: at once
    double pol, pol_to;     // the polarisation, relaxing towards pol_to
    double cv;              // the charger's CV level as read (0 = none): the cell is held there
    double sag;             // a load's sag (the speaker): at once
    double wave, wave_min;  // a slow wobble of the rest level: amplitude (mV) and period (min)
    double noise;           // +- per ADC read (mV at the cell)
    int spike, spike_reads; // a spike on the next reads
    uint32_t rng;
} c;

int sim_adc_pin_mv(void)
{
    c.rng = c.rng * 1664525u + 1013904223u;
    double n = ((double)((c.rng >> 8) & 0xFFFF) / 32768.0 - 1.0) * c.noise;
    double w = c.wave_min > 0 ? c.wave * sin((double)g_now_us / 60e6 / c.wave_min * 6.2831853) : 0;
    double v = c.rest + w + c.ohm + c.pol;
    if (c.cv > 0 && v > c.cv) {
        v = c.cv;
    }
    v -= c.sag;
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
    uint32_t load_t;        // the last load step
    uint32_t t_bat;
    bool polled;
    int samples;
    // watched while the test runs
    int chg_run, chg_max;   // CHARGING: the current and the longest stretch (ms)
    int changes;            // power state changes
    int pct_min, pct_max;
    int drain_pct_max;      // the highest percent shown in USB_DRAIN
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
    if (a.audio_until || (a.audio_end && now - a.audio_end < 15000)) {
        return BOARD_BAT_SKIP;
    }
    return now - a.load_t >= 60000 ? BOARD_BAT_SETTLED : BOARD_BAT_LOADED;
}

static void watch_reset(void)
{
    board_bat_t b = st();
    a.chg_run = a.chg_max = a.changes = 0;
    a.pct_min = a.pct_max = b.pct;
    a.drain_pct_max = -1;
    a.last = b.power;
}

static void tick(void)
{
    g_now_us += 100000;
    uint32_t now = now_ms();
    c.rest += c.rate / 600.0;
    c.pol += (c.pol_to - c.pol) * (0.1 / POL_TAU_S);
    if (a.audio_until && now >= a.audio_until) {
        a.audio_until = 0;
        a.audio_end = a.load_t = now;
        c.sag = 0;
    }
    board_bat_t b = st();
    uint32_t poll = b.poll_ms > 0 ? (uint32_t)b.poll_ms : 5000;
    if (!a.polled || now - a.t_bat >= poll) {
        a.polled = true;
        a.t_bat = now;
        a.samples++;
        board_battery_sample(quality(now));
        b = st();
        if (s_trace) {
            printf("    %8.1f q%d raw %d mv %d %s ext %s d3 %d%s d4 %d%s pct %d hi? peak %d\n", now / 1000.0,
                   (int)quality(now), b.raw, b.mv, board_power_name(b.power), board_ext_name(b.ext), b.d3,
                   b.d3_ok ? "" : "x", b.d4, b.d4_ok ? "" : "x", b.pct, s_m.level_peak);
        }
    }
    if (b.power != a.last) {
        a.changes++;
        a.last = b.power;
    }
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
    c.sag = sag;
}

// A new boot on a cell resting at rest (mV as read), drifting by rate mV/min, the USB host as given
static void boot(double rest, double rate, bool usb)
{
    memset(&s_m, 0, sizeof(s_m));           // the next reading starts the model afresh
    s_vfull = VFULL_DEF_MV;
    s_sim_on = false;
    memset(&c, 0, sizeof(c));
    c.rest = rest;
    c.rate = rate;
    c.noise = 12;                           // +-12 mV per ADC read at the cell: about 3 mV on an 8-read average
    c.rng = 12345;
    g_usb = usb;
    g_now_us += 1000000;
    memset(&a, 0, sizeof(a));
    a.load_t = now_ms();                    // booting is a load (Wi-Fi, TLS): settled after a minute
    board_battery_log_clear();
    tick();
    watch_reset();
}

// The charger: plugged in (ohm at once, pol within a minute, the cell rising at rate) or out
static void charger(double ohm, double pol, double rate, double cv)
{
    c.ohm = ohm;
    c.pol_to = pol;
    c.rate = rate;
    c.cv = cv;
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
    CHECK(b.pct == 100 && logged(BOARD_BEV_PLATEAU, seq) && logged(BOARD_BEV_VFULL, seq));
    CHECK(b.vfull_learn >= 4112 && b.vfull_learn <= 4132 && b.vfull == b.vfull_learn);
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
    CHECK(st().vfull_learn >= 4130 && st().vfull_learn <= 4150);
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
    CHECK(b.vfull_learn >= 4130 && b.vfull_learn <= 4150);
    c.rest = 4115;
    charger(0, 0, -0.3, 0);
    watch_reset();
    run(20 * 60000);
    CHECK(st().power == BOARD_PWR_FULL && a.changes == 0 && st().ext == BOARD_EXT_CHARGER);
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

int main(int argc, char **argv)
{
    s_verbose = argc > 1 && !strcmp(argv[1], "-v");
    s_trace = argc > 2 && !strcmp(argv[2], "-t");      // every reading as well
    g_now_us = 1000000;
    t_wall_plug_unplug();
    t_usb_full_no_step();
    t_user_report();
    t_usb_cv_plateau();
    t_slow_charge();
    t_usb_boot_full();
    t_macbook();
    t_weak_port_drain();
    t_wall_full();
    t_not_a_charger();
    t_log();
    t_vfull();
    t_nobat();
    printf("%d checks, %d failures\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
