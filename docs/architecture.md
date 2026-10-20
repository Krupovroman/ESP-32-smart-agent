# Firmware architecture: Round AI Assistant (reviewed revision)

**Board:** Waveshare ESP32-S3-Touch-LCD-1.85 rev 2.1. ESP32-S3R8 with 8 MB octal PSRAM and 16 MB QIO flash, native USB-Serial-JTAG on COM7.
**Toolchain:** ESP-IDF v6.1, installed by EIM in `C:\esp\v6.1\esp-idf`. C. LVGL 9.5.0 through esp_lvgl_port 2.9.0.
**Repository:** `<repo>`, any ASCII path with no spaces (e.g. `C:\esp32-ai-assistant`).

## Status read from the repo on 2026-10-17

- **M1 is done and tagged `m1`** (commit `0ad4609`). It covers the power latch, I2C0/I2C1, TCA9554, the ST77916 panel-ID probe, the CST816, the LVGL port, and an esp_console REPL with `shot`, `tap` and `swipe`.
- **M2 is committed but not tagged** (commit `e9f33c0`). It covers I2S mic capture and PCM5101 output, plus the `mic` and `spk` commands.
- **Uncommitted work in progress:** `audio_in_set_rate()` and `mic rate`. Commit it or drop it before tagging `m2`.
- **This unit's hardware:**
  - The panel reports ID `00 02 7F 7F`, which selects batch 2 and the `vendor_specific_init_new` table (`shots/m2_rec.png`).
  - Touch works with `CONFIG_ESP_LCD_TOUCH_CST816S_DISABLE_READ_ID=y`.
  - The colour order is correct with `LCD_RGB_ELEMENT_ORDER_RGB` and `swap_bytes=1`.
- **`sdkconfig` is tracked in git.** It already has CPU 240 MHz, `FREERTOS_HZ=1000`, `-O2`, the FULL cert bundle, CLIB LVGL malloc, `SPIRAM_MALLOC_ALWAYSINTERNAL=4096` and a 16 KB I-cache.
- **Measured footprint of the M2 build** (`esp_idf_size build/round_assistant.map`):

  | Item | Value |
  |---|---|
  | DIRAM total | 341,760 B |
  | Static DIRAM used | 79,534 B (.text 55,567 + .data 16,727 + .bss 7,240) |
  | IRAM | 16,384 B, full |
  | Flash image | 731,610 B |

  §3 builds on these numbers.

## Sources of truth

| Document | What it owns |
|---|---|
| `docs/hardware.md` | Pins and hardware quirks |
| `docs/protocol.md` | The wire contract: request and response bodies, headers, timeouts, retries, error table, voices, sentence splitting, model pickers |
| `docs/design/ui_{calm,expressive,rim}.md` | The visual concept. One still has to be chosen |
| This document | Firmware structure, tasks, memory, configuration, dev tooling and tests |

When this document and `protocol.md` disagree about the wire format, `protocol.md` wins. The single deliberate exception is the FULL cert bundle (§4); update protocol.md §1.3 to match.

**Markers used below:**

- *(xiaozhi)*: taken from xiaozhi-esp32 commit `d91d682`, mirrored in `third_party/xiaozhi_ref/` (MIT).
- *(proven)*: already running in this repo on this unit.
- *(measure Mx)*: must be confirmed by the acceptance tests of milestone x.

---

## 0. Baseline: xiaozhi, what this repo has proven, and what this firmware does

| Item | xiaozhi | Proven in this repo (M1/M2) | This firmware |
|---|---|---|---|
| I2C0 | `i2c_new_master_bus` port 0, SDA 11, SCL 10 | Same, plus `glitch_ignore_cnt=7` and internal pull-ups (the PCB also has 10 k) | Unchanged |
| TCA9554 (0x20) | P0\|P1 set as outputs, driven 1 → 0 → 1 with 300 ms steps | P0\|P1\|P2 outputs (P2 = EXIO3 SD CS, held high); P3/P4 inputs. Reset pulse: 1, wait 10 ms, 0, hold 20 ms, 1, wait 120 ms | Keep the repo timing. If the panel-ID read fails, pulse again with 300/300 ms and probe once more |
| QSPI bus | SPI2, D0..D3 = 46/45/42/41, SCLK 40, `max_transfer_sz = 360*80*2` | `360*40*2` (matches the draw buffer) | Unchanged |
| Panel probe | 3 MHz IO, `lcd_cmd_bits=32`, `lcd_param_bits=8`, `quad_mode=1`, `esp_lcd_panel_io_rx_param(io, (0x0B<<24)\|(0x04<<8), id, 4)`. The probe IO is leaked | Same, but `esp_lcd_panel_io_del()` runs after the probe | Unchanged |
| Run clock | 80 MHz | 80 MHz *(proven)* | `CONFIG_RA_LCD_PCLK_MHZ`, default 80 |
| Init table | `00 02 7F 7F` → `vendor_specific_init_new` (184 commands); anything else → driver default | Same, with `vendor_config.flags.use_qspi_interface=1` and `init_cmds_size` set | Unchanged. Both tables send INVON (0x21). Neither sends TEON |
| Panel config | RGB element order, 16 bpp, `reset_gpio_num = NC` (the driver sends SWRESET) | Same | Unchanged |
| Rounder | None | Even-coordinate rounder | Keep it *(proven)*. `CONFIG_RA_LCD_EVEN_ROUNDER=y` |
| LVGL port | 360×20 single buffer, priority 1, core 1 | 360×40 **double** buffer, internal DMA, `swap_bytes=1`, priority 4, core 1, 8 KB stack, `task_max_sleep_ms=100` | Unchanged |
| LVGL heap | Built-in TLSF, 1 MB pool allocated in PSRAM through `LV_MEM_POOL_INCLUDE` | CLIB malloc. With `ALWAYSINTERNAL=4096`, every LVGL object of 4 KB or less lands in **internal** RAM | **Switch to xiaozhi's pool in PSRAM.** It frees internal RAM and makes `lv_mem_monitor()` work |
| Touch | Not initialised | CST816 on I2C1 at 400 kHz, INT on GPIO4, reset by the board pulse, ID read disabled. Own TIMER-mode indev; I2C is read only within 60 ms of an INT pulse | Unchanged, plus a lazy chip-ID read |
| Touch injection | – | A second pointer indev driven from the console | Keep it. Add a timed sample queue |
| Screenshot | – | Flush-shadow capture: `LV_EVENT_FLUSH_START` copies each flushed area (before the byte swap) into a 360×360 PSRAM frame | Keep this as the default `shot`, and run it in taskLVGL. Add `shot snap` (lv_snapshot) |
| Speaker | I2S0 at 24 kHz, 32-bit, MONO + `SLOT_BOTH`, DMA 6×240, volume (v/100)²·65536 | DMA 8×240, `auto_clear_after_cb`, 256 KB ring, task on core 0 at priority 8 | Ring grows to 2 MB, allocated lazily (protocol §5.3). Task moves to core 1, priority 6 |
| Mic | I2S1 at 16 kHz, 32-bit, MONO, RIGHT slot, `>>12` | Same, plus a DC blocker; task on core 0 at priority 7 | Task moves to core 1. Injected audio bypasses the shift and the DC blocker |
| Power latch | GPIO7 driven high after display init | `gpio_config` then `set_level`, first thing in `app_main` | Bootloader hook, then `app_main` again: level first, then direction |
| Buttons | `espressif/button ~4.2.1` | Raw GPIO reads | `espressif/button ==4.2.1` GPIO devices (§1.2) |
| sdkconfig | `-Os`, ALWAYSINTERNAL 1024, RESERVE 102400, I-cache 32 KB, Wi-Fi IRAM opts off, `DYNAMIC_BUFFER`, LVGL keyboard off | `-O2`, ALWAYSINTERNAL 4096, RESERVE 65536, I-cache 16 KB | See §4 |
| Partitions | 2 × 0x3F0000 app, 8 MB assets | 2 × 4 MB app, 7.75 MB storage, 128 KB coredump | Keep the repo table |

**Licences.**

- `components/display/st77916_init_new.inc` and the bring-up sequence keep the xiaozhi MIT notice.
- `components/dns_server` is copied from the IDF `captive_portal` example (Unlicense OR CC0-1.0).
- Fonts are Montserrat (OFL), shipped with `assets/fonts/OFL.txt`.
- The CJK font of the storage pack is a subset of Noto Sans CJK SC (SIL OFL 1.1, renamed "RA CJK"), with `assets/fonts/NotoSansCJK-OFL.txt`; the pack carries the same text as `cjk.ofl`. stb_truetype (inside LVGL's Tiny TTF) is MIT / public domain.
- Nothing is copied from WU-AetherCore (PolyForm Noncommercial).

---

## 1. Directory layout, components, public C APIs

### 1.0 Layout after the M3.0 restructure

The restructure changes no behaviour. The M1 and M2 tests are re-run afterwards.

```
esp32-ai-assistant/
├─ CMakeLists.txt                # MINIMAL_BUILD ON (§6)
├─ sdkconfig.defaults            # §4, dev profile
├─ sdkconfig.defaults.release    # §4 overlay
├─ partitions.csv                # §5, unchanged since M1
├─ dependencies.lock             # committed (registry-only deps)
├─ eim_config.toml               # existing
├─ bootloader_components/power_latch/   CMakeLists.txt power_latch.c
├─ main/
│   ├─ CMakeLists.txt idf_component.yml Kconfig.projbuild
│   ├─ main.c selftest.c                       # §8
│   ├─ app/  app.c app.h app_evt.h dev_cmds.c   # state machine + app/ui console commands
│   └─ ui/   ui.c ui.h ui_geom.h ui_subject.h ui_theme.c ui_orb.c ui_home.c ui_chat.c
│            ui_translator.c ui_settings.c ui_keyboard.c ui_portal.c ui_test.c ui_str.c
│            lvgl_psram_pool.h fonts/font_ru_{16,20,28,40}.c   # generated, committed
├─ components/
│   ├─ util/       util.c b64.c psram.c            (cJSON hooks → PSRAM, CRC wrapper)
│   ├─ board/      board.c board_pins.h buttons.c battery.c
│   ├─ display/    display.c st77916_init_new.inc
│   ├─ touch/      touch.c inject.c
│   ├─ audio/      audio_in.c audio_out.c wav.c vad.c
│   ├─ settings/   settings.c settings_table.inc
│   ├─ net/        wifi.c portal.c portal/index.html
│   ├─ dns_server/ (IDF example copy)
│   ├─ orhttp/     orconn.c errors.c sse.c jf.c    (sse/jf are only used by the v1.1 streaming path)
│   ├─ ai/         ai_client.c prompts.c history.c models.c tts_client.c tts_split.c voices.c stt_client.c
│   ├─ ota/        ota.c
│   └─ devconsole/ devconsole.c cmd_sys.c cmd_io.c cmd_audio.c cmd_net.c cmd_ota.c upload.c
├─ assets/  fonts/ (TTF + OFL.txt, cjk/RACJK-Medium.ttf → storage asset pack, §1.0)
├─ tools/   idf.ps1 (exists) dev.py (exists, protocol v2) py.ps1 build.ps1 flash.ps1 release.ps1
│           restore.ps1 slot.ps1 mkfonts.ps1 secrets_push.py mkaudio.py
├─ tests/   acceptance.py golden/*.rgb565.z audio/*.wav sdkconfig.forcefail sdkconfig.forcepanic
├─ backups/ factory_full_16MB.bin (gitignored) README.md
└─ third_party/xiaozhi_ref/
```

**Migration map** (M3.0, one commit, no functional change):

| From | To |
|---|---|
| `main/board/board*.{c,h}` | `components/board/` |
| `main/board/display.*`, `st77916_init_new.inc` | `components/display/` |
| `main/board/touch.*`, `main/dev/inject.*` | `components/touch/` |
| `main/audio/*` | `components/audio/` |
| `main/dev/devconsole.*` | `components/devconsole/` (moves from the REPL to the line protocol of §7) |
| `main/ui/ui_test.*` | `main/ui/ui_test.*` (stays; gains named test screens) |
| `tools/dev.py` | `tools/dev.py` (protocol v2 in §7; it also accepts id-less lines, so `idf.py monitor` stays usable) |

**Component `REQUIRES`.** These are needed because MINIMAL_BUILD is on. Managed components are referenced by their manager names.

| Component | REQUIRES (PRIV_REQUIRES in brackets) |
|---|---|
| util | heap esp_rom (espressif__cjson) |
| board | esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_adc espressif__esp_io_expander_tca9554 (espressif__button) |
| display | board esp_lcd esp_driver_spi espressif__esp_lvgl_port lvgl__lvgl (espressif__esp_lcd_st77916) |
| touch | board esp_lcd espressif__esp_lvgl_port (espressif__esp_lcd_touch_cst816s esp_timer) |
| audio | esp_driver_i2s util |
| settings | nvs_flash util |
| net | esp_wifi esp_netif esp_event esp_http_server settings (dns_server lwip) |
| orhttp | esp_http_client esp-tls mbedtls util |
| ai | orhttp audio settings util (espressif__cjson) |
| ota | app_update esp_https_ota esp_partition settings |
| assets | esp_partition (esp_rom esp_timer) |
| devconsole | console esp_driver_usb_serial_jtag util (display touch audio settings net ota ai) |

The dependency direction is `main → {devconsole, ai, ota, net, orhttp, audio, touch, display, board, settings, util}`. `devconsole` never includes `main`. App and UI commands are registered from `main/app/dev_cmds.c` through `devconsole_register()`.

The Inter fonts are compiled into the app as rodata, so every A/B slot carries fonts that match its code. The `storage` partition (7.75 MB at 0x820000) holds the **asset pack**: today the CJK fallback font of the translator (ui.md 1.3), later optional UI sounds.

- **No file system.** The pack is a flat read-only image (`components/assets`, written by `tools/mkpack.py`): a 4 KB header (`"RAPK"`, format 1, entry count, pack size, CRC-32 of the header, a label) with up to 63 entries (name, 4 KB-aligned offset, size, CRC-32, aux), then the data. `assets_init()` finds the partition by label (its `spiffs` subtype in `partitions.csv` is only a label now; the table is unchanged), checks the header and maps the whole pack once with `esp_partition_mmap(…, ESP_PARTITION_MMAP_DATA)`: readers get plain pointers into the flash cache and nothing is copied to RAM. Nothing mounts, nothing formats, and a blank or foreign partition is just "absent".
- **Integrity.** `assets_verify()` checks every entry's CRC-32 (estimated 60-120 ms per MB through the flash cache with DIO reads and the ROM CRC, 0.3-0.6 s for the pack; `font status` reports it) in the one-shot `cjk_crc` task after boot (core 0, idle priority so it never takes CPU from app_main's boot, PSRAM stack: only its TCB is internal). A reader uses an entry only after that passed, so a half-written pack (the header is written first) is never parsed; a pack that fails it makes the UI drop a stored zh / ja / ko translator pair, as when the pack is missing.
- **Entries now:** `cjk.ttf` (5,043,260 B, 21,803 glyphs), `cjk.cov` (8 KB coverage bitmap of its cmap), `cjk.ofl` (its licence). The pack is 5,066,752 B, 62 % of the partition.
- **Build and flash.** `main/CMakeLists.txt` builds `build/storage.bin` (target `storage_pack`, part of every build, re-run only when its inputs change) and registers it with `esp_partition_register_target(storage …)`. It is **not** part of `idf.py flash`; `idf.py -p COM7 storage-flash` writes it (only when the font changes). OTA images never touch it, so both A/B slots share it, and a slot without CJK support simply ignores it.
- **Without the pack** the firmware runs as before: CJK languages are listed but disabled in the translator, and nothing else changes.

### 1.1 `util`

```c
void     util_init(void);                       /* cJSON_InitHooks({psram_malloc, free}) */
void    *psram_malloc(size_t n);  void *psram_calloc(size_t n, size_t sz);  char *psram_strdup(const char *s);
bool     util_stack_is_internal(void);          /* !esp_ptr_external_ram(&local) - guards flash writes */
size_t   b64_enc_len(size_t n);                 /* 4*((n+2)/3) */
size_t   b64_encode(const uint8_t *in, size_t n, char *out);   /* local 20-line encoder (protocol §1.4) */
int      b64_decode(const char *in, size_t n, uint8_t *out, size_t out_max);   /* -1 on error */
uint32_t crc32_ieee(uint32_t crc, const void *buf, size_t n);  /* esp_rom_crc32_le(); == zlib.crc32 (proven by dev.py) */
```

### 1.2 `board` (plus the bootloader hook)

```c
/* board_pins.h - unchanged from the repo (verified against hardware.md) */
#define BOARD_PIN_PWR_LATCH GPIO_NUM_7    /* out, 1 = stay on (battery) */
#define BOARD_PIN_PWR_KEY   GPIO_NUM_6    /* in, active low */
#define BOARD_PIN_BOOT_KEY  GPIO_NUM_0    /* in, active low, strapping */
#define BOARD_PIN_BAT_ADC   GPIO_NUM_8    /* ADC1_CH7, x3 divider */
/* I2C0 10/11 (TCA9554 0x20, PCF85063 0x51, QMI8658 0x6B); I2C1 1/3 + INT 4 (CST816 0x15);
   LCD QSPI CS21 SCK40 D0..D3 46/45/42/41, TE18 unused, BL5; SPK 48/38/47; MIC 15/2/39 */
#define BOARD_EXIO_TP_RST   (1u << 0)     /* EXIO1 */
#define BOARD_EXIO_LCD_RST  (1u << 1)     /* EXIO2 */
#define BOARD_EXIO_SD_CS    (1u << 2)     /* EXIO3, output high */
#define BOARD_EXIO_IMU_INT2 (1u << 3)     /* EXIO4, input */
#define BOARD_EXIO_IMU_INT1 (1u << 4)     /* EXIO5, input */

typedef enum { BOARD_BTN_BOOT, BOARD_BTN_PWR } board_btn_t;
typedef enum { BTN_EV_DOWN, BTN_EV_UP, BTN_EV_CLICK, BTN_EV_DOUBLE, BTN_EV_LONG } board_btn_ev_t;
typedef void (*board_btn_cb_t)(board_btn_t b, board_btn_ev_t ev, uint32_t held_ms, void *ctx); /* esp_timer task: post only */

void      board_power_latch(void);      /* gpio_set_level(7,1) THEN gpio_config(OUTPUT); never gpio_reset_pin(7) */
void      board_power_off(void);        /* backlight 0, GPIO7=0 (switches off only on battery) */
esp_err_t board_init(void);             /* I2C0 + TCA9554 (dirs, SD_CS=1) + I2C1 + LEDC (duty 0, fade ISR) + ADC1 oneshot */
esp_err_t board_reset_lcd_touch(bool slow);   /* EXIO1|EXIO2: 1,10ms,0,20ms,1,120ms; slow: 300/300/300 (xiaozhi) */
i2c_master_bus_handle_t  board_i2c0(void);
i2c_master_bus_handle_t  board_i2c1(void);
esp_io_expander_handle_t board_exio(void);
bool      board_exio_ok(void);          /* esp_io_expander_get_level() succeeded */
esp_err_t board_backlight_set(uint8_t pct, uint32_t fade_ms);  /* mutex-protected; LEDC low-speed, timer0/ch0, 20 kHz,
                                                                  10-bit, duty = 1023*pct^2/10000; ledc_fade_func_install(0) once */
uint8_t   board_backlight_get(void);
esp_err_t board_buttons_start(board_btn_cb_t cb, void *ctx);   /* espressif/button 4.2.1: iot_button_new_gpio_device,
                                                                  active_level 0, long_press_time BOOT 1500 / PWR 3000 ms */
int       board_battery_mv(void);       /* adc_oneshot ch7 + adc_cali curve fitting, ×3; -1 on error */
/* battery model v3 (ui.md 3.16): the app calls sample() every poll_ms (5 s, 2 s while a step is measured); median
   of 5, Li-ion curve scaled by 4200 / v_full; external power = USB host (usb_serial_jtag_is_connected), a settled
   voltage step of 35 mV (a wall charger; down = unplugged), the host lost without a step down = kept (macOS
   suspend); CHARGING / FULL (plug-in step < 15 mV near v_full, flat 4 min, or the charger stopping near v_full;
   held 15 min, its highest level is learned as v_full) / USB_DRAIN (settled -20 mV in 3 min with the host below
   v_full - 120, or FULL under v_full - 160); the ETA6098 has no status pin to the MCU and the system load hangs on
   the cell */
typedef enum { BOARD_PWR_BATTERY, BOARD_PWR_CHARGING, BOARD_PWR_FULL, BOARD_PWR_USB_DRAIN } board_power_t;
typedef enum { BOARD_EXT_NONE, BOARD_EXT_USB, BOARD_EXT_CHARGER, BOARD_EXT_KEPT } board_ext_t;
typedef enum { BOARD_BAT_SKIP, BOARD_BAT_LOADED, BOARD_BAT_SETTLED } board_bat_q_t;
typedef struct { int pct, mv; board_power_t power; bool charging, full, usb, sim; int raw, d3; bool d3_ok; int d4;
                 bool d4_ok; int ofs; unsigned skipped; board_ext_t ext; int vfull, vfull_learn, poll_ms;
                 board_bat_ev_t why; } board_bat_t;
bool      board_battery_sample(board_bat_q_t q);   /* SKIP: speaker / turn / backlight step lately (app_io.c), the
                                                      reading is not taken; SETTLED: no load step for 60 s; true
                                                      when pct / power / usb changed */
void      board_battery_state(board_bat_t *out);    /* any task */
void      board_battery_set_vfull(int mv);          /* setting bat_vfull (4000-4250, else 4150) */
void      board_battery_sim(int pct, board_power_t power); /* console "bat sim"; pct < 0 ends it */
uint32_t  board_battery_log_range(uint32_t *first); /* the battery log (PSRAM, 1536 records): minutes + events */
bool      board_battery_log_get(uint32_t seq, board_bat_log_t *out);
void      board_battery_log_clear(void);
```

**Button rules.** These fix a conflict in the draft, where holding BOOT for 1.5 s to talk triggered a cancel.

- **BOOT** reports DOWN and UP with `held_ms`. It has no long or double action, so hold-to-talk can last any length of time.
- **PWR** reports CLICK, DOUBLE and LONG (3 s). PWR events are **ignored until PWR has been seen released once after boot**. On battery, the user holds PWR to switch the board on; without this rule, a 3 s hold during boot would trigger power-off.
- **Roles (ui.md 2.3), firmware today:** PWR CLICK = lock / unlock (`ui_pwr_short()`; on a dark, unlocked screen it only wakes), LONG = power off. BOOT DOWN / UP = talk; the app turns a second BOOT press within 300 ms of a tap's release into a double tap (Assistant ⇄ Translator) and latches a tap's recording only after that window. The driver itself has no DOUBLE event.

**Bootloader hook.** `bootloader_components/power_latch/power_latch.c`:

```c
#include "esp_rom_gpio.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
void bootloader_hooks_include(void) {}
void bootloader_before_init(void)              /* no BSS / flash / logging yet: ROM calls + registers only */
{
    esp_rom_gpio_pad_select_gpio(7);
    esp_rom_gpio_connect_out_signal(7, SIG_GPIO_OUT_IDX, false, false);
    REG_WRITE(GPIO_OUT_W1TS_REG, BIT(7));
    REG_WRITE(GPIO_ENABLE_W1TS_REG, BIT(7));
}
```

Its `CMakeLists.txt` is `idf_component_register(SRCS "power_latch.c" REQUIRES esp_rom soc)`, the same pattern as the IDF v6.1 `custom_bootloader/bootloader_hooks` example.

The bootloader is only ever updated over USB. The app latches GPIO7 again in any case.

### 1.3 `display`

```c
typedef struct { uint8_t panel_id[4]; int panel_batch; /* 1 default, 2 new, 0 unknown */ bool init_ok; uint32_t pclk_hz; } display_info_t;
typedef struct { uint32_t frames, fps_x10, render_us_avg, render_us_max, flush_us_max; } display_stats_t;

esp_err_t display_init(void);                 /* needs board_init(); runs board_reset_lcd_touch(false) itself */
lv_display_t *display_get(void);
const display_info_t *display_info(void);
void      display_stats(display_stats_t *out, bool reset);  /* LV_EVENT_RENDER_START/READY, FLUSH_START/FINISH, REFR_READY */
esp_err_t display_set_invert(bool on);        /* takes the LVGL lock; esp_lcd_panel_invert_color */
esp_err_t display_sleep(bool sleep);          /* esp_lcd_panel_disp_sleep + backlight */
const uint16_t *display_capture(uint32_t timeout_ms);  /* §7 "shot"; runs in taskLVGL via lv_async_call + semaphore */
```

**Sequence inside `display_init()`** *(proven)*:

1. `spi_bus_initialize(SPI2_HOST, &(spi_bus_config_t)ST77916_PANEL_BUS_QSPI_CONFIG(40, 46, 45, 42, 41, 360*40*2), SPI_DMA_CH_AUTO)`.
2. **Probe.**
   - `io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(21, NULL, NULL)` (the macro's default is 40 MHz); set `io_cfg.pclk_hz = 3000000`.
   - `esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &probe)`.
   - `esp_lcd_panel_io_rx_param(probe, (0x0B<<24)|(0x04<<8), id, 4)`.
   - `esp_lcd_panel_io_del(probe)`.
   - If the read fails, run `board_reset_lcd_touch(true)` and probe once more.
3. Create the fast IO with `io_cfg.pclk_hz = CONFIG_RA_LCD_PCLK_MHZ*1000000`.
4. Build the vendor config: `st77916_vendor_config_t v = { .flags.use_qspi_interface = 1 }`. For batch 2, also set `v.init_cmds = st77916_init_new; v.init_cmds_size = 184`.
5. Create and start the panel:
   - `esp_lcd_new_panel_st77916(io, &(esp_lcd_panel_dev_config_t){ .reset_gpio_num = GPIO_NUM_NC, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16, .vendor_config = &v }, &panel)`.
   - Then `reset`, `init`, `disp_on_off(true)`.
   - If the `lcd_inv` setting is 0, call `invert(false)`.
6. **LVGL port:**
   - `lvgl_port_cfg_t pc = ESP_LVGL_PORT_INIT_CONFIG()`, then set `.task_priority=4, .task_stack=8192, .task_affinity=1, .task_max_sleep_ms=100, .timer_period_ms=5`.
   - `lvgl_port_display_cfg_t dc`: `buffer_size = 360*CONFIG_RA_LVGL_BUF_LINES`, `double_buffer = true`, `hres = vres = 360`, `color_format = LV_COLOR_FORMAT_RGB565`, `rounder_cb = even_rounder`, flags `buff_dma=1, buff_spiram=0, swap_bytes=1`.
7. `lv_display_add_event_cb()` hooks: FLUSH_START for the capture shadow, plus the counters for `display_stats`.
8. The backlight stays at 0 until the first `LV_EVENT_REFR_READY` after `ui_init()`. Then `board_backlight_set(bright, 300)`.

TE (GPIO18) stays unused. Neither init table sends TEON, and Espressif documents SPI/QSPI as tearing mode NONE.

**Capture.** The 259,200 B PSRAM shadow is allocated on the first `shot`, not at init.

`display_capture()` posts `lv_async_call(capture_cb)` and waits on a binary semaphore for up to `timeout_ms`. `capture_cb` runs in taskLVGL:

1. Set `s_capture=1`.
2. `lv_obj_invalidate(lv_screen_active())`.
3. `lv_refr_now(disp)`.
4. Set `s_capture=0`.
5. Give the semaphore.

This keeps rendering on taskLVGL's own 8 KB stack instead of the console's. The shadow holds native little-endian RGB565, copied before the swap, and includes the top and system layers.

### 1.4 `touch` (CST816 plus injection)

```c
esp_err_t   touch_init(lv_display_t *disp);      /* I2C1 panel IO + CST816 + real indev; non-fatal on failure */
lv_indev_t *touch_indev(void);
bool        touch_seen(void);                    /* an INT edge AND a successful read since boot */
uint8_t     touch_chip_id(void);                 /* reg 0xA7, read lazily after the first good read; 0 = unknown */
void        touch_set_raw_listener(void (*cb)(int16_t x, int16_t y, bool pressed));   /* "touch watch" */
/* injection: a second pointer indev; samples are queued and consumed by LVGL reads */
esp_err_t   inject_init(lv_display_t *disp);
esp_err_t   inject_push(int16_t x, int16_t y, bool pressed, uint16_t hold_ms);   /* non-blocking, 32-entry ring */
esp_err_t   inject_tap(int16_t x, int16_t y, uint16_t hold_ms);                   /* push press+release, wait until drained */
esp_err_t   inject_swipe(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t dur_ms, uint16_t steps);
esp_err_t   inject_wait_idle(uint32_t timeout_ms);
```

**Hardware.** All of this is *(proven)*:

- Panel IO: `esp_lcd_new_panel_io_i2c(board_i2c1(), &cfg, &tp_io)` with `cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG()` (address 0x15), then `cfg.scl_speed_hz = 400000`.
- `esp_lcd_touch_config_t`: `{ .x_max=360, .y_max=360, .rst_gpio_num=GPIO_NUM_NC, .int_gpio_num=GPIO_NUM_4, .levels={0,0} }`.
- `esp_lcd_touch_new_i2c_cst816s(tp_io, &cfg, &tp)`, then `esp_lcd_touch_register_interrupt_callback(tp, touch_isr)`. The driver installs the GPIO ISR service with flags 0, which makes it a non-IRAM ISR. The callback only sets a flag and a timestamp.
- **The ID read is disabled** with `CONFIG_ESP_LCD_TOUCH_CST816S_DISABLE_READ_ID=y`, because the CST816 sleeps and NACKs. `touch_chip_id()` therefore reads 0xA7 with `esp_lcd_panel_io_rx_param(tp_io, 0xA7, &id, 1)` after the first successful touch read, when the chip is awake. Waveshare documents the part as CST816T *(measure M3)*.
- Call `esp_log_level_set("CST816S", ESP_LOG_NONE)`. A NACK at finger release is normal.

**Why not `lvgl_port_add_touch()`.** Its read callback calls `ESP_ERROR_CHECK(esp_lcd_touch_read_data())` (verified in esp_lvgl_port 2.9.0 `esp_lvgl_port_touch.c:127`), so a NACK from the sleeping CST816 aborts.

**Real indev.** It runs in LVGL's default TIMER mode, read every `LV_DEF_REFR_PERIOD` (16 ms):

```c
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *d) {
    int64_t now = esp_timer_get_time();
    bool irq = atomic_exchange(&s_irq, false);
    if (irq) s_last_irq_us = now;
    if (irq || (s_pressed && now - s_last_irq_us < 60000)) {        /* INT pulses while the finger is down */
        esp_lcd_touch_point_data_t p[1]; uint8_t n = 0;
        s_pressed = esp_lcd_touch_read_data(s_tp) == ESP_OK &&
                    esp_lcd_touch_get_data(s_tp, p, &n, 1) == ESP_OK && n > 0;   /* NACK = released */
        if (s_pressed) { s_x = p[0].x; s_y = p[0].y; s_seen = true; if (s_raw_cb) s_raw_cb(s_x, s_y, true); }
    } else s_pressed = false;
    d->point.x = s_x; d->point.y = s_y;
    d->state = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
```

**Injection indev.** It is also in TIMER mode. LVGL handles the two pointer indevs independently.

- Each queued sample `{x, y, pressed, hold_ms}` is reported for at least one read and for at least `hold_ms`.
- A tap is `{press, 80}` followed by `{release, 0}`.
- A swipe is `steps` samples of `dur/steps` ms each (16 ms minimum), followed by a release. LVGL therefore sees real-time velocity for scrolling and gestures.
- The ring is guarded by a `portMUX` critical section. The writer is the console task and the reader is taskLVGL.
- Nothing in the injection path calls LVGL. The console must never hold the LVGL lock while it waits for injection to drain, because the drain needs taskLVGL to run.

Mirror and swap are set with `esp_lcd_touch_set_mirror_x/y` and `set_swap_xy`. The current unit needs none; M1 test 1.6 checks orientation.

### 1.5 `audio_in` (I2S1 RX → 16 kHz s16)

```c
typedef struct { const int16_t *pcm; size_t samples; uint32_t rate, ms; int16_t peak; int rms_dbfs_x10; uint8_t buf_idx; } audio_clip_t;
typedef struct { int rms_dbfs_x10[2]; int zero_permille[2]; int distinct[2]; } audio_probe_t;   /* [0]=RIGHT [1]=LEFT */
typedef enum { AUDIO_SRC_MIC, AUDIO_SRC_INJECT } audio_src_t;

esp_err_t audio_in_init(void);    /* channel enabled, capture task, 2 PSRAM clip buffers (960,000 B each) */
esp_err_t audio_in_start(uint32_t turn, uint32_t max_ms, bool vad_stop);
esp_err_t audio_in_stop(uint32_t turn, audio_clip_t *out);   /* lends the buffer to the caller */
void      audio_in_release(const audio_clip_t *clip);        /* ai_client is done with the buffer */
void      audio_in_abort(void);
bool      audio_in_recording(void);
int       audio_in_level(void);                              /* 0..100, atomic, 20 ms RMS (-60 dBFS = 0, -10 dBFS = 100) */
void      audio_in_set_format(bool right_slot, int shift);  /* from settings mic_slot / mic_shift */
esp_err_t audio_in_probe(uint32_t ms_per_slot, audio_probe_t *out);
esp_err_t audio_in_inject_load(const uint8_t *wav, size_t len);   /* RIFF, 16 kHz, mono, s16 only; PSRAM copy */
esp_err_t audio_in_set_source(audio_src_t src, bool realtime);
void      audio_clip_trim(const audio_clip_t *c, size_t *first, size_t *count); /* energy VAD, 200 ms margins (protocol §1.4) */
uint8_t  *audio_clip_to_wav(const audio_clip_t *c, size_t *len);            /* dev dump only, PSRAM */
```

**Channel setup:**

- `i2s_new_channel(&I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER) + .dma_desc_num=6, .dma_frame_num=240, NULL, &rx)`.
- Standard mode: `I2S_STD_CLK_DEFAULT_CONFIG(16000)` and `I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO)` with `slot_mask = RIGHT` *(proven on this unit)*.
- GPIOs: `bclk 15, ws 2, din 39, mclk UNUSED`.

**Capture task.** It reads 320 frames (20 ms) per call into an internal DMA-capable scratch buffer, then:

1. `s16 = clamp((s32 >> shift))`.
2. DC blocker: `y = x − x1 + 0.995·y1`.
3. RMS, peak, level.
4. Append to the active clip buffer.

**Injected source.** Samples are copied from the inject buffer in 20 ms chunks. The task waits 20 ms between chunks when `realtime`, and copies immediately otherwise. **Injected samples bypass the shift and the DC blocker**, so `mic dump` returns them bit-exact. Level, VAD and limits still apply.

**VAD** (`vad_stop`):

- The noise floor is the median of the first 300 ms.
- Speech means floor + 12 dB.
- Recording stops after 1000 ms of silence that follows at least 500 ms of speech. That posts `EV_REC_SILENCE`. Reaching the limit posts `EV_REC_LIMIT`.

**Buffers.** Two clip buffers (30 s × 16000 × 2 B) are used ping-pong. On barge-in, a new turn records into the other buffer while the cancelled upload may still be reading the first.

### 1.6 `audio_out` (I2S0 TX → PCM5101)

```c
typedef struct { uint32_t rate; uint64_t in_bytes; uint32_t underruns, write_errors; size_t queued_bytes; uint8_t volume; bool active; } audio_out_stats_t;
esp_err_t audio_out_init(void);                               /* channel at 24 kHz; ring allocated on the first begin() */
esp_err_t audio_out_begin(uint32_t turn, uint32_t rate, uint8_t channels);   /* rate 8000..48000, ch 1|2 */
size_t    audio_out_write(uint32_t turn, const void *s16le, size_t bytes, TickType_t wait);   /* xStreamBufferSend; 0 if stale turn */
void      audio_out_end(uint32_t turn);                       /* → EV_AUDIO_DRAINED when ring + DMA empty + 100 ms silence */
void      audio_out_stop(void);                               /* cancel/barge-in: 10 ms fade, reset ring */
void      audio_out_set_volume(uint8_t pct);
esp_err_t audio_out_tone(uint32_t hz, uint32_t ms, uint8_t amp_pct, uint32_t rate /*0 = current*/);
int       audio_out_level(void);
void      audio_out_get_stats(audio_out_stats_t *s, bool reset);
```

**Channel setup:**

- `I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, MASTER)` with `.dma_desc_num=8, .dma_frame_num=240, .auto_clear_after_cb=true`. The DMA sends zeros on underrun.
- Philips 32-bit, `SLOT_MODE_MONO` with `slot_mask = I2S_STD_SLOT_BOTH`. The hardware copies the mono sample to L and R, so each DMA frame is 4 bytes *(proven)*.
- GPIOs: `bclk 48, ws 38, dout 47, mclk UNUSED`.

**Rate changes.** When `begin()` gets a different rate, the play task waits until the ring is empty, then runs `i2s_channel_disable → i2s_channel_reconfig_std_clock(&I2S_STD_CLK_DEFAULT_CONFIG(rate)) → i2s_channel_enable`. The PCM5101 derives its clock from BCK, which runs at 64·fs.

**Ring and playback:**

- Ring: `xStreamBufferCreateWithCaps(2*1024*1024, 1, MALLOC_CAP_SPIRAM)`, created on the first `begin()`. It holds about 43 s at 24 kHz, so a complete 400-character TTS chunk fits and the next one can be prefetched (protocol §5.3).
- Playback starts after 150 ms are buffered, or at `end()`.
- Two channels are downmixed as (L+R)/2.
- Volume: `gain_q16 = vol²·65536/10000`, `out32 = clamp((int64)s16·gain)`, ramped over 10 ms.

**Statistics.**

- `in_bytes` counts **s16 input bytes**, as the existing code does. A 2 s tone at 24 kHz mono gives 96,000.
- `underruns` counts only empty reads while `active` (between `begin` and drained).

### 1.7 `settings` (NVS)

```c
typedef enum { ST_U8, ST_U16, ST_U32, ST_STR, ST_BLOB } st_type_t;
#define ST_SECRET 0x01   /* never printed: "sk-or-v1-…c3f9 (73)" */
#define ST_REBOOT 0x02
typedef struct { const char *ns, *key; st_type_t type; uint32_t def_num; const char *def_str;
                 int32_t min, max; uint16_t max_bytes; /* incl. NUL */ uint8_t flags; } setting_desc_t;
esp_err_t settings_init(void);    /* nvs_flash_init (erase+retry on NO_FREE_PAGES / NEW_VERSION_FOUND), PSRAM cache, cfg_ver migrate */
int32_t   settings_get_int(const char *key);
size_t    settings_get_str(const char *key, char *buf, size_t len);   /* copy under mutex */
esp_err_t settings_set_int(const char *key, int32_t v);                /* validate, nvs_set + nvs_commit */
esp_err_t settings_set_str(const char *key, const char *v);
esp_err_t settings_set_from_string(const char *key, const char *v);   /* console / portal */
esp_err_t settings_reset(bool keep_wifi);
const setting_desc_t *settings_table(size_t *n);
void      settings_on_change(void (*cb)(const char *key, void *ctx), void *ctx);   /* cb posts only */
```

**Write rules:**

- Every setter returns `ESP_ERR_INVALID_STATE` when `!util_stack_is_internal()`. A flash write from a task with a PSRAM stack would assert. The `ai` and `tts` tasks therefore post to `app` instead of writing.
- UI-originated writes are debounced 2 s and deferred until the app is IDLE. Flash writes stall the caches, and with them PSRAM and LVGL.
- Console writes apply immediately. They are test tooling.
- An older firmware that finds a newer `cfg_ver` reads the keys it knows and **never erases** anything.
- NVS encryption stays off on purpose: HMAC-based NVS encryption needs an irreversible eFuse burn. Secrets stay out of git and out of logs.

**Namespace `cfg`.** Every key is 15 characters or fewer, and the schema follows protocol.md. `S` marks a secret.

| Key | Type | Default | Range / notes |
|---|---|---|---|
| `ui_lang` | str 4 | `ru` | `ru` / `en` |
| `mode` | u8 | 0 | 0 assistant, 1 translator |
| `text_only` | u8 | 1 | 1 = no TTS requests (no speaker yet) |
| `speaker` | u8 | 1 | 0 = "no speaker" (O4 / S / D → Динамик), written by the UI; a stored 0 reaches the app as `UI_IN_SPEAKER` 0 when it registers its UI callback |
| `face` | u8 | 1 | "Лицо шара" (UI only) |
| `volume` | u8 | 60 | 0–100; 0 behaves like `text_only` for TTS |
| `bright` | u8 | 80 | 5–100 |
| `dim_s` | u16 | 60 | 0 = never |
| `tz` | str 33 | `MSK-3` | POSIX TZ string for `{DATETIME}` |
| `max_rec_s` | u8 | 20 | 3–30 (protocol: default 20, hard cap 30) |
| `vad_stop` | u8 | 0 | auto-stop in assistant mode |
| `mic_slot` | u8 | 0 | 0 right *(proven)*, 1 left |
| `mic_shift` | u8 | 12 | 8–16 |
| `ai_url` | str 129 | `https://openrouter.ai/api/v1` | must start with `https://` |
| `ai_key` | str 201 S | `""` | |
| `ai_model` | str 97 | `google/gemini-3.1-flash-lite` | |
| `ai_model_fb` | str 97 | `google/gemini-3.5-flash-lite` | `""` = send `"model"`, not `"models"` |
| `ai_effort` | str 13 | `auto` | `auto` = rule in protocol §2.1 rule 2; `""` = omit; never `none` |
| `ai_stream` | u8 | 0 | 0 = v1 non-streaming; 1 = SSE (v1.1, §1.10) |
| `ai_maxtok` | u16 | 1024 | 64–4096 (the translator uses 768) |
| `ai_web` | u8 | 1 | assistant web search (protocol §2.8): 0 off, 1 auto (`needs_web`, then a re-run with the plugin), 2 always; OpenRouter and its relays only |
| `ai_web_n` | u8 | 3 | 1–5 `max_results` of the web plugin (about $0.004 each) |
| `hist_n` | u8 | 6 | 0–10 exchanges |
| `hist_idle_min` | u8 | 10 | 0 = never |
| `sys_prompt` | **blob** ≤ 4000 B | absent = built-in prompt | Blob, because a 4000-byte NVS *string* must fit a single empty page |
| `tts_url` / `tts_key` | str 129 / 201 S | `""` → `ai_*` | |
| `tts_model` | str 97 | `microsoft/mai-voice-2.1-flash` | |
| `tts_speed` | u8 | 100 | 80–125 (×0.01); 100 = field omitted |
| `tts_rate` | u32 | 0 | 0 = Content-Type, then the per-model table |
| `vo_<lang>` | str 129 | absent | Per-language override `model\|voice`, for example `vo_ru`. Defaults come from the protocol §5.5 table |
| `stt_mode` | u8 | 0 | 0 audio → chat; 1 Whisper, then text chat |
| `stt_url` / `stt_key` / `stt_model` | str | `""` / `""` / `openai/whisper-large-v3-turbo` | |
| `tr_lang_a` / `tr_lang_b` | str 4 | `ru` / `en` | ISO 639-1; the UI enforces a ≠ b |
| `tr_other` | u8 | 0 | 0 = translate to A, 1 = ignore (show transcript only) |
| `tr_vad` | u8 | 1 | auto-stop in translator mode |
| `lcd_inv` | u8 | 1 | 1 = keep INVON from the init table |
| `ota_url` | str 201 | `""` | |
| `ref_url` | str 129 | `""` | `HTTP-Referer`, sent only to openrouter.ai |
| `bat_vfull` | u16 | 4150 | 4000–4250 mV: the battery divider's reading of a full cell on the charger (100 %); learned from a FULL that held 15 min (down ≤ 30 mV per charge, changes < 8 mV ignored) and stored by the app once idle; `set` wins over what was learned before it (ui.md 3.16) |
| `dev_flags` | u32 | 0 | b0 perf log, b1 HTTP trace (no secrets), b2 portal reachable on the STA IP, b3 allow `:free` models |

**Namespace `wifi`:** `n` u8; `ssid0`..`ssid4` str 33; `pass0`..`pass4` str 65 (S); `last` u8.

**Namespace `sys`:** `cfg_ver` u16 = 1; `boot_cnt` u32; `ok_ver` str 32 (last version that passed self-test); `rb_reason` str 64; `rb_cnt` u16.

With `CONFIG_ESP_WIFI_NVS_ENABLED=n`, the Wi-Fi driver writes nothing to NVS.

**Capacity.** Six NVS pages (one kept free for garbage collection) give about 630 entries of 32 bytes. Peak use is about 380: settings about 150, the prompt blob 126, Wi-Fi about 30, PHY calibration about 60.

### 1.8 `net` (Wi-Fi STA, scan, SoftAP portal)

```c
typedef enum { NET_OFF, NET_CONNECTING, NET_UP, NET_FAILED } net_state_t;
typedef struct { net_state_t state; char ssid[33]; int8_t rssi; char ip[16]; bool portal; char ap_ssid[33]; } net_status_t;
typedef struct { char ssid[33]; int8_t rssi; uint8_t auth, ch; } net_ap_t;
esp_err_t net_init(void);    /* esp_netif_init, esp_event_loop_create_default, STA+AP netifs, esp_wifi_init, esp_wifi_set_storage(WIFI_STORAGE_RAM) */
esp_err_t net_start(void);   /* STA with saved networks, best RSSI first; backoff 1,2,4,…,60 s */
esp_err_t net_connect(const char *ssid, const char *pass, bool save);
esp_err_t net_disconnect(void);
esp_err_t net_forget(const char *ssid);
esp_err_t net_scan(uint32_t timeout_ms, net_ap_t *out, int max, int *n);   /* ≤ 20 records */
void      net_status(net_status_t *out);
bool      net_wait_up(uint32_t timeout_ms);
esp_err_t net_portal_start(uint32_t idle_timeout_s);   /* default 600 s; posts EV_PORTAL_ON */
esp_err_t net_portal_stop(void);
size_t    net_portal_qr_payload(char *out, size_t len); /* "WIFI:T:WPA;S:<ssid>;P:<pass>;;" */
void      net_set_low_latency(bool on);                  /* WIFI_PS_NONE during a turn, WIFI_PS_MIN_MODEM when idle */
```

After `NET_UP`, the module starts SNTP with `esp_netif_sntp_init(&ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org"))`. TLS does not need it, because `MBEDTLS_HAVE_TIME_DATE` stays off. It is used for `{DATETIME}` in the prompt.

**Portal:**

- **Wi-Fi:**
  - Mode `WIFI_MODE_APSTA`, SSID `RoundAI-%02X%02X` (the last two MAC bytes).
  - WPA2-PSK with a random 8-digit password per session, shown on screen together with a QR code. `max_connection=2`.
  - The AP channel follows the STA. A phone may drop the AP while the STA rescans.
- **DHCP option 114.** The URI must outlive the call, because the API stores the pointer:
  ```c
  static char uri[] = "http://192.168.4.1/";
  esp_netif_dhcps_stop(ap);
  esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, uri, strlen(uri));
  esp_netif_dhcps_start(ap);
  ```
- **DNS:** `start_dns_server(&(dns_server_config_t)DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF"))`. It binds INADDR_ANY:53.
- **HTTP server:** `httpd_start` with `max_uri_handlers=12, max_open_sockets=7, lru_purge_enable=true, stack_size=6144, core_id=0`.
  - `GET /` serves `portal/index.html` (`EMBED_TXTFILES`, ≤ 8 KB, inline CSS/JS, no external resources).
  - `GET /scan.json` and `GET /status.json`.
  - `POST /save` (urlencoded, ≤ 4 KB) accepts `ssid, pass, ai_url, ai_key, ai_model, tts_model, tts_voice→vo_<ui_lang>, tr_lang_a, tr_lang_b`. An empty secret field keeps the stored value, and secrets are never echoed back.
  - Captive-detection URIs (`/generate_204`, `/hotspot-detect.html`, `/connecttest.txt`, `/ncsi.txt`) and the 404 handler answer `302 Location: http://192.168.4.1/`.
  - A request whose local socket address (`httpd_req_to_sockfd` + `getsockname`) is not 192.168.4.1 gets 403, unless `dev_flags` bit 2 is set. That bit is used by the M3 test.
- **The portal blocks AI turns.** `app` cancels any turn and refuses PTT with "Закройте настройку сети". The portal costs about 15 KB of internal RAM (§3), and setup is a separate mode anyway.

### 1.9 `orhttp` (HTTPS transport shared by AI, TTS, STT and GETs)

It implements protocol.md §1.2 to §1.5 and §9. There is **one connection object per host**, shared strictly in sequence behind a mutex: chat, then TTS chunk 1, then TTS chunk 2, and so on.

```c
typedef struct or_conn or_conn_t;
typedef struct { int connect_ms, write_ms, first_byte_ms, stall_ms, total_ms; } or_timeouts_t;
#define OR_TO_CHAT {10000, 10000, 30000, 15000, 45000}
#define OR_TO_SSE  {10000, 10000, 30000, 20000, 60000}
#define OR_TO_TTS  {10000, 10000, 15000, 10000, 60000}
#define OR_TO_STT  {10000, 10000, 30000, 15000, 45000}
#define OR_TO_GET  {10000,     0, 10000, 10000, 20000}
typedef struct { int status; int64_t content_length; char content_type[64], retry_after[16], gen_id[64], provider[32], rl_reset[24]; } or_resp_t;
typedef struct { const char *id; int http; bool retry; uint16_t retry_ms[2]; const char *msg_ru; } or_err_t;   /* protocol §9.1 rows */
typedef bool (*or_abort_fn)(void *ctx);

or_conn_t *or_conn_get(const char *base_url);               /* keyed by scheme+host; lazily creates the esp_http_client handle */
bool       or_conn_lock(or_conn_t *c, uint32_t ms);  void or_conn_unlock(or_conn_t *c);
esp_err_t  or_begin(or_conn_t *c, esp_http_client_method_t m, const char *url, const char *bearer,
                    const char *accept, int64_t body_len, const or_timeouts_t *to);   /* set_url/method/headers; open (reuses a live socket) */
esp_err_t  or_write_all(or_conn_t *c, const void *p, size_t n, or_abort_fn ab, void *ctx);
esp_err_t  or_write_b64_wav(or_conn_t *c, const int16_t *pcm, size_t samples, uint32_t rate, or_abort_fn ab, void *ctx);
           /* 44-byte WAV header + PCM as one base64 stream: 3072 raw bytes → 4096 chars per write, carrying 0–2 bytes across the header/PCM boundary */
esp_err_t  or_fetch_headers(or_conn_t *c, or_resp_t *r);   /* response headers come from HTTP_EVENT_ON_HEADER, not esp_http_client_get_header() */
int        or_read(or_conn_t *c, void *buf, int len, or_abort_fn ab, void *ctx);   /* 1 s socket ticks; -ESP_ERR_HTTP_EAGAIN = tick; enforces stall/total */
esp_err_t  or_read_body(or_conn_t *c, char **out, size_t cap, size_t *len);         /* PSRAM, NUL-terminated, caps from protocol §1.5 */
esp_err_t  or_finish(or_conn_t *c, bool ok);   /* ok: complete body → keep alive; else esp_http_client_close() */
void       or_classify(const or_resp_t *r, const char *body, esp_err_t transport, or_err_t *e);
```

**Client configuration** (field names verified in v6.1 `esp_http_client.h`):

- `crt_bundle_attach = esp_crt_bundle_attach`.
- `buffer_size = 4096`, `buffer_size_tx = 2048`.
- `disable_auto_redirect = true`.
- `keep_alive_enable = true, keep_alive_idle = 15, keep_alive_interval = 5, keep_alive_count = 3`. These are **TCP** keepalive probes. HTTP connection reuse comes from not calling `close()`: after `esp_http_client_is_complete_data_received()`, `or_finish(ok)` leaves the socket open, and the next `or_begin` calls `set_url`/`set_method`/`set_header` and `esp_http_client_open()` again.
- Timeouts are changed per phase with `esp_http_client_set_timeout_ms()`.

**Header rules:**

- `Authorization` on every request.
- `User-Agent: RoundAssistant/1.0 (ESP32-S3; ESP-IDF 6.1)`.
- `X-OpenRouter-Title` (and `HTTP-Referer` when `ref_url` is set) **only when the host is openrouter.ai**.
- **Never** send `Accept-Encoding`.
- Ignore `Set-Cookie`.

**Reuse failure.** If the first write on a reused connection fails, reconnect once without reporting it as a retry.

**Retry policy** (protocol §9.1):

- Backoff is `min(8 s, 1 s·2^k)·U(0.8, 1.2)`.
- `Retry-After` is honoured when it is 10 s or less.
- At most 20 s of retries per user action.
- Never retry a whole turn after TTS playback has started.

**Cancellation.** Workers check `abort()` between 4 KB writes and on every 1 s read tick. On abort they call `esp_http_client_close()` from their own task. Worst-case cleanup is the 10 s connect phase; the UI does not wait for it.

### 1.10 `ai_client` (OpenAI-compatible chat with audio)

```c
typedef enum { AI_MODE_ASSISTANT, AI_MODE_TRANSLATOR } ai_mode_t;
typedef struct { uint32_t turn; ai_mode_t mode; const audio_clip_t *clip; const char *text; } ai_request_t;
typedef struct { uint32_t turn; char *transcript, *text; /* reply or translation, PSRAM, app frees */
                 char lang[8], src_lang[8], finish[16], model[64]; double cost; bool truncated; } ai_result_t;
typedef struct { char id[65], name[49]; float usd_in_m, usd_out_m, usd_audio_m, est_turn_usd; bool mandatory; char min_effort[12]; char expires[11]; } ai_model_t;
typedef struct { bool has_remaining; float remaining, usage_day, usage_month, usage_total; bool free_tier; char reset[12]; } ai_key_info_t;

esp_err_t ai_client_init(void);                     /* "ai" task (PSRAM stack) + ai_q(4) */
esp_err_t ai_client_submit(const ai_request_t *r);  /* non-blocking; results via app_post() */
void      ai_client_cancel(uint32_t turn);
void      ai_client_history_clear(void);
esp_err_t ai_client_prewarm(void);                  /* on PTT down with no live socket: GET {ai_url}/key */
esp_err_t ai_client_list_models(int page, ai_model_t *out, int max, int *n, int *total);   /* protocol §7.1 */
esp_err_t ai_client_check_model(const char *id, ai_model_t *out);    /* GET /model/{id}: 404 → picker */
esp_err_t ai_client_key_info(ai_key_info_t *out);                     /* GET /key, protocol §8 */
size_t    ai_client_last_request_redacted(char *out, size_t len);    /* prefix and suffix only: no audio, no key */
```

**v1 is non-streaming** (protocol §2.4).

**Request bodies.** Use the exact bodies in protocol §2.1 (assistant) and §3 (translator):

- `"models":[primary, fallback]` **without** `"model"` when a fallback is set and the host is OpenRouter. Otherwise send `"model"` only.
- `response_format` json_schema with `strict:true`. The assistant schema is `{transcript, reply, lang}`. The translator schema is `{src_lang ∈ [A, B, "other"], transcript, translation}`, with properties in that order.
- `reasoning:{effort, exclude:true}` with the effort chosen by the §2.1 rule.
- `max_tokens` 1024 (assistant) or 768 (translator).
- `stream:false`.
- `provider:{require_parameters:true}`.
- **Never send** `temperature`, `provider.sort`, `stream_options` or `transforms`.
- For a non-OpenRouter base URL, drop `models`, `provider` and the `reasoning` object.
- The last user message is `[{"type":"text","text":"Голосовое сообщение пользователя."}, {"type":"input_audio","input_audio":{"data":<b64 WAV>,"format":"wav"}}]`, or a plain string for typed input.

**Building the body.** cJSON builds it with a random placeholder for the base64 (protocol §1.4). `cJSON_PrintUnformatted` is split at the placeholder; the prefix and suffix (≤ 12 KB) live in PSRAM. Then `or_begin(len = pre + b64_enc_len(44+2·n) + suf)`, `or_write_all(prefix)`, `or_write_b64_wav`, `or_write_all(suffix)`. The clip is trimmed with `audio_clip_trim()` first.

**History** (protocol §4.3): a PSRAM ring of up to 6 exchanges and 3000 code points. It is never written to NVS. It is reset on mode change, model change, idle beyond `hist_idle_min`, or a `context_length_exceeded` error.

**Parsing** (protocol §2.3):

- A `status != 200`, or a root `error` even with 200, is an error. Classify it with `or_classify`.
- `finish_reason`:
  - `stop`: OK.
  - `length`: salvage `reply` and mark it `truncated`.
  - `content_filter`: error P5.
  - `error`: error path.
- Defensively strip code fences and keep the first `{` to the last `}`, then `cJSON_Parse`.
- An empty `transcript` gives P4 ("Не расслышал"), with no TTS and no history entry.
- A parse failure is retried once, then reported as P2.
- `usage.cost` is added to the RAM session counter.

**v1.1 (`ai_stream=1`).**

- Add `"stream":true` and `Accept: text/event-stream`.
- The `sse.c` framing follows protocol §2.7:
  - comment lines starting with `:` reset the stall timer;
  - `[DONE]` ends the stream;
  - a mid-stream `error` object or `finish_reason:"error"` is a failure;
  - the usage frame is accounting only;
  - lines are capped at 32 KB.
- `jf.c` decodes the `reply`/`translation` string incrementally (`\" \\ \/ \b \f \n \r \t \uXXXX` including surrogates) into the TTS splitter, so TTS can start before the reply ends.
- This adds `EV_AI_TRANSCRIPT` and `EV_AI_DELTA`. It needs its own connection for TTS, because chat and TTS then overlap.

### 1.11 `tts_client` and `stt_client`

```c
esp_err_t tts_client_init(void);                    /* "tts" task (PSRAM stack) + tts_q(16) */
esp_err_t tts_client_speak(uint32_t turn, const char *text, const char *lang);   /* sanitise → split → route → enqueue */
void      tts_client_cancel(uint32_t turn);
esp_err_t tts_route(const char *lang, char *model, size_t mlen, char *voice, size_t vlen, int attempt);  /* §5.5/§5.6, vo_<lang> override */
uint32_t  tts_rate_for(const char *model, const char *content_type, uint8_t *channels);   /* Content-Type first, then table */
esp_err_t tts_client_fetch(const char *text, const char *lang, int16_t **pcm, size_t *samples, uint32_t *rate);  /* dev: test fixtures */

esp_err_t stt_client_transcribe(uint32_t turn, const audio_clip_t *clip, const char *lang /* NULL = auto */,
                                char *text, size_t text_len);
```

**TTS request:** `POST {tts_url}/audio/speech` with `{"model","input","voice","response_format":"pcm"}`, plus `"speed"` only when it is not 1.0. The rules come from protocol §5:

- **Sanitise:** strip `* _ # \` ~ > |`, emoji and URLs.
- **Split** (§5.4): chunk 1 is at most 200 code points; later chunks are packed greedily up to 400 and emitted at 150 or more; no piece is shorter than 20; there is an abbreviation list. The text is capped at 1500 characters per turn.
- **Route** (§5.5/§5.6): fallback routes on 400, 403 or 404. A failed route is remembered in RAM for 1 h.

**TTS response:**

- `Content-Type` is captured in `HTTP_EVENT_ON_HEADER` and handled per media type:
  - `audio/pcm` or `audio/l16`: rate and channels from the parameters, or from the table.
  - WAV: parse `fmt `.
  - `audio/mpeg`: error T2.
  - JSON: error body.
- The body is read in 4 KB chunks with a 1-byte carry, and only whole frames are written with `audio_out_write(turn, …, pdMS_TO_TICKS(2000))`. TCP backpressure throttles the server.
- When chunk *k* has been fully received, chunk *k+1* is requested on the same connection while *k* plays.
- The first PCM byte of a turn posts `EV_TTS_FIRST_AUDIO` with the TTFB. After the last chunk, `audio_out_end()` is called.

**STT:** follows protocol §6.

- JSON `{"model","input_audio":{"data","format":"wav"},"language","temperature":0}`.
- `language` is the UI language in assistant mode and is omitted in translator mode.
- The text then goes to chat as a plain-string user message.

### 1.12 `app` (state machine, §2.3)

```c
typedef enum { APP_IDLE, APP_LISTENING, APP_UPLOADING, APP_THINKING, APP_SPEAKING, APP_ERROR } app_state_t;
typedef enum { EV_PTT_DOWN, EV_PTT_UP /* i = held_ms */, EV_CANCEL, EV_TEXT_SUBMIT, EV_REC_LIMIT, EV_REC_SILENCE,
               EV_AI_UPLOAD_DONE, EV_AI_RETRY /* i = n */, EV_AI_RESULT /* p = ai_result_t* */, EV_AI_ERROR /* p = or_err_t* */,
               EV_AI_RELEASED, EV_AI_TRANSCRIPT, EV_AI_DELTA /* v1.1 */, EV_TTS_FIRST_AUDIO, EV_TTS_ERROR, EV_AUDIO_DRAINED,
               EV_NET_UP, EV_NET_DOWN, EV_PORTAL_ON, EV_PORTAL_OFF, EV_SET_MODE, EV_SET_TEXT_ONLY, EV_SETTINGS_CHANGED,
               EV_SAVE_SETTING /* from PSRAM-stack tasks */, EV_TICK } app_evt_type_t;
typedef struct { app_evt_type_t type; uint32_t turn; int32_t i; void *p; /* PSRAM; receiver frees, even when dropped */ } app_evt_t;
typedef struct { app_state_t state; uint8_t mode; bool text_only, latched; uint32_t turn; char last_err[8];
                 uint32_t t_rec_ms, t_upload_ms, t_ai_ms, t_tts_ttfb_ms, t_total_ms; char tts_voice[64]; uint32_t tts_rate; } app_status_t;
esp_err_t   app_start(void);
bool        app_post(const app_evt_t *e, TickType_t wait);
app_state_t app_state(void);
void        app_status(app_status_t *out);
```

### 1.13 `ui`

```c
typedef enum { UI_SET_STATE, UI_SET_MODE, UI_USER_MSG, UI_BOT_MSG, UI_BOT_APPEND, UI_TOAST, UI_ERROR,
               UI_SET_NET, UI_PORTAL, UI_GOTO, UI_SET_TEXT_ONLY, UI_RETRY } ui_msg_type_t;
typedef struct { ui_msg_type_t type; int32_t i; char *s; /* PSRAM, ui frees */ } ui_msg_t;
esp_err_t ui_init(lv_display_t *disp);   /* theme, fonts, screens, subjects, ui_pump (20 ms) and level (33 ms) timers */
void      ui_post(const ui_msg_t *m);    /* any task, non-blocking, ui_q depth 32 */
bool      ui_lock(uint32_t ms);  void ui_unlock(void);   /* lvgl_port_lock/unlock; never timeout 0 after boot */
esp_err_t ui_snapshot(const char *obj_name, lv_draw_buf_t *dst);  /* in taskLVGL via lv_async_call: lv_snapshot_take_to_draw_buf(obj|screen, RGB565, dst) */
lv_obj_t *ui_find(const char *name);     /* lv_obj_find_by_name(lv_screen_active() then lv_layer_top(), name) */
int       ui_tree_json(char *out, size_t len);    /* named objects: name,type,x,y,w,h,radius,flags,text */
int       ui_audit_json(char *out, size_t len);   /* ui_geom rules below */
```

**Thread-safety rules:**

1. Only taskLVGL touches widgets and subjects. Other tasks use `ui_post()`.
2. The console reads (`ui tree/get/audit/find`) under `ui_lock(1000)`.
3. Rendering requests (`shot`, `shot snap`) run inside taskLVGL through `lv_async_call`.
4. Never hold the lock while sleeping or waiting for injection.
5. The mic and TTS level meters are atomics read by a 33 ms `lv_timer`.
6. Subjects (`subj_state`, `subj_mode`, `subj_net`, `subj_text_only`) change only inside `ui_pump`. They are wrapped in `ui_subject.h`, because LVGL 9.6 replaces `lv_subject_init_*` with `lv_subject_create/delete`.

**Geometry rules** (`ui_geom.h`, from ui_calm.md §1.1). `ui audit` enforces them:

- No `clip_corner` on any screen. In LVGL 9.5 it forces ARGB8888 layers plus a mask over the whole screen.
- Every visible named object has **Rmax ≤ 168**:
  - a circle: centre distance + r;
  - a rounded rectangle: the maximum over its corner-arc centres + rc;
  - otherwise: the farthest bbox corner.
- Every clickable object is **≥ 50 × 50**, counting `ext_click_area`.
- Objects that tests use get `lv_obj_set_name()` (`CONFIG_LV_USE_OBJ_NAME`).
- Toasts may live on `lv_layer_top()`. The default `shot` captures every layer.

**Baseline home layout.** The selected UI concept may restyle it but must keep the names. Every row below has been checked against Rmax ≤ 168 and for overlaps.

| Name | Geometry | Rmax | Role |
|---|---|---|---|
| `home_orb` | Ø128, centre (180,150) | 94 | PTT: press = start, release < 300 ms = latch, release ≥ 300 ms = send; while busy, tap = cancel, hold = barge-in |
| `home_ring` | 184×184, centre (180,150); 32 bars r 70–92 drawn in `LV_EVENT_DRAW_MAIN_BEGIN` | 122 | mic / TTS level, not clickable |
| `home_state` | `lv_arclabel` r = 152, 64° span centred at the top, 16 px font | 160 | idle / listening / … |
| `home_mode` | Ø52, centre (72,92) | 165 | assistant ↔ translator |
| `home_settings` | Ø52, centre (288,92) | 165 | |
| `home_voice` | Ø52, centre (66,232) | 151 | text-only toggle |
| `home_chat` | Ø52, centre (294,232) | 151 | chat history |
| `home_text` | x 80–280, y 262–310, 2 lines of 20 px font | 164 | last transcript or reply |

**Keyboard.** `scr_kb` uses `lv_keyboard` (a buttonmatrix) with `USER_1..4` maps for RU, the stock maps for EN, and `NUMBER`.

- Our own `VALUE_CHANGED` handler catches the page, language and shift keys and forwards everything else to `lv_keyboard_def_event_cb`.
- The geometry comes from the chosen UI spec. ui_calm D6 specifies 5 tapered rows of 51–53 px keys over alphabetical half-pages.
- Keys must be ≥ 50 px and meet Rmax ≤ 168. Popovers stay off on the top and edge rows, because they extend past the widget and would be clipped by the bezel.
- `ui kb` (§7) resolves characters through `lv_buttonmatrix_private.h` (`map_p`, `button_areas[]`, `btn_cnt`), offset by the object's content coordinates. It then injects real taps, including page and language switches. This ties the code to LVGL 9.5, which is pinned.

**Fonts:**

- `lv_font_conv` 4 bpp from Montserrat, ranges `0x20-0x7E,0xA0-0xFF,0x401,0x410-0x44F,0x451,0x2010-0x2027,0x2116`, with FontAwesome symbols merged in.
- Sizes 16/20/28/40, about 170 KB in total.
- `--lv-fallback lv_font_montserrat_14` only rescues ASCII and symbols. `ui_str.c` therefore sanitises every model string: it strips markdown and drops emoji and code points outside the ranges. `LV_USE_FONT_PLACEHOLDER` stays on as a last resort.

### 1.14 `devconsole` (§7) and `ota` (§8)

```c
esp_err_t devconsole_start(void);   /* usb_serial_jtag_driver_install({.rx_buffer_size=1024,.tx_buffer_size=2048})
                                       + usb_serial_jtag_vfs_use_driver() + esp_log_set_vprintf(devcon_vprintf)
                                       + esp_console_init({.max_cmdline_length=1100,.max_cmdline_args=16,.heap_alloc_caps=MALLOC_CAP_SPIRAM})
                                       + task "devcon" (internal stack, it writes NVS and OTA) */
esp_err_t devconsole_register(const esp_console_cmd_t *cmd);
void      devcon_ok(int id, const char *json);   void devcon_err(int id, esp_err_t e, const char *json);
void      devcon_event(const char *fmt, ...);    /* "@! EVT {...}" */

typedef struct { char running[8], next[8]; esp_ota_img_states_t state; char ver[32], rb_reason[64]; bool rollback_possible; } ota_info_t;
void      ota_get_info(ota_info_t *o);
esp_err_t ota_selftest_and_confirm(uint32_t budget_ms);
esp_err_t ota_stream_begin(size_t size);   /* esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, &h): erases sector by sector */
esp_err_t ota_stream_write(const void *p, size_t n);
esp_err_t ota_stream_end(bool set_boot);   /* esp_ota_end + esp_ota_set_boot_partition */
esp_err_t ota_start_https(const char *url);   /* esp_https_ota with the cert bundle; task "ota", internal stack; EVT progress */
esp_err_t ota_set_boot(const char *label);    /* "ota_0"/"ota_1" + esp_restart */
esp_err_t ota_rollback_now(void);             /* esp_ota_check_rollback_is_possible() → esp_ota_mark_app_invalid_rollback_and_reboot() */
```

---

## 2. FreeRTOS tasks, message flow, state machine

### 2.1 Task list

`CONFIG_FREERTOS_HZ=1000`. Stacks are in internal RAM unless marked PSRAM.

| Task | Owner | Core | Prio | Stack | Blocks on |
|---|---|---|---|---|---|
| `main` | IDF | 0 | 1 | 8192 | Boot sequence. Returns after self-test, which frees the stack |
| `taskLVGL` | esp_lvgl_port | 1 | 4 | 8192 | `xEventGroupWaitBits`, waking at the next lv_timer (16 ms while the indevs poll) |
| `audio_in` | audio | 1 | 7 | 4096 | `i2s_channel_read`, 20 ms chunks (200 ms timeout); idle-reading keeps the level meter alive |
| `audio_out` | audio | 1 | 6 | 4096 | `xStreamBufferReceive(20 ms)` → `i2s_channel_write` |
| `app` | app | 1 | 5 | 6144 | `xQueueReceive(app_q[24], next deadline)` |
| `ai` | ai | 0 | 5 | 10240 **PSRAM** | `xQueueReceive(ai_q[4])`; socket I/O with 1 s ticks. Never writes flash |
| `tts` | ai | 0 | 5 | 8192 **PSRAM** | `xQueueReceive(tts_q[16])`; socket read; stream-buffer send. Never writes flash |
| `net` | net | 0 | 4 | 4096 | `xQueueReceive(net_q[8], backoff)`; saves networks to NVS |
| `devcon` | devconsole | 0 | 3 | 6144 | `usb_serial_jtag_read_bytes(…, 100 ms)`; writes NVS and OTA |
| `ota` (on demand) | ota | 0 | 4 | 8192 | `esp_https_ota_perform` |
| `httpd` (portal only) | esp_http_server | 0 | 5 | 6144 | `select()` |
| `dns_server` (portal only) | dns_server | 0 | 5 | 4096 | `recvfrom()` |
| `wifi` | IDF | 0 | 23 | ≈6.5 K | driver |
| `tiT` (lwIP) | IDF | 0 | 18 | 3072 | mbox |
| `sys_evt` | IDF | any | 20 | 4096 | default event loop (Wi-Fi and IP events only) |
| `esp_timer` | IDF | 0 | 22 | 3584 | button callbacks (post only), LVGL tick |
| `ipc0/1`, `IDLE0/1`, `Tmr Svc` | IDF | 0/1 | 24/0/1 | 1280/1536/2048 | – |

**Placement.**

- Core 1 runs the UI and real-time audio. Audio priority sits above LVGL. The DMA covers 80 ms of TX at 24 kHz (8×240) and 90 ms of RX at 16 kHz (6×240).
- Core 0 runs Wi-Fi, TLS, the console and OTA.
- PSRAM stacks use `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)` (`CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM` defaults to y on the S3). Those tasks must never write flash, and settings writers enforce that (§1.7).
- `CONFIG_I2S_ISR_IRAM_SAFE=y` keeps the I2S ISR running while flash is being written. Underruns then play as silence instead of a repeated DMA buffer.

### 2.2 Message flow

```
BOOT/PWR (esp_timer) ─┐
LVGL widgets ─────────┼─ EV_PTT_* / EV_CANCEL / EV_TEXT_SUBMIT / EV_SET_* ─▶ app_q ─▶ [app]
devconsole ───────────┘                                                      │
esp_event (WIFI/IP) ─▶ [net] ─▶ EV_NET_UP/DOWN, EV_PORTAL_ON/OFF ─▶ app_q    │
                                                                             ├─▶ audio_in_start/stop/abort ─▶ [audio_in]
[audio_in] ── EV_REC_LIMIT / EV_REC_SILENCE ─▶ app_q ; level → atomic ─▶ ui level timer (30 Hz)
                                                                             ├─▶ ai_client_submit ─▶ ai_q ─▶ [ai] ─HTTPS─▶ /chat/completions
[ai] ── EV_AI_UPLOAD_DONE / RETRY / RESULT / ERROR / RELEASED ─▶ app_q          (or_conn per host, mutex)
                                                                             ├─▶ tts_client_speak ─▶ tts_q ─▶ [tts] ─HTTPS─▶ /audio/speech
[tts] ── PCM ─▶ StreamBuffer 2 MB (PSRAM) ─▶ [audio_out] ─▶ I2S0 DMA ─▶ PCM5101
[tts] ── EV_TTS_FIRST_AUDIO / EV_TTS_ERROR ─▶ app_q ; [audio_out] ── EV_AUDIO_DRAINED ─▶ app_q
[app] ── ui_post() ─▶ ui_q ─▶ ui_pump (lv_timer 20 ms in taskLVGL) ─▶ widgets + subjects
[app] ── devcon_event("state", …) ─▶ USB
```

- **Event group `g_sys`:** `NET_UP`, `UI_READY`, `AUDIO_READY`, `SELFTEST_DONE`, `PORTAL_ON`, `TIME_OK`.
- **`esp_event` carries only IDF system events.** App events use `app_q`, which keeps them ordered and defines payload ownership: the sender allocates in PSRAM and the receiver frees, even when it drops the event.
- **Turn ids.** Every AI, TTS and audio event carries a `turn`. Cancel increments `cur_turn`, so anything still in flight from the old turn is dropped (its payload is still freed).
- **Buffer lending.** A clip buffer stays lent to `ai` until `EV_AI_RELEASED`, which `ai` posts after its last read of the PCM, whether the request succeeded, failed or was cancelled. `app` then calls `audio_in_release()`.

### 2.3 App state machine

The orthogonal variables are `mode`, `text_only`, `cur_turn`, `latched`, `barge` and `tts_left`.

"Ready" means `NET_UP`, `ai_key` set and the portal off. The PTT sources are the BOOT button, `home_orb` and the console `ptt` command.

| State | Event | Guard | Action | Next |
|---|---|---|---|---|
| IDLE / ERROR | PTT_DOWN | ready | `turn++`; `net_set_low_latency(1)`; `ai_client_prewarm()`; `audio_in_start(turn, max_rec_s·1000, vad)`; `latched=0`, `barge=0` | LISTENING |
| IDLE | PTT_DOWN | not ready | `UI_ERROR`: N0 "Нет Wi-Fi…", "Нет ключа…" or "Закройте настройку сети" | ERROR |
| IDLE | TEXT_SUBMIT(s) | ready | `turn++`; `ai_client_submit(text)` | THINKING |
| LISTENING | PTT_UP, held < 300 ms | `!latched && !barge` | `latched=1` (keep recording; UI shows "нажмите, чтобы отправить") | LISTENING |
| LISTENING | PTT_UP, held < 300 ms | `barge` | `audio_in_abort()` (a tap while busy means cancel only) | IDLE |
| LISTENING | PTT_UP held ≥ 300 ms, or PTT_DOWN when `latched`, or REC_LIMIT, or REC_SILENCE | clip ≥ 400 ms after trim | `audio_in_stop` → clip; `ai_client_submit(clip)`, with STT first if `stt_mode=1` | UPLOADING |
| LISTENING | same | clip < 400 ms | release the buffer; toast "Слишком коротко" | IDLE |
| LISTENING | CANCEL, or deadline `max_rec_s + 2 s` | – | `audio_in_abort()` | IDLE |
| UPLOADING | AI_UPLOAD_DONE | – | UI "thinking" | THINKING |
| UPLOADING / THINKING | AI_RETRY(n) | – | `UI_RETRY` "Повтор n/2…" | same |
| UPLOADING / THINKING | AI_RESULT | `text_only`, empty text, or P4 | user bubble + bot bubble or P4 message; finish | IDLE |
| UPLOADING / THINKING | AI_RESULT | voice on | bubbles; voice = `lang` (assistant) or target language (translator, honouring `tr_other`); `tts_client_speak()` | THINKING |
| THINKING | TTS_FIRST_AUDIO | – | UI "speaking" | SPEAKING |
| SPEAKING | AUDIO_DRAINED | last chunk was queued | finish | IDLE |
| THINKING / SPEAKING | TTS_ERROR (T1/T2) | – | `audio_out_stop()`; toast "Озвучка недоступна"; text stays | IDLE |
| UPLOADING / THINKING / SPEAKING | PTT_DOWN | – | cancel the turn (below); then as from IDLE with `barge=1` | LISTENING |
| UPLOADING / THINKING / SPEAKING | CANCEL | – | cancel the turn | IDLE |
| any busy state | AI_ERROR(e) | – | cancel the turn; `UI_ERROR(e.msg_ru)` | ERROR |
| any busy state | NET_DOWN or PORTAL_ON | – | cancel the turn; N0 or portal message | ERROR |
| any busy state | deadline (UPLOADING + THINKING 45 s total for chat; THINKING 15 s without TTS first audio; SPEAKING 60 s per chunk) | – | cancel the turn; N8 | ERROR |
| ERROR | 4 s, CANCEL or a tap | – | – | IDLE |
| any | SET_MODE / SET_TEXT_ONLY | – | cancel if busy; apply; clear history on a mode switch | IDLE |
| any | SAVE_SETTING(k, v) | – | `settings_set_*` (app has an internal stack) | same |

**Cancel turn** runs:

1. `ai_client_cancel(t)`
2. `tts_client_cancel(t)`
3. `audio_out_stop()`
4. `audio_in_abort()`
5. `net_set_low_latency(0)`
6. `cur_turn++`

The UI switches at once (0 ms perceived). Workers notice within 1 s, or within the 10 s connect timeout.

**Physical input mapping:**

| Input | Action |
|---|---|
| BOOT down | `PTT_DOWN` (a second press within 300 ms of a tap's release: Assistant ⇄ Translator, ui.md 2.3) |
| BOOT up | `PTT_UP(held_ms)` |
| PWR click | lock / unlock (`ui_pwr_short()` → `UI_LOCK`; locking ends the turn through `UI_IN_LOCK`); on a dark, unlocked screen only a wake |
| PWR long (3 s, armed) | `board_power_off()` after `EVT power_off` |

---

## 3. Memory budget

### 3.1 Internal SRAM

**Ground truth.** In the M2 build, DIRAM totals **341,760 B** with a 16 KB I-cache and a 32 KB D-cache. Static use is **79,534 B**.

**Projection to M6.**

- Static use: +30 KB for Wi-Fi, lwIP, mbedTLS/PSA, the HTTP client and server, NVS, and our code, with the Wi-Fi IRAM optimisations **off**. That gives ≈ 110 KB.
- Startup allocations: main stack 8 KB, IPC, idle, esp_timer and timer-service stacks, and heap metadata, ≈ 22 KB.
- Plus 8 KB of RTC FAST heap.
- **Free internal heap at `app_main` entry ≈ 212 KB**, and +8 KB once `app_main` returns *(measure M3: `info.heap_stages`)*.

The draft assumed 300 KB. That number is not reachable on this chip with this configuration.

**Steady state.** STA connected, a voice turn in progress, TTS playing, no portal.

| Consumer | Internal KB | Notes |
|---|---|---|
| LVGL draw buffers 2 × 360×40×2 B | 56.3 | DMA-capable, from the reserve pool |
| taskLVGL stack | 8.0 | |
| Our internal stacks: app 6, audio_in 4, audio_out 4, net 4, devcon 6 | 24.0 | `ai` and `tts` stacks are in PSRAM |
| Our TCBs, queues, event groups, semaphores | 4.0 | FreeRTOS objects are always internal |
| IDF runtime tasks: wifi ≈6.5, tiT 3, sys_evt 4 | 13.5 | IPC, idle, esp_timer and timer service are counted at startup |
| I2S DMA: TX 8×240×4 = 7.5, RX 6×240×4 = 5.6, scratch 2.2, descriptors | 15.5 | |
| USB-Serial-JTAG driver rings, RX 1 KB + TX 2 KB | 3.0 | Flow-controlled, so a small RX ring loses nothing |
| Wi-Fi, PHY and supplicant internal state | 30.0 | *(measure M3)* Static and dynamic RX/TX buffers go to PSRAM through `TRY_ALLOCATE_WIFI_LWIP` |
| lwIP, netif and esp_event internals | 6.0 | pbufs are in PSRAM |
| mbedTLS/PSA internal leftovers | 3.0 | `EXTERNAL_MEM_ALLOC` + `DYNAMIC_BUFFER` |
| NVS handles and page cache | 3.0 | The settings cache is in PSRAM |
| Mallocs of 1 KB or less | 12.0 | `ALWAYSINTERNAL=1024`; cJSON is hooked to PSRAM |
| **Total** | **≈ 179** | |
| **Headroom (212 + 8 − 179)** | **≈ 41** | |

- **Portal on** (turns blocked): +6 httpd stack, +4 dns stack, +5 for the AP netif, DHCP server and 2 sockets. Headroom ≈ 26 KB.
- **Gates checked by tests:**
  - idle with STA up: `int.free ≥ 40 KB`, `int.largest ≥ 16 KB`;
  - during a turn: `int.free ≥ 28 KB`;
  - `int.min_ever ≥ 20 KB`;
  - `dma.free ≥ 8 KB`.

  After the M3 measurement, tighten each gate to the measured value minus 16 KB.
- **Recovery ladder** if a gate is missed, applied in order:
  1. `CONFIG_RA_LVGL_BUF_LINES=30` (+14 KB; still above LVGL's 1/10-screen minimum).
  2. Shrink stacks to measured high-water mark + 1 KB, using `tasks` (≈ +4 KB).
  3. If the M3 heap delta around `esp_wifi_init` shows the static RX buffers landed internally: `ESP_WIFI_STATIC_RX_BUFFER_NUM=8` (+13 KB).
  4. Fold the `net` task into esp_event handlers plus an `esp_timer` backoff (+4 KB).
  5. xiaozhi's small Wi-Fi profile: static RX 3, dynamic RX 6, RX BA window 3.
- **Spending list** if the measured headroom exceeds 70 KB, applied in order: `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y` (≈ −10 KB, faster rendering); `CONFIG_ESP_WIFI_RX_IRAM_OPT=y` (≈ −17 KB); `CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y` (−16 KB SRAM, used by xiaozhi).
- **Why the reserve pool helps.** `SPIRAM_MALLOC_RESERVE_INTERNAL=98304` keeps 96 KB that only explicit `MALLOC_CAP_INTERNAL|DMA` requests can use, such as stacks, DMA buffers and Wi-Fi internals. Plain `malloc()` of 1 KB or less then falls back to PSRAM instead of starving DMA.

### 3.2 PSRAM

8,192 KB, minus about 30 KB of external BSS for lwIP, net80211 and pp (`SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`), leaves ≈ 8,160 KB.

| Consumer | KB | When |
|---|---|---|
| LVGL TLSF pool (`LV_MEM_POOL_ALLOC`) | 1,024 | boot |
| Clip buffers 2 × 960,000 B | 1,875 | `audio_in_init` |
| TTS ring | 2,048 | first `audio_out_begin` |
| Capture shadow 259,200 B | 253 | first `shot` |
| Mic inject buffer, ≤ 30 s | 938 | dev only, after `mic load` |
| Wi-Fi static TX 16 + static RX 16 + dynamic RX ≤ 32 + TX cache 32, at 1.6 KB each | ≈ 154 | worst case |
| lwIP pbufs and socket buffers (23 KB send buffer × 2 sockets) | ≈ 60 | |
| mbedTLS, 2 sessions at the handshake peak (dynamic buffers) | ≈ 80 | normally 1 session (one connection per host) |
| esp_http_client buffers, 2 × (4 + 2) | 12 | |
| Chat body prefix and suffix ≤ 12, response cap 64, history ≤ 12, TTS chunk queue 8 | ≈ 96 | per turn |
| Model picker: one 10-entry page (≤ 21 KB) + cJSON tree + 160 B × entries | ≈ 70 | transient, never during a turn |
| `ai` and `tts` stacks, 10 + 8 | 18 | boot |
| OTA buffers, settings cache, cJSON scratch | ≈ 40 | |
| CJK glyph bitmaps (font draw buffers, ui.md 1.3) | ≤ 310 | grows with use, 608 glyphs at most; the cache nodes (≤ 160 KB) are inside the LVGL pool; the 5 MB font is mapped flash, no RAM |
| **Total, worst case including the inject buffer** | **≈ 6,980** | |
| **Headroom** | **≈ 1.18 MB** (≈ 2.1 MB without the inject buffer) | gate: `psram.free ≥ 1 MB` |

`CONFIG_SPIRAM_XIP_FROM_PSRAM` stays off. It would move about 1.8 MB of code and rodata into PSRAM.

### 3.3 Flash and upload size

- **App size.** M2 is 731,610 B. M6 is projected at 1.8–2.2 MB:
  - Wi-Fi, TLS, HTTP client and server: ≈ 0.75 MB;
  - fonts: ≈ 0.17 MB;
  - FULL cert bundle: ≈ 0.065 MB;
  - our code: ≈ 0.25 MB.

  That is 55 % or less of a 4 MB slot. `tools/build.ps1` fails above 85 %.
- **Measured with the CJK fallback (feat-cjk):** the app is 2,003,328 B (48 % of the slot; Tiny TTF, stb_truetype and `ui_font.c` add 28 KB to main's 1,974,880 B). The CJK font stays out of the app on purpose: the 5,066,752 B asset pack (§1.0) is written once to `storage` and never travels with an OTA image.
- **Upload size.** A 30 s clip is 960,044 B of WAV and 1,280,060 base64 characters. A 10 s clip is 426,728 characters.
  - At 400 KB/s TLS upload *(measure M4)*, 10 s of speech uploads in ≈ 1.1 s.
  - AAC through `esp_audio_codec` (`format:"aac"`) is a v2 option if the upload dominates latency.

---

## 4. `sdkconfig.defaults`

Every symbol below was checked against the Kconfig files of IDF v6.1 (`C:\esp\v6.1\esp-idf`), LVGL 9.5.0, esp_lcd_touch 1.2.1 and esp_lcd_touch_cst816s 1.1.2 in `managed_components/`, and our `main/Kconfig.projbuild`.

**`sdkconfig` is tracked in git today, which hides edits to the defaults:** an existing `sdkconfig` is not overridden by defaults. In M3.0, `git rm --cached sdkconfig`, add it to `.gitignore`, then `Remove-Item sdkconfig; .\tools\idf.ps1 reconfigure`. Release archives keep a copy of the resolved `sdkconfig`.

```
# ---- Target, flash, CPU, caches, compiler
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
# 16 KB I-cache on purpose: the 32 KB option costs 16 KB of internal SRAM (see 3.1 spending list)
CONFIG_ESP32S3_INSTRUCTION_CACHE_16KB=y
CONFIG_ESP32S3_DATA_CACHE_32KB=y
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y
CONFIG_COMPILER_OPTIMIZATION_PERF=y
CONFIG_FREERTOS_HZ=1000

# ---- Partitions, bootloader, rollback, crash dumps
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=n
CONFIG_BOOTLOADER_LOG_LEVEL_WARN=y
CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y

# ---- Octal PSRAM
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MEMTEST=n
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=98304
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y

# ---- Console, logging, system
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESP_CONSOLE_SECONDARY_NONE=y
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y
CONFIG_LOG_COLORS=n
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096
CONFIG_ESP_TASK_WDT_TIMEOUT_S=10
CONFIG_FREERTOS_USE_TRACE_FACILITY=y
CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y
CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS=y
CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID=y
CONFIG_BT_ENABLED=n

# ---- Audio: keep the I2S ISR alive during flash writes (underrun = silence)
CONFIG_I2S_ISR_IRAM_SAFE=y

# ---- Wi-Fi: buffers in PSRAM; with TRY_ALLOCATE the TX buffer type is forced to Static,
# ---- so DYNAMIC_TX_BUFFER_NUM does not apply. IRAM optimisations off (as xiaozhi) to save internal SRAM.
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=16
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=32
CONFIG_ESP_WIFI_STATIC_TX_BUFFER=y
CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=16
CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM=32
CONFIG_ESP_WIFI_RX_BA_WIN=16
CONFIG_ESP_WIFI_TX_BA_WIN=6
CONFIG_ESP_WIFI_DYNAMIC_RX_MGMT_BUFFER=y
CONFIG_ESP_WIFI_IRAM_OPT=n
CONFIG_ESP_WIFI_RX_IRAM_OPT=n
CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=n
CONFIG_ESP_WIFI_NVS_ENABLED=n
CONFIG_LWIP_MAX_SOCKETS=16
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=23040
CONFIG_LWIP_TCP_WND_DEFAULT=11520
CONFIG_LWIP_TCP_RECVMBOX_SIZE=16
CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y

# ---- TLS: FULL bundle because every endpoint has its own user-set base URL
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y
CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA=y
CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE=n
CONFIG_MBEDTLS_SSL_RENEGOTIATION=n

# ---- Captive portal HTTP server (phones send large headers)
CONFIG_HTTPD_MAX_REQ_HDR_LEN=2048
CONFIG_HTTPD_MAX_URI_LEN=1024

# ---- Touch
CONFIG_ESP_LCD_TOUCH_MAX_POINTS=1
CONFIG_ESP_LCD_TOUCH_CST816S_DISABLE_READ_ID=y

# ---- LVGL 9.5 (heap = built-in TLSF, pool placed in PSRAM by main/CMakeLists.txt)
CONFIG_LV_COLOR_DEPTH_16=y
CONFIG_LV_USE_BUILTIN_MALLOC=y
CONFIG_LV_MEM_SIZE_KILOBYTES=1024
CONFIG_LV_USE_CLIB_STRING=y
CONFIG_LV_USE_CLIB_SPRINTF=y
CONFIG_LV_OS_NONE=y
CONFIG_LV_DEF_REFR_PERIOD=16
CONFIG_LV_DRAW_SW_COMPLEX=y
CONFIG_LV_USE_DRAW_SW_COMPLEX_GRADIENTS=y
CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=n
CONFIG_LV_USE_LOG=y
CONFIG_LV_LOG_LEVEL_WARN=y
CONFIG_LV_LOG_PRINTF=n
CONFIG_LV_USE_OBJ_NAME=y
CONFIG_LV_USE_OBSERVER=y
CONFIG_LV_USE_SNAPSHOT=y
CONFIG_LV_USE_QRCODE=y
CONFIG_LV_USE_SYSMON=y
CONFIG_LV_USE_PERF_MONITOR=y
CONFIG_LV_USE_PERF_MONITOR_LOG_MODE=y
CONFIG_LV_USE_MEM_MONITOR=n
CONFIG_LV_FONT_MONTSERRAT_14=y
CONFIG_LV_FONT_MONTSERRAT_20=y
CONFIG_LV_FONT_MONTSERRAT_28=y
CONFIG_LV_FONT_DEFAULT_MONTSERRAT_14=y
CONFIG_LV_USE_FONT_COMPRESSED=n
CONFIG_LV_USE_FONT_PLACEHOLDER=y
CONFIG_LV_TXT_ENC_UTF8=y
CONFIG_LV_USE_THEME_DEFAULT=y
CONFIG_LV_THEME_DEFAULT_DARK=y
CONFIG_LV_USE_FLEX=y
CONFIG_LV_USE_ARC=y
CONFIG_LV_USE_ARCLABEL=y
CONFIG_LV_USE_BAR=y
CONFIG_LV_USE_BUTTON=y
CONFIG_LV_USE_BUTTONMATRIX=y
CONFIG_LV_USE_CANVAS=y
CONFIG_LV_USE_IMAGE=y
CONFIG_LV_USE_KEYBOARD=y
CONFIG_LV_USE_LABEL=y
CONFIG_LV_USE_LINE=y
CONFIG_LV_USE_ROLLER=y
CONFIG_LV_USE_SCALE=y
CONFIG_LV_USE_SLIDER=y
CONFIG_LV_USE_SPINNER=y
CONFIG_LV_USE_SWITCH=y
CONFIG_LV_USE_TEXTAREA=y
CONFIG_LV_USE_ANIMIMG=n
CONFIG_LV_USE_CALENDAR=n
CONFIG_LV_USE_CHART=n
CONFIG_LV_USE_CHECKBOX=n
CONFIG_LV_USE_DROPDOWN=n
CONFIG_LV_USE_IMAGEBUTTON=n
CONFIG_LV_USE_LED=n
CONFIG_LV_USE_LIST=n
CONFIG_LV_USE_LOTTIE=n
CONFIG_LV_USE_MENU=n
CONFIG_LV_USE_MSGBOX=n
CONFIG_LV_USE_SPAN=n
CONFIG_LV_USE_SPINBOX=n
CONFIG_LV_USE_TABLE=n
CONFIG_LV_USE_TABVIEW=n
CONFIG_LV_USE_TILEVIEW=n
CONFIG_LV_USE_WIN=n
CONFIG_LV_BUILD_EXAMPLES=n
CONFIG_LV_BUILD_DEMOS=n

# ---- Project (main/Kconfig.projbuild)
CONFIG_RA_LCD_PCLK_MHZ=80
CONFIG_RA_LVGL_BUF_LINES=40
CONFIG_RA_LCD_EVEN_ROUNDER=y
CONFIG_RA_DEVCONSOLE=y
CONFIG_RA_SELFTEST_FORCE_FAIL=n
CONFIG_RA_SELFTEST_FORCE_PANIC=n
CONFIG_RA_HTTP_REFERER=""
```

**Notes on the configuration:**

- **Kconfig.projbuild.** `RA_LCD_PCLK_MHZ` is an int with range 10..80; `RA_LVGL_BUF_LINES` is an int with range 20..90; the others are bool or string.
- **Montserrat 20 and 28** stay until `ui_test.c` stops using them (M6).
- **`LV_USE_MEM_MONITOR=n`.** The memory overlay would appear in every flush-shadow screenshot. `heap` calls `lv_mem_monitor()` directly, which needs only the built-in malloc.
- **Sysmon.** `LV_USE_PERF_MONITOR_LOG_MODE=y` *(proven)* draws no overlay. At boot `ui_init` calls `lv_sysmon_performance_pause(NULL)`, because `hide` does not stop log mode. `perf on` resumes it (one log line per second).
- **LVGL logging** goes through `lv_log_register_print_cb()` → `ESP_LOGW("LVGL", …)`, which keeps console lines intact.

**`sdkconfig.defaults.release`:**

```
CONFIG_LV_USE_SYSMON=n
CONFIG_LV_USE_PERF_MONITOR=n
CONFIG_LOG_MAXIMUM_LEVEL_INFO=y
CONFIG_RA_DEVCONSOLE=y
```

The console stays in release builds because the tests and the rollback runbook depend on it.

Build the release with `-B build-rel -D SDKCONFIG=build-rel/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.release"`.

**Test overlays** (M6):

- `tests/sdkconfig.forcefail` contains `CONFIG_RA_SELFTEST_FORCE_FAIL=y`.
- `tests/sdkconfig.forcepanic` contains `CONFIG_RA_SELFTEST_FORCE_PANIC=y`.
- Each is built into its own `-B` directory with `SDKCONFIG_DEFAULTS="sdkconfig.defaults;tests/sdkconfig.force…"`.

---

## 5. `partitions.csv`

This is the repo's current table. It has been unchanged since M1, and it is valid.

```
# Name,   Type, SubType,  Offset,   Size
nvs,      data, nvs,      0x9000,   0x6000
otadata,  data, ota,      0xf000,   0x2000
phy_init, data, phy,      0x11000,  0x1000
ota_0,    app,  ota_0,    0x20000,  0x400000
ota_1,    app,  ota_1,    0x420000, 0x400000
# fonts, UI sounds, cached assets
storage,  data, spiffs,   0x820000, 0x7c0000
coredump, data, coredump, 0xfe0000, 0x20000
```

**Checks:**

- **Bootloader:** 22,672 B today; it must stay below 0x8000. The table sits at 0x8000.
- **Alignment:** both app slots are 64 KB aligned and 4 MB (the requirement is ≥ 3 MB). otadata is exactly 0x2000. 0x12000–0x1FFFF is padding.
- **End of flash:** 0x820000 + 0x7C0000 = 0xFE0000, and 0xFE0000 + 0x20000 = 0x1000000.
- **Comments** are whole lines that start with `#`.
- **NVS** has 6 pages, one kept free for garbage collection (§1.7 capacity).
- **Coredump:** 128 KB holds an ELF dump of about 20 tasks.

**Never change this table again.** A change needs a full flash and can relocate NVS.

The `storage` row keeps its `spiffs` subtype, but nothing mounts SPIFFS: the asset pack (§1.0) is found by the label `storage` and memory-mapped, so the subtype is only a name.

---

## 6. `main/idf_component.yml` and CMake glue

```yaml
## Exact pins. They satisfy the decided ranges ^2.0.2 / ~1.1.2 / ~2.0.3 / ~2.9 / 9.5.x.
## Bump one at a time, rebuild, run tests, commit dependencies.lock (registry-only deps → lock is committed).
dependencies:
  idf:
    version: ">=6.1.0,<6.2.0"
  espressif/esp_lcd_st77916: "==2.0.2"
  espressif/esp_lcd_touch: "==1.2.1"
  espressif/esp_lcd_touch_cst816s: "==1.1.2"
  espressif/esp_io_expander_tca9554: "==2.0.3"
  espressif/esp_lvgl_port: "==2.9.0"
  lvgl/lvgl: "==9.5.0"
  espressif/cjson: "==1.7.19~2"
  ## 4.2.2 was published 2026-10-14; 4.2.1 is the version xiaozhi's ~4.2.1 has been built against
  espressif/button: "==4.2.1"
  ## later, only if AAC upload or MP3 TTS is needed:
  ## espressif/esp_audio_codec: "==2.6.2"
```

- The current `dependencies.lock` already resolves lvgl 9.5.0, esp_lvgl_port 2.9.0, esp_lcd_st77916 2.0.2, esp_lcd_touch 1.2.1, cst816s 1.1.2, tca9554 2.0.3, esp_io_expander 1.2.1 and cmake_utilities 0.5.3.
- Adding cjson and button extends the lock. Commit it.
- cJSON is **not** part of IDF v6.1: there is no `json` component in `components/`.

**Project `CMakeLists.txt`:**

```cmake
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(round_assistant)   # PROJECT_VER = git describe --tags --dirty
```

**`main/CMakeLists.txt` tail.** This places the LVGL pool in PSRAM, the same mechanism xiaozhi uses (verified in its `main/CMakeLists.txt`):

```cmake
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
    target_include_directories(${lvgl_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/ui")
    target_compile_definitions(${lvgl_lib} PRIVATE "LV_MEM_POOL_INCLUDE=\"lvgl_psram_pool.h\"")
endif()
```

`main/ui/lvgl_psram_pool.h`:

```c
#pragma once
#include "esp_heap_caps.h"
#define LV_MEM_POOL_ALLOC(size) heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
```

LVGL 9.5 `lv_conf_internal.h` keeps an externally defined `LV_MEM_POOL_INCLUDE` when `LV_MEM_ADR == 0`. `lv_mem_core_builtin.c` then includes it and calls `lv_tlsf_create_with_pool(LV_MEM_POOL_ALLOC(LV_MEM_SIZE), …)`. The self-test checks that the pool total equals 1,048,576.

**`.gitignore` additions:** `sdkconfig`, `build-rel/`, `build-ff/`, `build-fp/`, `releases/`, `tests/out/`.

---

## 7. Dev console protocol v2 (USB-Serial-JTAG, COM7)

### 7.1 Transport

- CDC-ACM, so the baud rate is ignored. UTF-8 lines end in LF; CR LF is accepted. Command lines are ≤ 1024 B and data lines ≤ 4200 B.
- The device reads with `usb_serial_jtag_read_bytes`, not linenoise. There is no echo and no prompt, which removes the `ra>` and echo handling from `dev.py`.
- One writer mutex makes all output line-atomic. `esp_log_set_vprintf()` routes ESP_LOG through the same writer.
- When `!usb_serial_jtag_is_connected()`, writes time out after 50 ms and are dropped, so a device on battery never stalls.
- The PC opens the port with `dtr=False, rts=False` set **before** `open()` *(proven in `dev.py`)*. It reconnects automatically after a re-enumeration on reboot.

### 7.2 Framing

```
PC  → [#<id> ]<command> [args…]           id 1..65535; no id = id 0 (human typing in idf.py monitor)
                                          args split by esp_console_split_argv (double quotes, \ escapes)
DEV → @<id> OK <json>                     single-line compact JSON
DEV → @<id> ERR <esp_err_name> {"msg":"…"}
DEV → @<id> BEGIN <kind> {"w":360,"h":360,"fmt":"rgb565le","bytes":259200,"crc32":"1a2b3c4d"}
DEV → @<id> D <seq> <base64>              3072 raw bytes → 4096 chars per line
DEV → @<id> END {"bytes":259200,"crc32":"1a2b3c4d"}   (followed by @<id> OK {...})
DEV → @! EVT {"t":12345,"ev":"state","from":"thinking","to":"speaking","turn":7}
DEV → @! READY {"ver":"m2-3-g1a2b3c","reset":"poweron","boot":42,"part":"ota_0","boot_ms":930}
Any line not starting with '@' is a log line: shown, ignored by parsers.
```

**Uploads** (`mic load`, `ota push`):

1. The PC sends `#9 mic load bytes=96044 crc32=…`.
2. The device answers `@9 READY {"chunk":3072,"window":8}`.
3. The PC sends `D <seq> <b64>` lines and waits for `@9 ACK <seq>` after every 8 lines.
4. The PC sends `END`. The device verifies the CRC and answers `@9 OK {…}`.

Any other line, `ABORT`, or 5 s of silence aborts the upload with `ERR`.

**CRC.** CRC-32 is IEEE (`esp_rom_crc32_le(0, …)`, equal to `zlib.crc32`). `crc 123456789` must return `cbf43926`, and the PC checks this on connect.

### 7.3 Commands

Names already used by the M1/M2 console are kept.

| Command | Purpose / response |
|---|---|
| `help`, `ping` | `{"uptime_ms":…}` |
| `info` | `ver, idf, part, ota_state, reset, boot_ms, panel_id "00027f7f", panel_batch, pclk_mhz, touch_seen, touch_id, exio_ok, psram_kb, flash_mb, mac, gpio7, bl, selftest, safe_mode, heap_stages{after_board,after_display,after_audio,after_wifi_init,after_connect}` |
| `state` | `{"state","mode","text_only","latched","turn","net":{state,ssid,rssi,ip,portal},"ai":{rec_ms,upload_ms,ai_ms,total_ms,last_err,model,cost_session},"tts":{voice,rate,ttfb_ms,chunks,underruns},"ui":{"screen"}}` |
| `heap` | `int:{free,min,largest}, dma:{free,largest}, psram:{free,min,largest}, lvgl:{total,free,biggest,frag_pct}` (`heap_caps_*`, `lv_mem_monitor`) |
| `tasks` | `uxTaskGetSystemState` → `name, core, prio, state, hwm_bytes, stack_caps, cpu_pct` |
| `perf [on\|off\|dump [reset]]` | `display_stats` JSON (`fps_x10`, `render_us_avg/max`, `flush_us_max`); on/off = `lv_sysmon_performance_resume/pause` (log lines) |
| `shot [x y w h]` | Flush-shadow capture of exactly what the panel receives, all layers, RGB565LE native order, stride w·2, top-left first → BEGIN/D/END |
| `shot snap [name]` | `lv_snapshot_take_to_draw_buf` of the active screen or a named object, in taskLVGL |
| `tap x y [ms=80]`, `press x y`, `move x y`, `release`, `swipe x0 y0 x1 y1 [ms=300] [steps=15]`, `longpress x y [ms=800]` | `inject_*` → `@id OK {}` once the queue has drained |
| `touch watch <s>` (`touchlog 0\|1` kept as an alias) | Streams `@! EVT {"ev":"touch","x":…,"y":…,"p":1}` from the real panel |
| `ui screens`, `ui goto <scr>`, `ui tree`, `ui get <name> <text\|value\|visible\|checked\|scroll_y\|x\|y\|w\|h>`, `ui tap <name>`, `ui text <name> "<utf8>"`, `ui kb "<utf8>"`, `ui audit` | `ui tap` injects at the object's centre from `lv_obj_get_coords`; `ui kb` taps keys (§1.13); `ui audit` returns the violations |
| `bl <0-100>`, `invert <0\|1>` | backlight / panel inversion |
| `btn boot\|pwr down\|up\|click\|double\|long` | Simulated physical buttons through the same callbacks |
| `get <key>`, `set <key> <value>`, `settings [json]`, `settings reset [keep_wifi]` | Secrets come back masked |
| `mic probe [ms]` | `{"right":{"rms_dbfs":-48.1,"zero":0.00,"distinct":3117},"left":{…}}` |
| `mic rec <ms>`, `mic dump`, `mic level`, `mic fmt right\|left <shift>` | Record from the current source / download the last clip as WAV (BEGIN/D/END) |
| `mic load bytes=N crc32=X`, `mic src mic\|inject [fast]`, `mic play` | Upload a WAV (16 kHz mono s16) / select the source / run a full turn (PTT_DOWN, PTT_UP at the end of the buffer) |
| `spk tone <hz> <ms> [amp] [rate]`, `spk stat [reset]`, `spk vol <n>` | audio_out tests |
| `ptt down\|up [held_ms]\|tap`, `cancel`, `mode assistant\|translator`, `textonly 0\|1`, `ask "<text>"`, `say "<text>" [lang]` | App events / text turn / TTS only |
| `tts fetch "<text>" [lang]` | Synthesises through the configured TTS and returns the PCM as a WAV blob (test fixtures, §9) |
| `ai last-req`, `ai models [page]`, `ai model-check [id]`, `ai key` | Redacted request prefix / picker data / `/model` check / `/key` summary |
| `wifi scan`, `wifi connect <ssid> <pass>`, `wifi disconnect`, `wifi forget <ssid>`, `wifi status` | net |
| `portal start\|stop\|status` | `status` → `{"on":1,"ssid":"RoundAI-6E68","qr":"WIFI:T:WPA;S:…;P:…;;"}`. The password appears only in dev builds |
| `net get <https-url>` | HTTPS smoke test: `{"status":200,"ms":…,"bytes":…,"tls_ms":…}` |
| `ota info`, `ota push bytes=N crc32=X`, `ota url <https-url>`, `ota boot ota_0\|ota_1`, `ota rollback`, `ota mark valid\|invalid` | §8 |
| `log <tag\|*> <none\|error\|warn\|info\|debug>` | `esp_log_level_set` |
| `crc <text>` | CRC self-check |
| `reboot [dl]` | `esp_restart()`. `dl` first writes `RTC_CNTL_FORCE_DOWNLOAD_BOOT` into `RTC_CNTL_OPTION1_REG`, which enters ROM download mode without BOOT |

**Events emitted:** `boot, ready, selftest, state, screen, ui_click(name), touch, rec(ms,rms), ai(turn,phase,ms), retry(n), tts(phase,rate,ttfb,chunk), audio_drained, net, portal, error(id,msg), ota(pct), heap_low, power_off`.

### 7.4 PC side

`tools/dev.py` keeps its name and moves to protocol v2. It uses pyserial 3.5, which is already in the IDF venv, so nothing extra is installed.

- `Device(port)` runs a reader thread that routes lines into per-id queues and an event list.
- Methods: `cmd(line, timeout) → dict`, `blob(line) → bytes`, `shot(path, mask=True)`, `upload(kind, data)`, `wait_event(pred, timeout)`, `wav_prepare(path)` (stdlib `wave`, downmix, linear resample to 16 kHz).
- The PNG writer uses only the stdlib (`zlib`, `struct`), as it does today. RGB565 expands to RGB888 as `(c<<3)|(c>>2)` and `(c<<2)|(c>>4)`. `--mask` dims or clears pixels outside r = 180.
- Golden screenshots are stored as `zlib`-compressed raw RGB565. They are compared as raw arrays, so no imaging library is needed.

---

## 8. Boot sequence, self-test, rollback runbook

### 8.1 Boot

Targets: UI visible within 1.5 s, self-test verdict within 3 s.

0. **Bootloader:** the hook sets GPIO7 = 1. Then the rollback logic runs: an image still in `PENDING_VERIFY` at boot becomes `ABORTED`, and the other slot runs.
1. **`app_main`:**
   - `board_power_latch()`.
   - Record `esp_reset_reason()`, `esp_app_get_description()`, the running partition and `esp_ota_get_state_partition()`.
   - `RTC_NOINIT_ATTR` crash counter: +1 when the reset reason is PANIC/INT_WDT/TASK_WDT and the previous boot never reached `READY`.
   - At 3 or more, and only when the image is not `PENDING_VERIFY`, enter **safe mode**: devconsole plus a minimal UI, with no Wi-Fi, audio or AI.
2. `util_init()`, `settings_init()`, `devconsole_start()`. The console comes this early so that a device with a later failure is still reachable. Commands report `ERR ESP_ERR_INVALID_STATE "not ready"` until their subsystem is up.
3. `board_init()` → `display_init()` (reset pulse, probe, init, LVGL) → `touch_init()` → `inject_init()` → `ui_init()` (boot screen). The backlight fades in after the first frame. Record `heap_stages`.
4. `audio_out_init()` (ring allocated lazily) and `audio_in_init()`.
5. `net_init()`: netifs, event loop and `esp_wifi_init()`. Wi-Fi is not started yet.
6. `ota_selftest_and_confirm(3000)` (§8.2), under a 10 s `esp_timer` guard. If the self-test hangs, the guard calls `esp_restart()`, which leads to rollback.
7. `net_start()` (non-blocking), `ai_client_init()`, `tts_client_init()`, `app_start()`, `ui_post(UI_GOTO home)`, `@! READY`. Then `app_main` returns and its 8 KB stack is freed.

### 8.2 Checks before `esp_ota_mark_app_valid_cancel_rollback()`

| Check | Fatal |
|---|---|
| NVS: write `sys/boot_cnt` and read it back | yes |
| TCA9554 input register readable (`board_exio_ok()`) | yes |
| ST77916 init returned ESP_OK | yes |
| Panel ID read OK and a known pattern | no (warn) |
| LVGL alive: ≥ 1 `LV_EVENT_REFR_READY` and ≥ 10 ticks of a 20 ms lv_timer within 500 ms | yes |
| LVGL pool total = 1,048,576 B, located in PSRAM | yes |
| I2S0 and I2S1 channels created and enabled | yes |
| CST816 handle created (the ID read is disabled) | no (warn; BOOT and the console still work) |
| `esp_wifi_init` OK (association **not** required) | yes |
| Internal free ≥ 40 KB, internal largest ≥ 16 KB, DMA free ≥ 12 KB, PSRAM free ≥ 3 MB at this point | yes. Projection at step 6 is ≈ 58 KB internal and ≈ 4.9 MB PSRAM; tighten after M3 |
| devconsole task running | yes |
| `CONFIG_RA_SELFTEST_FORCE_FAIL` / `FORCE_PANIC` | test hooks |

- **Pass:** if `PENDING_VERIFY`, call `esp_ota_mark_app_valid_cancel_rollback()`. Store `sys/ok_ver` and emit `EVT selftest ok`.
- **Fail:** store `sys/rb_reason`. If `PENDING_VERIFY`, call `esp_ota_mark_app_invalid_rollback_and_reboot()`. Otherwise (a USB-flashed image) show the failure and stay up for debugging.
- **Soft stage 2,** within 60 s of boot: if credentials exist and the STA associates, but 3 HTTPS attempts to `ai_url` fail, the UI offers "Откатить прошивку?". The button calls `ota_rollback_now()`, which works because the previous slot stays intact until the next OTA.

### 8.3 Developer rollback runbook

| Situation | Action |
|---|---|
| Milestone done | `tools\release.ps1 -Tag m2`. It requires a clean tree and runs `git tag -a m2`. It archives `releases\m2\` with `bootloader\bootloader.bin`, `partition_table\partition-table.bin`, `ota_data_initial.bin`, `round_assistant.bin/.elf/.map`, `flash_args` (relative paths kept as in `build\`), `sdkconfig`, `dependencies.lock` and `SHA256SUMS.txt`. The existing tag is `m1`; later tags are `m2` … `m6` and `v1.0.0` |
| Back to a tag (source) | `git switch --detach m1` → `.\tools\idf.ps1 build` → `.\tools\idf.ps1 -p COM7 flash` |
| Back to a tag (binary, no build) | `.\tools\restore.ps1 -Tag m1`, which runs `esptool --chip esp32s3 -p COM7 write-flash "@flash_args"` inside `releases\m1\`. NVS is preserved |
| A/B try a build without losing the current one | `.\tools\slot.ps1 -Write build\round_assistant.bin -Slot 1 -Boot`, which runs `otatool.py -p COM7 write_ota_partition --slot 1 --input …`, then `erase_otadata`, then `switch_ota_partition --slot 1`. Back: `.\tools\slot.ps1 -Boot 0` |
| Exercise the real OTA and rollback path over USB | `python tools\dev.py ota-push build\round_assistant.bin`. It uses the esp_ota API, so the image boots in `PENDING_VERIFY` |
| Settings corrupt | `dev.py cmd "settings reset"`, or `parttool.py -p COM7 erase_partition --partition-name=nvs` |
| Crash analysis | `.\tools\idf.ps1 -p COM7 coredump-info` (128 KB coredump partition) |
| App broken, USB still enumerates | `.\tools\idf.ps1 -p COM7 flash` (esptool resets through USB-JTAG), or `dev.py cmd "reboot dl"` |
| Nothing responds, or a boot loop | Download mode: **hold BOOT, tap RESET, release BOOT**. The ROM USB-Serial-JTAG appears (VID 0x303A). Then `flash` or `restore.ps1` |
| Return to factory firmware | `esptool --chip esp32s3 -p COM7 write-flash 0 backups\factory_full_16MB.bin` (SHA-256 `90151CB92E6D224D02B19A6956D8EC9AD5F7ADE0008EE24E97012210F8802AE9`). This replaces the partition table and NVS. Come back with a full `flash` plus `secrets_push.py` |

**Notes on OTA state:**

- **`flash` vs `app-flash`.** `idf.py flash` writes `ota_data_initial.bin` at 0xF000, so the board always boots `ota_0` afterwards. `app-flash` writes only `ota_0`; if otadata currently selects `ota_1`, an `app-flash` has no visible effect. `idf.py erase-otadata` resets the selection to `ota_0`.
- **`otatool switch_ota_partition`** (verified in v6.1 `otatool.py`) writes only the sequence number (offset 0) and its CRC (offset 28) into the other otadata copy. It keeps that copy's stale `ota_state` word.
  - 0xFFFFFFFF (UNDEFINED) counts as valid, so manual switches have no rollback protection.
  - A stale ABORTED or INVALID state makes the bootloader ignore the switch. That is why `slot.ps1` runs `erase_otadata` first.
- **USB-flashed images are never `PENDING_VERIFY`.** Bootloader rollback only guards images written through `esp_ota_*`.
- **Never burn eFuses:** no flash encryption, no secure boot, no HMAC NVS encryption. That keeps every state above recoverable.

---

## 9. Windows build and flash workflow (EIM, `C:\esp`, v6.1)

### 9.1 Facts verified on this PC

- EIM is `C:\esp\tools-eim\eim.exe`. IDF is `C:\esp\v6.1\esp-idf`.
- `C:\Espressif\tools\eim_idf.json` lists the install `name: "v6.1"` with `activationScript: C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1` and Python `C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe`.
- `eim_config.toml` sets `activation_script_path_override = 'C:\Espressif\tools'`. `Microsoft.v6.1.PowerShell_deactivate.ps1` sits next to the profile.
- **What the activation profile does:**
  - sets `IDF_PATH`, `IDF_TOOLS_PATH`, `IDF_PYTHON_ENV_PATH`, `IDF_CCACHE_ENABLE=1` and `PATH`;
  - defines the global functions `esptool`, `espefuse`, `espsecure`, `otatool.py` and `parttool.py`, plus the alias `idf.py` → `Invoke-idfpy`;
  - activates the venv, which contains pyserial 3.5, esptool 5.6.0, esp-coredump 1.17.2 and esp-idf-monitor 1.10.0;
  - registers tab completion, which runs `idf.py` once, about 1–2 s;
  - runs `eim select v6.1`, which writes `eim_idf.json`.
- `-e` prints the environment without activating anything.
- **`eim run <COMMAND> [IDF_VERSION]`** sources that version's activation script and runs one command in the current directory (EIM CLI docs). `eim shell [IDF_VERSION]` opens an interactive PowerShell (`-NoExit`).

### 9.2 Activation

```powershell
# interactive PowerShell session
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
# non-interactive one-shot (agent, CI, Git Bash). tools\idf.ps1 dot-sources the profile quietly and removes
# MSYSTEM, which makes idf.py refuse to run under MSYS/MinGW
powershell -NoProfile -ExecutionPolicy Bypass -File <repo>\tools\idf.ps1 build
# EIM alternative
Set-Location <repo>; & 'C:\esp\tools-eim\eim.exe' run "idf.py build" v6.1
```

### 9.3 Commands, from the repo root

```powershell
git rm --cached sdkconfig; Remove-Item .\sdkconfig; .\tools\idf.ps1 reconfigure   # once in M3.0, then after every defaults edit
.\tools\idf.ps1 build
.\tools\idf.ps1 size; .\tools\idf.ps1 size-components
.\tools\idf.ps1 -p COM7 flash                       # bootloader + table + otadata + ota_0
.\tools\idf.ps1 -p COM7 app-flash                   # ota_0 only (see the 8.3 note)
.\tools\idf.ps1 -p COM7 storage-flash               # asset pack build/storage.bin (CJK font, 5 MB) at 0x820000, only when it changes
.\tools\idf.ps1 -p COM7 monitor                     # humans only: needs a TTY; Ctrl+] exits
.\tools\py.ps1 tools\dev.py --port COM7 log 20      # agent-friendly log capture
```

Only one program can hold COM7 at a time. Close `monitor` or `dev.py` before flashing. `-b` is irrelevant on USB-Serial-JTAG.

### 9.4 Helper scripts

| Script | Behaviour |
|---|---|
| `tools\idf.ps1` (exists) | Dot-sources the profile with `*> $null`, sets `$ErrorActionPreference='Continue'`, removes `Env:MSYSTEM`, runs `idf.py @args` from the repo root, and returns its exit code |
| `tools\py.ps1` | `& 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' @args` |
| `tools\build.ps1 [-Release] [-Variant forcefail\|forcepanic]` | Builds into `build\`, `build-rel\`, `build-ff\` or `build-fp\` with the defaults chain. Parses `idf.py size` and fails above 85 % of 0x400000 |
| `tools\flash.ps1 [-Port COM7] [-AppOnly \| -Storage]` | Flashes (`-Storage`: the asset pack, `idf.py storage-flash`), then runs `dev.py wait-ready --timeout 10`, then prints `info` |
| `tools\release.ps1 -Tag <t>` | Requires a clean tree; `git tag -a`; release build; archive to `releases\<t>\` with SHA256SUMS |
| `tools\restore.ps1 -Tag <t> \| -Factory [-EraseNvs]` | esptool `write-flash "@flash_args"` from the archive, or the factory image; optionally `parttool.py erase_partition --partition-name=nvs` |
| `tools\slot.ps1 -Write <bin> -Slot 0\|1 [-Boot]` / `-Boot 0\|1` / `-Info` | otatool `write_ota_partition`; `erase_otadata` + `switch_ota_partition`; `read_otadata` |
| `tools\dev.py` (exists, moves to v2) | Library and CLI: `cmd`, `shot out.png [--mask]`, `snap name out.png`, `tap`, `swipe`, `ui-tap name`, `wav in.wav`, `mic-dump out.wav`, `events --until state=idle`, `log <s>`, `ota-push bin`, `wait-ready`, `reset` (`esptool --after hard-reset chip-id`) |
| `tools\secrets_push.py secrets.json` | `{"wifi":[{"ssid","pass"}],"ai_key":"…"}` → `set` / `wifi connect`. The file is gitignored and never printed |
| `tools\mkaudio.py` | Creates test fixtures with no microphone: `tts fetch "<text>" ru\|en` → 24 kHz WAV → resample to 16 kHz with stdlib `audioop`-free linear interpolation → `tests\audio\*.wav` (committed) |
| `tools\mkfonts.ps1` | Runs `npx lv_font_conv@1.5.3 …` (Node is on PATH; the first run downloads the npm package, so run it manually) → `main\ui\fonts\*.c` (committed); then points `ui_16/20/28` at their CJK fallbacks (`.fallback = &ui_cjk_<px>`) |
| `tools\mkcjk.py [--download \| src.otf]` | fonttools (`py -m pip install fonttools==4.67.0`): Noto Sans CJK SC Medium 2.004 (SHA-256 checked) → subset, TrueType outlines → `assets\fonts\cjk\RACJK-Medium.ttf` (committed; same input, same bytes) |
| `tools\mkpack.py --font … --license … -o …` | Standard library only; run by the build (target `storage_pack`) → `build\storage.bin` (§1.0 layout). Without a build: `esptool --chip esp32s3 -p COM7 write-flash 0x820000 build\storage.bin` |
| `tests\acceptance.py -m m1..m6 [-p COM7]` | Runs §10 and writes `tests\out\<ts>\report.json` and screenshots |

---

## 10. Milestones and acceptance tests

All tests run through the dev console. "Finger" means a physical touch is needed, but nobody has to look at the screen. Exactly one test (1.10) needs a human to look, once, because the panel's own output cannot be read back.

### M1: display, touch, console, rollback skeleton

**Status:** done, tagged `m1`. Re-run after M3.0 with protocol v2.

| # | Test | Pass |
|---|---|---|
| 1.1 | `info` | `panel_id = 00027f7f` on this unit (`007f7f7f` is also accepted) and `panel_batch` matches; `pclk_mhz=80`; `exio_ok`; `gpio7=1`; `selftest=ok` |
| 1.2 | `ui goto test`, then `shot` | CRC OK. RGB565 at the centres of the four squares: (102,160)=0xF800, (154,160)=0x07E0, (206,160)=0x001F, (258,160)=0xFFFF. This proves capture and byte order |
| 1.3 | `ui goto test_anim` (orb Ø128 + 32-bar ring animating), 5 s, `perf dump` | `fps_x10 ≥ 300`, `render_us_max < 33000` *(measure; on failure record the value and open an issue)* |
| 1.4 | `ui goto test_touch` (9 named buttons), `ui tap tt_5` | `EVT ui_click tt_5`; `ui get tt_count text == "1"` |
| 1.5 | `swipe 180 300 180 80 300` on `tt_list` | `ui get tt_list scroll_y > 100` |
| 1.6 | `info.touch_seen`, finger: `touch watch 10` and tap the centre | true; the reported point is within ±20 px of (180,180), so orientation is correct; `touch_id` is non-zero |
| 1.7 | `heap`, `tasks` (no Wi-Fi yet) | `int.free ≥ 100 KB`, `dma.free ≥ 24 KB`, every task HWM ≥ 512 B |
| 1.8 | `reboot` × 5 | `@! READY` within 3 s each time; same `panel_id` |
| 1.9 | `ui audit` on the test screens | no violations |
| 1.10 | One-time human photo of `test` | Correct colours, no stripes. Otherwise toggle `invert`, set `RA_LCD_PCLK_MHZ=40`, or check the rounder |

### M2: audio in/out

**Status:** committed `e9f33c0`. Commit or drop the WIP, run these tests, then `release.ps1 -Tag m2`.

| # | Test | Pass |
|---|---|---|
| 2.1 | `mic probe 600` in a quiet room | The active slot (right, *proven*) has `rms_dbfs > −75` and `distinct > 200`. The other slot is constant (`zero ≥ 0.95` or `distinct ≤ 4`) **or** ≥ 15 dB lower; it may float, because the ICS-43434 tri-states. The script writes `mic_slot` |
| 2.2 | `mic rec 3000`, `mic dump` | WAV of 96,044 B, 16 kHz mono, CRC OK, not all zeros, `peak < 32767` |
| 2.3 | `wav tests/audio/ru_hello_16k.wav`, `mic src inject fast`, `mic rec 2000`, `mic dump` | Bit-exact match with the first 2000 ms of the injected samples (injection bypasses the shift and DC blocker) |
| 2.4 | `spk stat reset`, `spk tone 1000 2000 50`, wait 2.3 s, `spk stat` | `in_bytes = 96,000 ± 480`, `underruns = 0`, `write_errors = 0`, `rate = 24000` |
| 2.5 | Tones at rates 16000, 44100 and 24000 | `spk stat.rate` follows each one; no error |
| 2.6 | 30 s tone during `test_anim` | fps drop ≤ 20 %; underruns 0 |
| 2.7 | `heap` | PSRAM free has dropped by the clip buffers (≈ 1.83 MB); internal unchanged within ±4 KB |

### M3: restructure, Wi-Fi, settings, keyboard, portal

| # | Test | Pass |
|---|---|---|
| 3.0 | M3.0 restructure: re-run the M1 and M2 suites with protocol v2 and `sdkconfig` regenerated from the defaults | all pass; `info.heap_stages` recorded in the report |
| 3.1 | For every row of `settings json`: `set` boundary and out-of-range values, `get`, `reboot`, `get` | values persist; out-of-range → ERR; secrets masked, so a response never contains the full value |
| 3.2 | `wifi connect` (from `secrets.json`) | `EVT net up` within 15 s; `state.net.ip` set; rssi present |
| 3.3 | `wifi scan` | ≥ 1 AP |
| 3.4 | `net get https://openrouter.ai/api/v1/models?limit=1` | status 200 (TLS with the bundle); `tls_ms` logged |
| 3.5 | `ui goto kb_test`; `ui kb "привет"`; `ui kb "Hello 123"`; backspace | `ui get kb_ta text` equals the expected string |
| 3.6 | `set dev_flags 4`, `portal start`. From the PC: `GET http://<sta_ip>/`, then `POST /save ai_model=test/model` | 200 with the form; `get ai_model == test/model`; key unchanged when left blank |
| 3.7 | The PC sends a UDP DNS query for `example.com` to `<sta_ip>:53` (stdlib `socket`) | A record 192.168.4.1 |
| 3.8 | `portal status`; `ui get portal_qr visible` | `qr` matches `WIFI:T:WPA;S:RoundAI-[0-9A-F]{4};P:[0-9]{8};;`; visible = 1 |
| 3.9 | Memory gates: `heap` idle with STA up; `heap` after 20 × `net get`; `heap` with the portal on | `int.free ≥ 40 KB` and `largest ≥ 16 KB` idle; `int.min ≥ 24 KB`; portal on: `int.free ≥ 24 KB`. Record the measured `esp_wifi_init` delta and tighten the gates |
| 3.10 | `ptt down` while the portal is on | ERR state with the portal message; no network request made |

### M4: assistant AI and TTS

| # | Test | Pass |
|---|---|---|
| 4.0 | `ai model-check`, `ai key` | `ai_model` exists and has no `expiration_date`; `/key` parses. A 403 "region" stops the suite with a clear report (protocol §9.1) |
| 4.1 | `ask "Скажи одно слово: тест"` | `EVT ai done` within 15 s; non-empty reply; then idle |
| 4.2 | `mkaudio.py` fixture "Какая столица Франции?" → `wav`, `mic play` | Events listening → uploading → thinking → idle; transcript contains "столиц"; reply contains "Париж"; `lang = ru` |
| 4.3 | `state.ai` timings over 5 runs | upload < 3 s for a 3 s clip; `ai_ms < 6 s`; logged to CSV |
| 4.4 | `textonly 0`, `say "Проверка связи"` | `tts.rate = 24000`; `in_bytes ≥ 24 KB`; underruns 0; `EVT audio_drained` |
| 4.5 | `textonly 0`, `ask "Расскажи в трёх предложениях про Луну"` | `tts.chunks ≥ 2`, and `EVT tts first_audio` arrives before the request for the last chunk is sent (chunk pipelining in v1) |
| 4.6 | `cancel` right after `EVT uploading`, ×20 | idle within 500 ms; no event carries the old turn id; `int.free` within ±8 KB of baseline; `EVT ai released` every time |
| 4.7 | `ptt down` during speaking, then `ptt up 600` | listening within 300 ms; `spk stat.queued_bytes = 0` |
| 4.8 | `set ai_key sk-invalid` → `ask`; `set ai_model nonexistent/x` → `ask`; `wifi disconnect` → `ask` | Error ids 401 / 404 / N0; `ui get err_label text` non-empty; idle after 4 s |
| 4.9 | Inject a 30 s WAV, `mic play` | success, or 413 handled; no allocation failure; `psram.free ≥ 1 MB` |
| 4.10 | 3 turns, then `ui tree` + `ui audit` | 6 bubbles; 0 violations |
| 4.11 | Soak: 50 text turns | no reboot (uptime monotonic); `int.min ≥ 20 KB`; LVGL `frag_pct < 30` |

### M5: translator

| # | Test | Pass |
|---|---|---|
| 5.1 | `mode translator`, `set tr_lang_a ru`, `set tr_lang_b en`; inject "Доброе утро" | `EVT ai src_lang=ru`; translation contains "morning"; `state.tts.voice` starts with `en-` (voice on) |
| 5.2 | Inject "Where is the train station?" (fixture made with the en voice) | `src_lang = en`; translation contains "вокзал"; voice `ru-RU-…` |
| 5.3 | `set tr_lang_a de`, `ai last-req` | schema enum `["de","en","other"]`; no audio or key in the output |
| 5.4 | `ui tap tr_swap` | `tr_lang_a` and `tr_lang_b` are swapped in settings |
| 5.5 | `tr_vad 1`; WAV with 2 s of speech then 2 s of silence; `mic play` without `ptt up` | `EVT rec` stops on silence within 1.3 s of the end of speech |
| 5.6 | Tests 4.6–4.8 in translator mode | same pass criteria |
| 5.7 | Inject German speech with A = ru, B = en, `tr_other = 0` | `src_lang = other`; translated into ru; badge "Другой язык" visible |

### M6: polish and OTA

**Tags:** `m6`, `v1.0.0`.

| # | Test | Pass |
|---|---|---|
| 6.1 | `dev.py ota-push build\round_assistant.bin` | reboot; `ota info` running = `ota_1`; `EVT selftest ok`; state valid |
| 6.2 | Push a `forcefail` build | automatic rollback; running = previous slot; `rb_reason` set |
| 6.3 | Push a `forcepanic` build | bootloader rollback (ABORTED); previous slot runs |
| 6.4 | `ota url https://…/round_assistant.bin` | progress events to 100 %; boots and is marked valid |
| 6.5 | `ui audit` on every screen; golden `shot` per screen | 0 violations; pixel difference vs golden < 2 % |
| 6.6 | `info.boot_ms`; time to `EVT selftest` | < 1500 ms / < 3000 ms; home orb ≥ 30 fps |
| 6.7 | Flash the `m5` release over `m6` NVS, then back | settings readable both ways (`cfg_ver` rule: never erase) |
| 6.8 | `btn pwr long` | `EVT power_off` logged (the board stays on while powered from USB) |
| 6.9 | `slot.ps1 -Write build\round_assistant.bin -Slot 1 -Boot`, then `slot.ps1 -Boot 0` | `info.part` is `ota_1` (state undefined), then back to `ota_0` |

Every milestone ends with `tests\acceptance.py -m mN` all green, followed by `tools\release.ps1 -Tag mN`.

---

## Review changes

1. **Repo reality.** The draft ignored the M1/M2 code already in the repo. This revision adds the actual status: tag `m1`, M2 commit `e9f33c0`, the uncommitted `mic rate` work, the panel ID `00 02 7F 7F` on this unit, and the migration map to components (M3.0). The draft claimed an "untracked `sdkconfig` with 160 MHz/HZ 100". In fact `sdkconfig` is tracked and already has 240 MHz and HZ 1000. Untracking it is now an explicit step.
2. **Proven values kept** over draft guesses: TCA9554 reset 1/10/0/20/1/120 ms, with 300/300 as fallback; even rounder on by default; CST816 I2C at 400 kHz; `DISABLE_READ_ID=y`; LVGL priority 4 and `max_sleep` 100; `LV_DEF_REFR_PERIOD=16`; sysmon in log mode; Montserrat 20/28 kept for `ui_test`.
3. **Memory budget corrected.** Free internal RAM at `app_main` is ≈ 212 KB, not 300 KB, derived from the measured M2 map (DIRAM 341,760 B, static 79,534 B).
   - Dropped `ESP32S3_INSTRUCTION_CACHE_32KB` (it costs 16 KB of SRAM).
   - Wi-Fi IRAM optimisations off, as xiaozhi does.
   - `LV_ATTRIBUTE_FAST_MEM_USE_IRAM=n` by default.
   - `ai` and `tts` stacks moved to PSRAM, with a no-flash-write rule.
   - Smaller USB rings.
   - The portal blocks AI turns.
   - Measurable gates, `heap_stages`, a recovery ladder and a spending list.
4. **Dead Kconfig line removed.** With `SPIRAM_TRY_ALLOCATE_WIFI_LWIP`, Wi-Fi TX buffers are forced to Static, so `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` did nothing. It is replaced by `STATIC_TX_BUFFER(_NUM)`. Added `CONFIG_I2S_ISR_IRAM_SAFE=y`. `LV_USE_MEM_MONITOR=n`. Every remaining symbol was checked against the local v6.1 and component Kconfig files.
5. **LVGL heap.** The repo's CLIB malloc put small LVGL objects in internal RAM. This revision adopts xiaozhi's TLSF pool in PSRAM (`LV_MEM_POOL_INCLUDE`, verified in xiaozhi's CMakeLists and in LVGL 9.5 `lv_conf_internal.h`).
6. **Aligned with `docs/protocol.md`:**
   - v1 is non-streaming (`ai_stream=0`); SSE moves to v1.1.
   - `models` is sent without `model`; no `temperature`; no `provider.sort`.
   - `max_tokens` 1024/768.
   - The `lang` field is added; `src_lang` includes `"other"`; `tr_other` is added.
   - `reasoning.exclude` plus the per-model effort rule.
   - Protocol §5.4 split rules (200/400/150/20).
   - Per-phase timeout table; §9.1 error ids and retry policy.
   - One connection per host, used sequentially.
   - Attribution headers only for openrouter.ai; no `Accept-Encoding`.
   - The picker no longer uses the `supported_parameters` query filter, which returns flex-tier prices.
   - `/key` semantics; 2 MB TTS ring with prefetch.
   - `max_rec_s` default 20; silence trim.
   - `sys_prompt` stored as an NVS **blob**; `hist_idle_min`.
7. **Clarified that `keep_alive_enable` is TCP keepalive.** HTTP reuse depends on not calling `close()`, plus `set_url`/`open` again.
8. **Fixed the `esp_netif_dhcps_option` call:** it takes 5 arguments, needs the DHCP server stopped and restarted, and keeps the URI pointer (so the string is static).
9. **Self-test bugs fixed.** The draft checked `esp_wifi_init` before `net_init` ran; `net_init` now runs before the self-test. The draft required PSRAM free ≥ 6 MB, which is impossible after the 1 MB LVGL pool and the 1.83 MB clip buffers; it is now 3 MB. Internal thresholds now match the projection.
10. **Input semantics fixed:**
    - BOOT long-press 1.5 s = cancel collided with hold-to-talk.
    - Starting PTT only after 300 ms lost the first syllable; it now starts on press.
    - Double-click delayed single-click.
    - A PWR long-press during battery power-on would switch the board off; PWR is now armed only after its first release.
    - Unified behaviour for the orb, BOOT and the console: tap latches; a tap while busy cancels; a hold while busy barges in.
11. **Home layout overlaps fixed.** In the draft, `home_text` overlapped mode, voice and PTT, and the ring's bbox fell outside the safe circle. New coordinates are checked against ui_calm's Rmax ≤ 168 and 50 px target rules. The orb is the PTT.
12. **Touch path simplified to what is proven.** A TIMER-mode real indev plus a second injection indev with a timed sample queue replace the draft's merged EVENT-mode indev and extra poll timer. `touch_chip_id` is read lazily because the ID read is disabled.
13. **Screenshots.** The proven flush-shadow capture stays as `shot`. It shows exactly what the panel receives and includes every layer. `lv_snapshot` is added as `shot snap`. Both run in taskLVGL via `lv_async_call`, so the console stack never renders.
14. **Tests made checkable:**
    - Injection bypasses the DC blocker, so 2.3 can be bit-exact.
    - `bytes_played` is defined as s16 input bytes, as in the existing code (2.4).
    - The mic-probe criterion tolerates a floating unused slot.
    - `ui get portal_qr data` was impossible because `lv_qrcode` keeps no payload; it is replaced by `portal status`.
    - The SSE pipelining test (4.5) was invalid for v1; it now tests TTS chunk pipelining.
    - M1 tests use the existing colour squares.
    - Test fixtures come from `tts fetch` (no microphone or human needed).
    - Added 3.10, 4.0, 5.7 and 6.9.
15. **Component pins.** `espressif/button` is pinned to 4.2.1 (4.2.2 was published 3 days ago) and `cjson` to `1.7.19~2`, the current registry revision. Confirmed that IDF v6.1 ships no `json` component.
16. **SPIFFS** is mounted lazily and never auto-formatted. Formatting 7.75 MB takes tens of seconds.
17. **EIM details verified on disk.** The profile also runs `eim select v6.1` and registers tab completion; it has the `-e` switch. The `eim run "<cmd>" [version]` syntax matches the EIM docs. `idf.py erase-otadata/read-otadata` exist in v6.1. `storage-flash` is generated by `esp_partition_register_target`.
18. **OTA push** uses `OTA_WITH_SEQUENTIAL_WRITES` to erase incrementally. `ota_rollback_now` checks `esp_ota_check_rollback_is_possible()`.
19. **Thread-safety additions:**
    - Never hold the LVGL lock while waiting for injection.
    - Lock timeouts for console reads.
    - Settings writes are forbidden from PSRAM-stack tasks and go through `EV_SAVE_SETTING`.
    - `board_backlight_set` is mutex-protected, because the LEDC fade API is not thread-safe.
20. **Tags and tools.** Kept the existing tag scheme (`m1`, `m2`, …) and the existing `tools/dev.py` name. Protocol v2 accepts id-less lines for humans.
21. **Fonts.** Noted that the Montserrat-14 fallback has no Cyrillic. The sanitiser and placeholder are what actually cover missing glyphs.
22. **Deliberate deviation from protocol.md:** the FULL cert bundle instead of CMN, because every endpoint's base URL is user-configurable. Update protocol.md §1.3.
