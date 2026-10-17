# Firmware architecture: Round AI Assistant

Board: Waveshare ESP32-S3-Touch-LCD-1.85 rev 2.1 (ESP32-S3R8, 8 MB octal PSRAM, 16 MB flash, COM7).
Toolchain: ESP-IDF v6.1 (EIM, `C:\esp\v6.1\esp-idf`), C, LVGL 9.5.0 via esp_lvgl_port 2.9.0.
Repository: `C:\Users\krupo\esp32-ai-assistant`. This document replaces the `C:\dev\round-assistant` path used in the research. The existing repo path has no spaces and is ASCII only, so it already meets the IDF path rule.

Status: architecture for M1 to M6. Every value marked *(xiaozhi)* is copied from the proven xiaozhi board port, commit `d91d682`, mirrored in `third_party/xiaozhi_ref/`. Every value marked *(measure)* must be confirmed on the device at the milestone named.

---

## 0. Baseline from xiaozhi, and where we deviate

I read xiaozhi `main/boards/waveshare/esp32-s3-touch-lcd-1.85/` (`esp32-s3-touch-lcd-1.85.cc`, `config.h`, `config.json`), plus `sdkconfig.defaults`, `sdkconfig.defaults.esp32s3`, `main/idf_component.yml`, `main/display/lcd_display.cc` and `main/audio/codecs/no_audio_codec.cc`. `config.json` has an empty `sdkconfig_append`, so the board runs on the generic S3 defaults.

| Item | xiaozhi (proven on this board) | This firmware | Reason |
|---|---|---|---|
| I2C0 | `i2c_new_master_bus` port 0, SDA 11, SCL 10, `I2C_CLK_SRC_DEFAULT` | same, `glitch_ignore_cnt=7` | – |
| TCA9554 | `esp_io_expander_new_i2c_tca9554(bus, 0x20)`. P0\|P1 set as outputs and driven 1→0→1 with 300 ms steps (resets touch and LCD) | same pins. 1→0, hold 20 ms, →1, wait 150 ms. Falls back to 300/300 if the panel ID read fails | Faster boot |
| QSPI bus | SPI2_HOST, D0..D3 = 46/45/42/41, SCLK 40, `max_transfer_sz = 360*80*2`, `SPI_DMA_CH_AUTO` | same, `max_transfer_sz = 360*40*2` | Matches our draw buffer |
| Panel probe | IO at 3 MHz, `lcd_cmd_bits=32`, `lcd_param_bits=8`, `quad_mode=1`, `esp_lcd_panel_io_rx_param(io, (0x0B<<24)\|(0x04<<8), buf, 4)` | identical | – |
| Probe IO handle | never deleted (leaked) | `esp_lcd_panel_io_del()` before creating the fast IO | Frees the SPI device |
| Run clock | 80 MHz | `CONFIG_RA_LCD_PCLK_MHZ` default 80, fallback 40 | hardware.md gotcha 2 |
| Init table | `00 02 7F 7F` → `vendor_specific_init_new` (184 cmds; ends `0x21` INVON, `0x11` + 120 ms, `0x29`). Anything else → driver default | identical; table copied with the MIT notice | – |
| Panel cfg | `LCD_RGB_ELEMENT_ORDER_RGB`, 16 bpp, `reset_gpio_num = NC` (so the driver sends SWRESET) | identical | – |
| LVGL port | 360×20 single buffer, `buff_dma=1`, `buff_spiram=0`, `swap_bytes=1`, task prio 1 on core 1 | 360×40 **double** buffer (internal DMA), `swap_bytes=1`, prio 3, core 1, 8 KB stack | Smooth animation; LVGL asks for at least 1/10 of the screen |
| LVGL heap | builtin TLSF, `LV_MEM_SIZE_KILOBYTES=1024`, pool allocated in PSRAM through `LV_MEM_POOL_INCLUDE`/`LV_MEM_POOL_ALLOC` | same mechanism | Proven |
| Touch | **not initialised** (pins in `config.h` are unused) | CST816 on I2C1 (SDA 1 / SCL 3, internal pull-ups), INT GPIO4, reset on EXIO1, our own merged indev | Gap in xiaozhi |
| Speaker | `NoAudioCodecSimplex`: I2S port 0, 24 kHz, 32-bit, `I2S_SLOT_MODE_MONO` + `I2S_STD_SLOT_BOTH` (hardware duplicates mono to L/R), BCLK 48 / WS 38 / DOUT 47, DMA 6×240, Philips, no MCLK. Volume `(v/100)^2·65536` applied to int16 → int32 | same; rate reconfigured per TTS stream; DMA 8×240 | TTS rates vary (24 k / 44.1 k) |
| Mic | I2S port 1, 16 kHz, 32-bit, MONO, `I2S_STD_SLOT_RIGHT`, SCK 15 / WS 2 / DIN 39, `s32 >> 12` with clamp | same; slot and shift come from settings | rev 2.1 ICS-43434 needs checking |
| Power | GPIO7 set high only in `InitializeButtons()`, after display init; GPIO6 long press 5 s toggles the latch | GPIO7 high in a **bootloader hook** and again on the first line of `app_main` (no `gpio_reset_pin`) | Board stays on when started from battery |
| sdkconfig | QIO, 240 MHz, OCT 80M, `ALWAYSINTERNAL=1024`, `RESERVE_INTERNAL=102400`, `MBEDTLS_EXTERNAL_MEM_ALLOC`, `MBEDTLS_DYNAMIC_BUFFER`, rollback, cross-signed verify, renegotiation off, I-cache 32 KB / D-cache line 64 B, `LV_USE_SNAPSHOT`, `-Os`, tiny internal Wi-Fi buffers, `LV_USE_KEYBOARD=n` | adopted, **except**: `-O2` (PERF), Wi-Fi/lwIP buffers in PSRAM, keyboard enabled, `FREERTOS_HZ=1000` | Performance; keyboard needed |
| Partitions | nvs 0x4000, otadata 0xd000, 2×0x3F0000 app, 8 MB assets | the repo's existing table (2×4 MB app, 7.75 MB storage, 128 KB coredump) | Coredump; larger app slots |

Licences: the xiaozhi-derived files (`components/display/st77916_init_new.h`, display bring-up) keep the MIT header. `components/dns_server` is copied from the IDF `captive_portal` example (Unlicense OR CC0-1.0). Fonts are Montserrat (OFL, with `assets/fonts/OFL.txt`). Nothing is copied from WU-AetherCore (PolyForm-NC).

---

## 1. Directory layout, components and public C APIs

```
esp32-ai-assistant/
├─ CMakeLists.txt              # + idf_build_set_property(MINIMAL_BUILD ON)
├─ sdkconfig.defaults          # §4 (dev profile)
├─ sdkconfig.defaults.release  # 4 overrides (SYSMON/PERF/MEM monitor off, log max INFO)
├─ partitions.csv              # §5 (already in repo)
├─ dependencies.lock           # committed
├─ eim_config.toml             # already in repo
├─ bootloader_components/
│   └─ power_latch/            # bootloader_before_init(): GPIO7 = 1
├─ main/
│   ├─ CMakeLists.txt  idf_component.yml  Kconfig.projbuild
│   ├─ main.c                  # boot sequence + self-test (§8)
│   ├─ app/  app.c app.h app_evt.h dev_cmds.c   # state machine + app/ui console commands
│   └─ ui/   ui.c ui.h ui_theme.c ui_orb.c ui_home.c ui_chat.c ui_translator.c
│            ui_settings.c ui_keyboard.c ui_portal.c ui_test.c ui_str.c
│            lvgl_psram_pool.h fonts/font_ru_{16,20,28,40}.c   # generated, committed
├─ components/
│   ├─ util/        b64.c crc.c psram.c json.c (cJSON hooks → PSRAM)
│   ├─ board/       board.c                (latch, I2C0/1, TCA9554, buttons, LEDC, ADC)
│   ├─ display/     display.c st77916_init_new.h
│   ├─ touch/       touch.c                (CST816 + merged LVGL indev + injection)
│   ├─ audio/       audio_in.c audio_out.c wav.c
│   ├─ settings/    settings.c settings_table.inc
│   ├─ net/         wifi.c portal.c portal/index.html
│   ├─ dns_server/  (copy of IDF example component)
│   ├─ httpx/       http_util.c sse.c jsonfield.c   (streamed JSON+audio POST, SSE, incremental field extractor)
│   ├─ ai/          ai_client.c prompts.c tts_client.c stt_client.c
│   ├─ ota/         ota.c selftest.c
│   └─ devconsole/  devconsole.c cmd_sys.c cmd_ui_io.c cmd_audio.c cmd_net.c cmd_ota.c
├─ assets/  fonts/ (TTF + OFL)  storage/ (UI sounds → spiffs image)
├─ tools/   idf.ps1 (exists) py.ps1 build.ps1 flash.ps1 release.ps1 restore.ps1 slot.ps1
│           devcon.py secrets_push.py mkfonts.ps1
├─ tests/   acceptance.py golden/*.png audio/*.wav
└─ third_party/xiaozhi_ref/  (exists)
```

Registry pins live only in `main/idf_component.yml` (§6). Local components reach managed components by their manager names (`espressif__esp_lvgl_port`, `lvgl__lvgl`, `espressif__cjson`, `espressif__button`, `espressif__esp_lcd_st77916`, `espressif__esp_lcd_touch_cst816s`, `espressif__esp_io_expander_tca9554`).

Dependency direction: `main → {devconsole, ai, net, ota, audio, touch, display, board, settings, httpx, util}`. `devconsole` never includes `main`. App- and UI-specific commands are registered from `main/app/dev_cmds.c` through `devconsole_register()`.

Fonts are compiled into the app (rodata), so each A/B slot carries fonts that match its code. The `storage` spiffs partition holds only optional UI sounds. It is built with `spiffs_create_partition_image(storage assets/storage)` **without** `FLASH_IN_PROJECT` and flashed separately with `idf.py storage-flash`.

### 1.1 `util`
```c
void   util_init(void);                       /* cJSON_InitHooks → heap_caps_malloc(MALLOC_CAP_SPIRAM) */
void  *psram_malloc(size_t n);  void *psram_calloc(size_t n, size_t sz);  char *psram_strdup(const char *s);
size_t b64_enc_len(size_t n);                 /* 4*((n+2)/3) */
size_t b64_encode(const uint8_t *in, size_t n, char *out);          /* n % 3 == 0 except last chunk */
int    b64_decode(const char *in, size_t n, uint8_t *out, size_t out_max); /* -1 on error */
uint32_t crc32_ieee(uint32_t crc, const void *buf, size_t n);        /* esp_rom_crc32_le(); zlib-compatible, self-checked by "crc" cmd */
```

### 1.2 `board`
```c
#define BOARD_PIN_PWR_LATCH GPIO_NUM_7   /* out, 1 = stay on (battery) */
#define BOARD_PIN_PWR_KEY   GPIO_NUM_6   /* in, active low */
#define BOARD_PIN_BOOT_KEY  GPIO_NUM_0   /* in, active low, strapping */
#define BOARD_PIN_BL        GPIO_NUM_5   /* LEDC */
#define BOARD_PIN_I2C0_SDA  GPIO_NUM_11
#define BOARD_PIN_I2C0_SCL  GPIO_NUM_10  /* TCA9554 0x20, PCF85063 0x51, QMI8658 0x6B; 10k pull-ups */
#define BOARD_PIN_TP_SDA    GPIO_NUM_1
#define BOARD_PIN_TP_SCL    GPIO_NUM_3   /* strapping pin; internal pull-ups enabled */
#define BOARD_PIN_TP_INT    GPIO_NUM_4
#define BOARD_PIN_BAT_ADC   GPIO_NUM_8   /* ADC1_CH7, x3 divider */
#define BOARD_EXIO_TP_RST   IO_EXPANDER_PIN_NUM_0   /* EXIO1 */
#define BOARD_EXIO_LCD_RST  IO_EXPANDER_PIN_NUM_1   /* EXIO2 */
/* EXIO3 (SD CS), EXIO4/5 (IMU INT2/INT1): left as inputs (TCA9554 reset state) */

typedef enum { BOARD_BTN_BOOT, BOARD_BTN_PWR } board_btn_t;
typedef enum { BTN_EV_DOWN, BTN_EV_UP, BTN_EV_CLICK, BTN_EV_DOUBLE, BTN_EV_LONG_START, BTN_EV_LONG_UP } board_btn_ev_t;
typedef void (*board_btn_cb_t)(board_btn_t b, board_btn_ev_t ev, void *ctx);   /* runs in esp_timer task: post only */

void      board_power_latch(void);    /* gpio_set_level(7,1) then gpio_config(OUTPUT); never gpio_reset_pin(7) */
void      board_power_off(void);      /* GPIO7 = 0 (turns off only on battery) */
esp_err_t board_init(void);           /* I2C0 port 0 + TCA9554 (P0|P1 out =1) + I2C1 port 1 + ADC1 oneshot + LEDC (duty 0) */
esp_err_t board_reset_lcd_touch(void);/* EXIO1|EXIO2: 0 → 20 ms → 1 → 150 ms */
i2c_master_bus_handle_t  board_i2c0(void);
i2c_master_bus_handle_t  board_i2c1(void);
esp_io_expander_handle_t board_exio(void);
esp_err_t board_backlight_set(uint8_t pct, uint32_t fade_ms); /* LEDC_LOW_SPEED_MODE, TIMER_0, CHANNEL_0, 20 kHz, 10-bit, duty=1023*(pct/100)^2.2, ledc_set_fade_with_time */
uint8_t   board_backlight_get(void);
esp_err_t board_buttons_start(board_btn_cb_t cb, void *ctx);  /* espressif/button 4.2.2: iot_button_new_gpio_device, active_level 0; long 1500 ms BOOT / 3000 ms PWR */
int       board_battery_mv(void);     /* adc_oneshot + curve-fitting cali ×3; -1 on error */
```
Bootloader hook (`bootloader_components/power_latch/power_latch.c`):
```c
#include "esp_rom_gpio.h"
#include "soc/gpio_reg.h"
void bootloader_hooks_include(void) {}
void bootloader_before_init(void) {
    esp_rom_gpio_pad_select_gpio(7);
    REG_WRITE(GPIO_OUT_W1TS_REG, BIT(7));
    REG_WRITE(GPIO_ENABLE_W1TS_REG, BIT(7));
}
```
The bootloader is only ever updated over USB. The app latches GPIO7 again in any case.

### 1.3 `display`
```c
typedef struct { uint8_t id[4]; bool id_ok; bool init_new; uint32_t pclk_hz; } display_info_t;
typedef struct { uint32_t frames, fps_x10, render_us_avg, render_us_max, refr_us_max; } display_stats_t;

esp_err_t display_init(lv_display_t **out);  /* needs board_init() + board_reset_lcd_touch() */
const display_info_t *display_info(void);
void      display_stats(display_stats_t *out, bool reset); /* LV_EVENT_RENDER_START/READY + REFR_READY counters */
esp_err_t display_set_invert(bool on);       /* esp_lcd_panel_invert_color(); setting lcd_inv */
esp_err_t display_sleep(bool sleep);         /* SLPIN/SLPOUT + backlight */
```
Sequence inside `display_init()`:
1. `spi_bus_initialize(SPI2_HOST, &ST77916_PANEL_BUS_QSPI_CONFIG(40, 46, 45, 42, 41, 360*40*2), SPI_DMA_CH_AUTO)`.
2. Probe. Use `io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(21, NULL, NULL)` with `io_cfg.pclk_hz = 3000000`, then `esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io_probe)`, then `esp_lcd_panel_io_rx_param(io_probe, (0x0B<<24)|(0x04<<8), id, 4)`, then `esp_lcd_panel_io_del(io_probe)`.
3. Create the fast IO with `io_cfg.pclk_hz = CONFIG_RA_LCD_PCLK_MHZ*1000000`.
4. Choose the init table. `id == 00 02 7F 7F` → `vendor_config.init_cmds = st77916_init_new` (184 entries). `00 7F 7F 7F`, an unknown ID or a failed read → driver default, with `ESP_LOGW` on unknown or failed.
5. `esp_lcd_new_panel_st77916(io, &(esp_lcd_panel_dev_config_t){ .reset_gpio_num = GPIO_NUM_NC, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16, .vendor_config = &vcfg }, &panel)`, then `esp_lcd_panel_reset`, `esp_lcd_panel_init`, `esp_lcd_panel_disp_on_off(panel, true)`. If `lcd_inv == 0`, call `display_set_invert(false)`.
6. Start the LVGL port:
   ```c
   lvgl_port_cfg_t pc = ESP_LVGL_PORT_INIT_CONFIG();
   pc.task_priority = 3; pc.task_stack = 8192; pc.task_affinity = 1;
   pc.task_max_sleep_ms = 500; pc.timer_period_ms = 5;
   lvgl_port_init(&pc);
   lvgl_port_display_cfg_t dc = {
     .io_handle = io, .panel_handle = panel,
     .buffer_size = 360 * CONFIG_RA_LVGL_BUF_LINES /*40*/, .double_buffer = true,
     .hres = 360, .vres = 360, .color_format = LV_COLOR_FORMAT_RGB565,
     .rounder_cb = IS_ENABLED(CONFIG_RA_LCD_EVEN_ROUNDER) ? even_rounder : NULL, /* x1&=~1, x2|=1, y1&=~1, y2|=1 */
     .flags = { .buff_dma = 1, .buff_spiram = 0, .swap_bytes = 1, .full_refresh = 0, .direct_mode = 0 } };
   *out = lvgl_port_add_disp(&dc);
   ```
   TE (GPIO18) is not used. Neither init table sends TEON, and Espressif documents QSPI as tearing-mode NONE.
7. The backlight stays at 0 until the first `LV_EVENT_REFR_READY` after `ui_init()`. Then `board_backlight_set(bright, 300)`.

### 1.4 `touch`
```c
esp_err_t   touch_init(lv_display_t *disp);   /* I2C1 + CST816 + indev; non-fatal on failure */
lv_indev_t *touch_indev(void);
bool        touch_hw_ok(void);
uint8_t     touch_chip_id(void);              /* reg 0xA7 as read by the driver */
/* injection (devconsole, tests) */
esp_err_t touch_inject(int16_t x, int16_t y, bool pressed);           /* one sample, wakes LVGL */
esp_err_t touch_inject_tap(int16_t x, int16_t y, uint32_t hold_ms);   /* blocking in caller task */
esp_err_t touch_inject_swipe(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t dur_ms, uint16_t steps);
void      touch_set_raw_listener(void (*cb)(int16_t x, int16_t y, bool pressed));  /* "touch watch" */
```
Hardware setup:
- Bus: `i2c_new_master_bus({.i2c_port=1, .sda_io_num=1, .scl_io_num=3, .clk_source=I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt=7, .flags.enable_internal_pullup=1})`.
- Panel IO: `esp_lcd_new_panel_io_i2c(bus1, &(esp_lcd_panel_io_i2c_config_t)ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG(), &tp_io)` (address 0x15, 100 kHz).
- Touch config: `{ .x_max=360, .y_max=360, .rst_gpio_num=GPIO_NUM_NC /*EXIO1 done by board*/, .int_gpio_num=GPIO_NUM_4, .levels={.reset=0,.interrupt=0}, .interrupt_callback=touch_isr }`, then `esp_lcd_touch_new_i2c_cst816s(tp_io, &cfg, &tp)`.
- If the ID read fails: pulse EXIO1 again and retry once, then continue with `touch_hw_ok()==false`. The documented fallback is `CONFIG_ESP_LCD_TOUCH_CST816S_DISABLE_READ_ID=y`.

LVGL indev: one pointer indev of our own instead of `lvgl_port_add_touch()`. The port's read callback uses `ESP_ERROR_CHECK(esp_lcd_touch_read_data())`, and that aborts on the I2C NACK the CST816 returns while asleep.
```c
s_indev = lv_indev_create();
lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
lv_indev_set_mode(s_indev, LV_INDEV_MODE_EVENT);   /* read on wake only */
lv_indev_set_read_cb(s_indev, touch_read_cb);
lv_indev_set_display(s_indev, disp);
/* ISR: s_irq = true; lvgl_port_task_wake(LVGL_PORT_EVENT_TOUCH, NULL);   (IRAM_ATTR)          */
/* lv_timer s_poll (20 ms), resumed while pressed (real or injected), calls lv_indev_read() so
   long-press, scroll and release work even if INT pulses stop                                  */
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *d) {
    inj_sample_t s;
    if (inj_pop_or_hold(&s)) {          /* injected sample queued, or injected finger still down */
        d->point.x = s.x; d->point.y = s.y;
        d->state = s.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        return;                           /* injection has priority until its release is consumed */
    }
    if (atomic_exchange(&s_irq, false) || s_hw_pressed) {
        esp_lcd_touch_point_data_t p[1]; uint8_t n = 0;
        s_hw_pressed = esp_lcd_touch_read_data(s_tp) == ESP_OK &&
                       esp_lcd_touch_get_data(s_tp, p, &n, 1) == ESP_OK && n > 0;   /* NACK = released */
        if (s_hw_pressed) { s_last.x = p[0].x; s_last.y = p[0].y; }
        if (s_raw_cb) s_raw_cb(s_last.x, s_last.y, s_hw_pressed);
    }
    d->point = s_last;
    d->state = s_hw_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
```
Injection details:
- A 32-entry ring guarded by a spinlock. `touch_inject()` pushes a sample and calls `lvgl_port_task_wake(LVGL_PORT_EVENT_TOUCH, NULL)`.
- A tap is press, held samples every 20 ms for `hold_ms` (default 80), then release.
- A swipe is `steps` linear samples spaced `dur_ms/steps` apart, paced with `vTaskDelay` in the devconsole task, so LVGL sees real-time velocity for scroll and gesture detection.
- Mirroring and swap are set with `esp_lcd_touch_set_mirror_x/y` and `set_swap_xy` *(measure, M1)*.

### 1.5 `audio_in` (I2S1 RX)
```c
typedef struct { const int16_t *pcm; size_t samples; uint32_t ms; int16_t peak; int16_t rms_dbfs_x10; uint8_t buf_idx; } audio_clip_t;
typedef struct { int16_t rms_dbfs_x10[2]; uint16_t zero_permille[2]; } audio_probe_t;  /* [0]=RIGHT [1]=LEFT */
typedef enum { AUDIO_SRC_MIC, AUDIO_SRC_INJECT } audio_src_t;

esp_err_t audio_in_init(void);   /* creates channel disabled; 2 rec buffers in PSRAM */
esp_err_t audio_in_start(uint32_t turn_id, uint32_t max_ms, bool vad_stop);
esp_err_t audio_in_stop(uint32_t turn_id, audio_clip_t *out);   /* buffer lent to caller */
void      audio_in_release(const audio_clip_t *clip);             /* ai done with buffer */
void      audio_in_abort(void);
int       audio_in_level(void);                                   /* 0..100, atomic, 20 ms RMS */
esp_err_t audio_in_set_source(audio_src_t src, bool realtime);
esp_err_t audio_in_inject_load(const uint8_t *wav, size_t len);   /* RIFF, 16 kHz, mono, s16 only */
esp_err_t audio_in_probe(uint32_t ms_per_slot, audio_probe_t *out);
```
- Channel: `i2s_new_channel({.id=I2S_NUM_1, .role=I2S_ROLE_MASTER, .dma_desc_num=6, .dma_frame_num=240, .auto_clear_after_cb=true})`.
- Std mode: `clk 16000, mclk_multiple 256`; `slot {data_bit_width=32, slot_bit_width=AUTO, slot_mode=MONO, slot_mask=(mic_slot?LEFT:RIGHT), ws_width=32, bit_shift=true, left_align=true}`; `gpio {mclk=UNUSED, bclk=15, ws=2, din=39}`.
- The task reads 320 frames (20 ms) per call. Conversion is `s16 = clamp(s32 >> mic_shift)` (default 12), then a DC blocker `y=x-x1+0.995·y1`, then RMS.
- Recording buffers: 2 × (30 s × 16000 × 2 B + 44 B WAV header) = 2 × 960,044 B in PSRAM, ping-pong. A new turn can start while the AI task is still releasing the previous buffer.
- VAD (`vad_stop`): noise floor is the median of the first 300 ms. Speech means floor + 12 dB. Stop after 1000 ms of silence following at least 500 ms of speech. This produces `EV_REC_SILENCE`; reaching the limit produces `EV_REC_LIMIT`.
- Injected source: samples are copied from the inject buffer in 20 ms chunks (`vTaskDelay(20)` when `realtime`, otherwise immediately). The rest of the pipeline is identical.

### 1.6 `audio_out` (I2S0 TX → PCM5101)
```c
typedef struct { uint32_t rate; uint64_t bytes_played; uint32_t underruns, queued_bytes, writes_err; uint8_t volume; } audio_out_stats_t;
esp_err_t audio_out_init(void);                       /* 24 kHz, ring 256 KB PSRAM */
esp_err_t audio_out_begin(uint32_t turn_id, uint32_t rate, uint8_t channels);
size_t    audio_out_write(uint32_t turn_id, const void *s16le, size_t bytes, TickType_t wait); /* xStreamBufferSend */
void      audio_out_end(uint32_t turn_id);           /* EV_AUDIO_DRAINED when ring + DMA empty */
void      audio_out_stop(void);                      /* cancel/barge-in: 10 ms fade, drain-discard */
void      audio_out_set_volume(uint8_t pct);         /* ramp over 10 ms */
esp_err_t audio_out_tone(uint32_t hz, uint32_t ms, uint8_t vol, uint32_t rate);  /* generated in-task */
int       audio_out_level(void);
void      audio_out_get_stats(audio_out_stats_t *s, bool reset);
```
- Channel: `{.id=I2S_NUM_0, .dma_desc_num=8, .dma_frame_num=240, .auto_clear_after_cb=true}` (silence on underrun).
- Std mode: `slot {32-bit, MONO, slot_mask=BOTH, ws_width 32, bit_shift true}`; `gpio {bclk=48, ws=38, dout=47, mclk=UNUSED}`. That is xiaozhi's mono→stereo, done in hardware. A software fallback (`STEREO`, L=R interleave) sits behind `CONFIG_RA_SPK_SW_STEREO`.
- Rate change (`audio_out_begin` with a different rate): wait until DMA is idle, then `i2s_channel_disable` → `i2s_channel_reconfig_std_clock(tx, &I2S_STD_CLK_DEFAULT_CONFIG(rate))` → `i2s_channel_enable`. Supported rates: 16000, 22050, 24000, 44100, 48000. PCM5101 takes its PLL reference from BCK, which is 64·fs.
- Channels = 2 → downmix (L+R)/2.
- Volume: `gain_q16 = (vol/100)^2 · 65536`, `out32 = clamp((int64)s16 · gain_q16)`.
- Ring: `xStreamBufferCreateWithCaps(256*1024, 1, MALLOC_CAP_SPIRAM)`, with one writer (tts) and one reader (audio_out). Playback starts after 150 ms are buffered or at end of stream.
- Volume changes are debounced. NVS is written only in IDLE, because flash writes stall both cores.

### 1.7 `settings` (NVS)
```c
typedef enum { ST_U8, ST_U16, ST_U32, ST_STR } st_type_t;
#define ST_SECRET 0x01  /* never printed, masked "sk-or-…c3f9 (73)" */
#define ST_REBOOT 0x02  /* applied after reboot */
typedef struct { const char *ns, *key; st_type_t type; uint32_t def_num; const char *def_str;
                 int32_t min, max; uint16_t max_len; uint8_t flags; } setting_desc_t;

esp_err_t settings_init(void);    /* nvs_flash_init (erase+retry on NO_FREE_PAGES/NEW_VERSION_FOUND), load cache, migrate cfg_ver */
int32_t   settings_get_int(const char *key);
size_t    settings_get_str(const char *key, char *buf, size_t len);    /* copy, thread-safe */
esp_err_t settings_set_int(const char *key, int32_t v);                 /* validate, nvs_set + nvs_commit */
esp_err_t settings_set_str(const char *key, const char *v);
esp_err_t settings_set_from_string(const char *key, const char *v);     /* console/portal */
esp_err_t settings_reset(bool keep_wifi);
const setting_desc_t *settings_table(size_t *n);
void      settings_on_change(void (*cb)(const char *key, void *ctx), void *ctx);  /* → EV_SETTINGS_CHANGED */
```
NVS encryption stays off on purpose: HMAC-based NVS encryption needs an irreversible eFuse burn. Secrets are kept out of git and out of logs.

Namespace **`cfg`** (all keys are 15 characters or fewer):

| key | type | default | range / notes |
|---|---|---|---|
| `ui_lang` | str 3 | `ru` | `ru`/`en` |
| `mode` | u8 | 0 | 0 assistant, 1 translator |
| `text_only` | u8 | 1 | 1 = no TTS (no speaker connected yet) |
| `volume` | u8 | 60 | 0–100 |
| `bright` | u8 | 80 | 5–100 |
| `dim_s` | u16 | 60 | 0 = never |
| `ptt_mode` | u8 | 0 | 0 tap-or-hold, 1 toggle only |
| `max_rec_s` | u8 | 30 | 3–30 |
| `vad_stop` | u8 | 0 | auto-stop in assistant mode |
| `mic_slot` | u8 | 0 | 0 right, 1 left |
| `mic_shift` | u8 | 12 | 8–16 |
| `ai_url` | str 128 | `https://openrouter.ai/api/v1` | must start `https://` |
| `ai_key` | str 200 S | `""` | secret |
| `ai_model` | str 96 | `google/gemini-3.1-flash-lite` | |
| `ai_model_fb` | str 96 | `google/gemini-3.5-flash-lite` | `""` = no `models` array |
| `ai_effort` | str 12 | `minimal` | `""` = field omitted. Never `none` (rejected by 3.5 on fallback) |
| `ai_stream` | u8 | 1 | SSE |
| `ai_json` | u8 | 1 | json_schema strict |
| `ai_maxtok` | u16 | 400 | 50–4000 |
| `ai_temp` | u8 | 40 | ×0.01 |
| `ai_hist` | u8 | 6 | turns of text history |
| `ai_sys` | str 2000 | `""` | custom system prompt |
| `tts_url` | str 128 | `""` | empty → `ai_url` |
| `tts_key` | str 200 S | `""` | empty → `ai_key` |
| `tts_model` | str 96 | `microsoft/mai-voice-2.1-flash` | |
| `tts_voice` | str 64 | `ru-RU-Masha:MAI-Voice-2.1-Flash` | |
| `tts_speed` | u8 | 100 | 50–200 % |
| `tts_rate` | u32 | 0 | 0 = Content-Type/table, else forced |
| `stt_mode` | u8 | 0 | 0 audio→chat, 1 /audio/transcriptions then text chat |
| `stt_url` | str 128 | `""` | empty → `ai_url` |
| `stt_key` | str 200 S | `""` | empty → `ai_key` |
| `stt_model` | str 96 | `openai/whisper-large-v3-turbo` | |
| `tr_lang_a` | str 8 | `ru` | ISO 639-1 |
| `tr_lang_b` | str 8 | `en` | |
| `tr_voice_a` | str 64 | `ru-RU-Masha:MAI-Voice-2.1-Flash` | |
| `tr_voice_b` | str 64 | `""` | auto: first `supported_voices` id with prefix `<lang>-` |
| `tr_vad` | u8 | 1 | auto-stop in translator mode |
| `lcd_inv` | u8 | 1 | 1 = keep INVON from the init table |
| `ota_url` | str 200 | `""` | |
| `dev_flags` | u32 | 0 | b0 perf overlay, b1 HTTP trace (no secrets), b2 portal on STA |

Namespace **`wifi`**: `n` u8, `ssid0`..`ssid4` str 33, `pass0`..`pass4` str 65 (S), `last` u8.
Namespace **`sys`**: `cfg_ver` u16 = 1, `boot_cnt` u32, `ok_ver` str 32 (last version that passed self-test), `rb_reason` str 64, `rb_cnt` u16.
`CONFIG_ESP_WIFI_NVS_ENABLED=n`, so the Wi-Fi driver writes nothing to NVS.

### 1.8 `net` (Wi-Fi STA + scan + SoftAP portal)
```c
typedef enum { NET_OFF, NET_CONNECTING, NET_UP, NET_FAILED } net_state_t;
typedef struct { net_state_t state; char ssid[33]; int8_t rssi; char ip[16]; bool portal; char ap_ssid[33]; char ap_pass[12]; } net_status_t;
typedef struct { char ssid[33]; int8_t rssi; uint8_t auth, ch; } net_ap_t;

esp_err_t net_init(void);   /* esp_netif_init, esp_event_loop_create_default, STA+AP netifs, esp_wifi_init, WIFI_STORAGE_RAM */
esp_err_t net_start(void);  /* STA with saved networks, best RSSI first; backoff 1,2,4,…,60 s */
esp_err_t net_connect(const char *ssid, const char *pass, bool save);
esp_err_t net_disconnect(void);
esp_err_t net_forget(const char *ssid);
esp_err_t net_scan(uint32_t timeout_ms, net_ap_t *out, int max, int *n);  /* esp_wifi_scan_start, ≤20 records */
void      net_status(net_status_t *out);
bool      net_wait_up(uint32_t timeout_ms);
esp_err_t net_portal_start(uint32_t idle_timeout_s);   /* default 600 s */
esp_err_t net_portal_stop(void);
void      net_set_low_latency(bool on);                 /* WIFI_PS_NONE during turns, MIN_MODEM idle */
```
After `NET_UP` the module starts SNTP (`esp_netif_sntp_init`, `pool.ntp.org`). TLS does not need it (`MBEDTLS_HAVE_TIME_DATE=n`); it is for the clock.

Portal:
- Mode `WIFI_MODE_APSTA`. SSID `RoundAI-%02X%02X` (last 2 MAC bytes). `WIFI_AUTH_WPA2_PSK` with a random 8-digit password per session, shown on screen with a QR code `WIFI:T:WPA;S:<ssid>;P:<pass>;;`. `max_connection=2`. The channel follows the STA.
- DHCP option 114: `esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, "http://192.168.4.1/")`.
- DNS: `start_dns_server(&DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF"))`.
- HTTP: `httpd_start` with `max_uri_handlers=12`, `max_open_sockets=7`, `lru_purge_enable=true`, `stack_size=6144`, `core_id=0`.
  - `GET /` serves `portal/index.html` (EMBED_TXTFILES, ≤ 8 KB, inline CSS/JS, no external resources).
  - `GET /scan.json`, `GET /status.json`.
  - `POST /save` (urlencoded, ≤ 4 KB): `ssid, pass, ai_url, ai_key, ai_model, tts_model, tts_voice, tr_lang_a, tr_lang_b`. An empty secret field keeps the stored value. Secrets are never echoed back.
  - Captive-detection URIs (`/generate_204`, `/hotspot-detect.html`, `/connecttest.txt`, `/ncsi.txt`) and the 404 handler answer `302 Location: http://192.168.4.1/`.
- Requests whose local socket address is not the AP IP get 403, unless `dev_flags` bit 2 is set. That bit is used by the automated M3 test.

### 1.9 `httpx` (shared HTTPS helpers)
```c
typedef struct { const char *base_url, *path, *api_key, *accept; int timeout_ms; int rx_buf, tx_buf; } httpx_cfg_t;
esp_http_client_handle_t httpx_client(const httpx_cfg_t *c, http_event_handle_cb on_evt, void *ctx);
                         /* crt_bundle_attach=esp_crt_bundle_attach, keep_alive_enable=true, buffer 4096/2048,
                            headers: Authorization: Bearer, Content-Type: application/json,
                            X-OpenRouter-Title: Round Assistant */
typedef bool (*httpx_abort_fn)(void *ctx);
esp_err_t httpx_post_json_audio(esp_http_client_handle_t h, const char *prefix, const int16_t *pcm, size_t samples,
                                const char *suffix, httpx_abort_fn abort, void *ctx, void (*progress)(size_t sent, size_t total));
          /* Content-Length = strlen(prefix) + b64_enc_len(44+2*samples) + strlen(suffix);
             esp_http_client_open(h,len); write prefix; WAV header + PCM encoded 3072 B → 4096 chars per write; suffix */
int       httpx_read(esp_http_client_handle_t h, char *buf, int len, httpx_abort_fn abort, void *ctx);  /* 1000 ms socket timeout loop */
typedef void (*sse_cb_t)(const char *data, size_t len, void *ctx);   /* one "data:" payload */
esp_err_t sse_pump(esp_http_client_handle_t h, sse_cb_t cb, void *ctx, httpx_abort_fn abort);
          /* ignores ":"-comments (": OPENROUTER PROCESSING"), stops at "data: [DONE]", 16 KB PSRAM line buffer */
typedef struct jf jf_t;   /* incremental JSON string-field extractor over streamed partial JSON */
jf_t *jf_new(const char *const *keys, void (*on_chars)(int key, const char *utf8, size_t n, void *), void *ctx);
void  jf_feed(jf_t *j, const char *s, size_t n);  /* handles \" \\ \n \uXXXX incl. surrogates */
void  jf_free(jf_t *j);
```

### 1.10 `ai_client` (OpenAI-compatible chat with audio)
```c
typedef enum { AI_MODE_ASSISTANT, AI_MODE_TRANSLATOR } ai_mode_t;
typedef struct { uint32_t turn_id; ai_mode_t mode; const audio_clip_t *clip; const char *text; } ai_request_t;
typedef struct { char id[96]; char name[64]; float usd_in_m, usd_out_m, usd_audio_m; } ai_model_t;
typedef struct { float limit_remaining; float usage; bool free_tier; bool has_limit; } ai_key_info_t;

esp_err_t ai_client_init(void);                       /* creates "ai" task + ai_q(2) */
esp_err_t ai_client_submit(const ai_request_t *r);    /* non-blocking; events via app_post() */
void      ai_client_cancel(uint32_t turn_id);
void      ai_client_history_clear(void);
esp_err_t ai_client_prewarm(void);                    /* GET {ai_url}/key on the kept handle if idle > 30 s (TLS warm-up while user speaks) */
esp_err_t ai_client_list_models(int page, ai_model_t *out, int max, int *n);
          /* GET /models?input_modalities=audio&output_modalities=text&supported_parameters=response_format,structured_outputs
             &sort=pricing-low-to-high&limit=12&offset=12*page; drop ':batch', '-preview', '~' aliases, negative price, expiration_date != null */
esp_err_t ai_client_key_info(ai_key_info_t *out);     /* GET /key (per-key limit, not account balance) */
const char *ai_client_last_request_redacted(void);    /* console "ai last-req": prefix without audio/key */
```
Request (assistant). `messages` comes last, so the base64 is the only variable chunk:
```json
{"model":"google/gemini-3.1-flash-lite","models":["google/gemini-3.1-flash-lite","google/gemini-3.5-flash-lite"],
 "stream":true,"reasoning":{"effort":"minimal"},"max_tokens":400,"temperature":0.4,
 "provider":{"require_parameters":true,"sort":"latency"},
 "response_format":{"type":"json_schema","json_schema":{"name":"assistant_turn","strict":true,"schema":{
   "type":"object","properties":{"transcript":{"type":"string"},"reply":{"type":"string"}},
   "required":["transcript","reply"],"additionalProperties":false}}},
 "messages":[{"role":"system","content":"<prompt RU>"}, …history…,
   {"role":"user","content":[{"type":"text","text":"Transcribe the audio verbatim into transcript, answer in reply."},
     {"type":"input_audio","input_audio":{"data":"<BASE64 WAV>","format":"wav"}}]}]}
```
Translator schema: `{"src_lang":{"type":"string","enum":["<a>","<b>"]},"transcript":…,"translation":…}`, with `required` in that order so that `src_lang` streams first. The translator sends no history.

Response handling:
- `sse_pump` feeds `choices[0].delta.content` into `jf_feed`, which watches the keys transcript / reply / translation / src_lang.
- Sentence splitter on reply or translation: split after `.!?…` followed by a space, quote or end, or at a newline. Pieces shorter than 24 characters are merged; pieces longer than 250 are split at the last space or comma. Each piece becomes `EV_AI_SENTENCE`.
- `finish_reason:"error"` or an `error` object → `EV_AI_ERROR`. The usage frame is logged.
- At the end, `cJSON_Parse(content)` validates. If the model ignored the schema, the whole content becomes the reply.
- `ai_stream=0` reads `choices[0].message.content` instead.

Errors: 401 key, 402 credits, 403 region/forbidden, 404 model, 408/timeout, 413 too long, 429 rate, 5xx/524/529 provider. On connect errors or 5xx before the first response byte, the request is retried once after 500 ms (the base64 is re-streamed). There is no retry after partial data.

Timeouts: connect 10 s, each write 15 s, first byte 30 s, gap between SSE events 20 s, turn cap 90 s.

### 1.11 `tts_client` and `stt_client`
```c
esp_err_t tts_client_init(void);   /* "tts" task + tts_q(12) */
esp_err_t tts_client_enqueue(uint32_t turn_id, uint16_t seq, const char *text, const char *voice, bool last);
void      tts_client_cancel(uint32_t turn_id);
esp_err_t tts_client_say(const char *text, const char *voice, uint32_t timeout_ms);   /* console "say" */
uint32_t  tts_rate_for(const char *model, const char *content_type);
          /* parse "audio/pcm;rate=24000;channels=1"; table: heygen/ 44100, everything else 24000; tts_rate overrides */

esp_err_t stt_client_transcribe(uint32_t turn_id, const audio_clip_t *clip, const char *lang /* NULL=auto */,
                                char *text, size_t text_len, char *lang_out, size_t lang_len);
          /* POST {stt_url}/audio/transcriptions {"model","input_audio":{"data","format":"wav"},"language","response_format":"verbose_json"} */
```
TTS POST `{tts_url}/audio/speech`, body `{"model","input","voice","response_format":"pcm","speed"}`.
- The `HTTP_EVENT_ON_HEADER` callback captures Content-Type. `audio/mpeg` → `EV_TTS_ERROR` "mp3 unsupported". JSON → error body.
- The body is read in 2 KB chunks, an odd trailing byte is carried over, and data goes to `audio_out_write(turn, …, 100 ms)`, which applies backpressure to TCP.
- Sentences are fetched one after another on one keep-alive handle. The first PCM byte of a turn → `EV_TTS_FIRST_AUDIO` with TTFB. A job with `last` → `audio_out_end()`.

### 1.12 `app` (state machine), §2.3
```c
typedef enum { APP_IDLE, APP_LISTENING, APP_UPLOADING, APP_THINKING, APP_SPEAKING, APP_ERROR } app_state_t;
typedef enum { APP_MODE_ASSISTANT, APP_MODE_TRANSLATOR } app_mode_t;
typedef enum { EV_PTT_DOWN, EV_PTT_UP, EV_PTT_TOGGLE, EV_CANCEL, EV_TEXT_SUBMIT, EV_REC_LIMIT, EV_REC_SILENCE,
  EV_AI_UPLOAD_DONE, EV_AI_TRANSCRIPT, EV_AI_SRC_LANG, EV_AI_DELTA, EV_AI_SENTENCE, EV_AI_DONE, EV_AI_ERROR,
  EV_TTS_FIRST_AUDIO, EV_TTS_ERROR, EV_AUDIO_DRAINED, EV_NET_UP, EV_NET_DOWN, EV_SET_MODE, EV_SET_TEXT_ONLY,
  EV_SETTINGS_CHANGED, EV_TICK } app_evt_type_t;
typedef struct { app_evt_type_t type; uint32_t turn_id; int32_t i; char *s; /* PSRAM, receiver frees */ } app_evt_t;
typedef struct { app_state_t state; app_mode_t mode; bool text_only; uint32_t turn; int32_t last_err;
                 uint32_t t_upload_ms, t_ttfb_ms, t_tts_ttfb_ms, t_total_ms; } app_status_t;
esp_err_t   app_start(void);
bool        app_post(const app_evt_t *e, TickType_t wait);
app_state_t app_state(void);
void        app_status(app_status_t *out);
```

### 1.13 `ui`
```c
typedef enum { UI_SET_STATE, UI_SET_MODE, UI_USER_MSG, UI_BOT_MSG, UI_BOT_APPEND, UI_TOAST, UI_ERROR,
               UI_SET_NET, UI_PORTAL, UI_GOTO, UI_SET_TEXT_ONLY } ui_msg_type_t;
typedef struct { ui_msg_type_t type; int32_t i; char *s; } ui_msg_t;
esp_err_t ui_init(lv_display_t *disp);  /* theme, fonts, screens, subjects, ui_pump timer (20 ms) */
void      ui_post(const ui_msg_t *m);   /* any task, non-blocking (ui_q 32) */
bool      ui_lock(uint32_t ms);  void ui_unlock(void);   /* lvgl_port_lock/unlock */
esp_err_t ui_snapshot(lv_draw_buf_t *dst);               /* lv_snapshot_take_to_draw_buf(lv_screen_active(), RGB565) */
lv_obj_t *ui_find(const char *name);                     /* lv_obj_find_by_name(lv_screen_active(), name) */
int       ui_tree_json(char *out, size_t len);           /* named objects: name,type,x,y,w,h,flags,text */
int       ui_audit_json(char *out, size_t len);          /* bbox inside r=174 circle; clickable >= 48x48 */
```
Rules:
- Only the LVGL task touches widgets. Every other task uses `ui_post()`, or `ui_lock()` for devconsole snapshots.
- Subjects (`subj_state`, `subj_mode`, `subj_net`, `subj_text_only`) change only inside `ui_pump`. They are wrapped in `ui_subject.h`, because LVGL 9.6 replaces `lv_subject_init_*`.
- No `clip_corner` on the root screen. No `lv_layer_top()` for content: toasts and dialogs are children of the active screen, so `lv_snapshot` captures them.
- Objects that tests use get `lv_obj_set_name()` (`CONFIG_LV_USE_OBJ_NAME`).

Home screen coordinates (360×360, centre 180,180, usable radius 168):

| name | geometry | note |
|---|---|---|
| `home_orb` | 128×128 at (116,86), radial gradient | level ring: 32 bars drawn r 88–112 in `LV_EVENT_DRAW_MAIN_BEGIN` on a 220×220 object at (70,40) |
| `home_state` | `lv_arclabel` r=158, 120° centred at top | idle / listening / … text |
| `home_text` | x 50–310, y 232–272 | last transcript / reply, 2 lines |
| `home_ptt` | Ø80, centre (180,292) | tap = toggle, hold > 300 ms = push-to-talk |
| `home_mode` | Ø56, centre (88,262) | assistant ↔ translator |
| `home_voice` | Ø56, centre (272,262) | text-only toggle |
| `home_settings` | Ø48, centre (180,44) | |

Keyboard (`scr_kb`):
- `kb_ta` at x 70–290, y 46–96.
- `kb` (`lv_keyboard`) at y 104–312, four rows of 50 px. Row centres are y 130/182/234/286. Key counts per row are 6/6/6/4, taken from chord widths of 300/331/296/210 px at r=168.
- Maps: `USER_1/USER_2` RU pages 1/2 (33 letters over 2×18 keys), `TEXT_LOWER/UPPER` EN, `NUMBER` symbols. Row 4: shift, `РУС/EN`, space, ⌫/OK.
- Our own `VALUE_CHANGED` handler catches the page and language keys and forwards everything else to `lv_keyboard_def_event_cb`. Popovers are on for rows 2–3 only.

Fonts:
- Built with `lv_font_conv` 4 bpp from Montserrat. Ranges `0x20-0x7E,0xA0-0xFF,0x401,0x410-0x44F,0x451,0x2010-0x2027,0x2116`, with FontAwesome symbols merged in. Sizes 16/20/28/40 (≈150 KB total).
- `--lv-fallback lv_font_montserrat_14`.

### 1.14 `devconsole` (§7) and `ota` (§8)
```c
esp_err_t devconsole_start(void);      /* usb_serial_jtag_driver_install(rx 4096, tx 4096) + usb_serial_jtag_vfs_use_driver()
                                          + esp_log_set_vprintf(devcon_vprintf) + esp_console_init(max_cmdline_length 4200) */
esp_err_t devconsole_register(const esp_console_cmd_t *cmd);
void      devcon_ok(int id, const char *json);   void devcon_err(int id, esp_err_t e, const char *msg);
void      devcon_event(const char *fmt, ...);    /* "@! EVT {…}" */

typedef struct { char running[8], next[8]; esp_ota_img_states_t state; bool pending; char ver[32]; char rb_reason[64]; } ota_info_t;
void      ota_get_info(ota_info_t *o);
esp_err_t ota_selftest_and_confirm(uint32_t budget_ms);
esp_err_t ota_stream_begin(size_t size);  esp_err_t ota_stream_write(const void *p, size_t n);  esp_err_t ota_stream_end(bool set_boot);
esp_err_t ota_start_https(const char *url);   /* esp_https_ota, task "ota", EVT progress */
esp_err_t ota_set_boot(const char *label);    /* "ota_0"/"ota_1" + esp_restart */
esp_err_t ota_rollback_now(void);             /* boot the other valid slot */
```

---

## 2. FreeRTOS tasks, message flow, state machine

### 2.1 Task list (`CONFIG_FREERTOS_HZ=1000`; all stacks are internal RAM)

| Task | Owner | Core | Prio | Stack | Blocks on |
|---|---|---|---|---|---|
| `main` | IDF | 0 | 1 | 8192 | boot sequence; returns after self-test (stack freed) |
| `taskLVGL` | esp_lvgl_port | 1 | 3 | 8192 | `xEventGroupWaitBits(lvgl_events, next lv_timer ≤ 500 ms)` |
| `audio_in` | audio | 1 | 7 | 4096 | `ulTaskNotifyTake` while idle; `i2s_channel_read` 20 ms chunks (200 ms timeout) |
| `audio_out` | audio | 1 | 6 | 4096 | `xStreamBufferReceive(ring, 20 ms worth, 50 ms)`, `i2s_channel_write` (DMA) |
| `app` | app | 1 | 5 | 6144 | `xQueueReceive(app_q[24], until next state deadline)` |
| `ai` | ai | 0 | 5 | 8192 | `xQueueReceive(ai_q[2])`; socket I/O (1 s timeouts, checks cancel) |
| `tts` | ai | 0 | 5 | 6144 | `xQueueReceive(tts_q[12])`; socket read; `xStreamBufferSend(100 ms)` |
| `net` | net | 0 | 4 | 4096 | `xQueueReceive(net_q[8], backoff timeout)` |
| `devcon` | devconsole | 0 | 2 | 6144 | `usb_serial_jtag_read_bytes(…, 100 ms)` |
| `ota` (on demand) | ota | 0 | 4 | 8192 | `esp_https_ota_perform` |
| `httpd` (portal) | esp_http_server | 0 | 5 | 6144 | `select()` |
| `dns_server` (portal) | dns_server | 0 | 5 | 4096 | `recvfrom()` |
| `wifi` | IDF | 0 | 23 | 6656 | driver |
| `tiT` (lwIP) | IDF | 0 (affinity) | 18 | 3072 | mbox |
| `sys_evt` | IDF | any | 20 | 4096 | default event loop (Wi-Fi/IP only) |
| `esp_timer` | IDF | 0 | 22 | 3584 | button callbacks (post only), LEDC fades |
| `ipc0/1`, `IDLE0/1`, `Tmr Svc` | IDF | 0/1 | 24/0/1 | 1280/1536/2048 | – |

Placement:
- Core 1 runs UI and real-time audio. Audio priority sits above LVGL, and DMA covers 80 ms TX at 24 kHz and 90 ms RX at 16 kHz.
- Core 0 runs network, TLS and the console.

### 2.2 Message flow
```
BOOT/PWR (esp_timer) ─┐
LVGL widgets ─────────┼─ EV_PTT_* / EV_CANCEL / EV_TEXT_SUBMIT / EV_SET_MODE ─▶ app_q ─▶ [app]
devconsole ───────────┘                                                      │
esp_event (WIFI/IP) ─▶ net handlers ─▶ EV_NET_UP/DOWN ─▶ app_q               │
                                                                             ├─▶ audio_in_start/stop (task notify) ─▶ [audio_in]
[audio_in] ── EV_REC_LIMIT / EV_REC_SILENCE ─▶ app_q ; level → atomic ─▶ ui level timer (30 Hz)
                                                                             ├─▶ ai_client_submit ─▶ ai_q ─▶ [ai] ─HTTPS SSE─ OpenRouter
[ai] ── EV_AI_UPLOAD_DONE / TRANSCRIPT / SRC_LANG / DELTA / SENTENCE / DONE / ERROR ─▶ app_q
                                                                             ├─▶ tts_client_enqueue ─▶ tts_q ─▶ [tts] ─HTTPS─ /audio/speech
[tts] ── PCM ─▶ StreamBuffer 256 KB (PSRAM) ─▶ [audio_out] ─▶ I2S0 DMA ─▶ PCM5101
[tts] ── EV_TTS_FIRST_AUDIO / EV_TTS_ERROR ─▶ app_q ; [audio_out] ── EV_AUDIO_DRAINED ─▶ app_q
[app] ── ui_post() ─▶ ui_q ─▶ ui_pump (lv_timer 20 ms in taskLVGL) ─▶ widgets + subjects
[app] ── devcon_event("state") ─▶ USB
```
- Event group `g_sys` bits: `NET_UP`, `UI_READY`, `AUDIO_READY`, `SELFTEST_DONE`, `PORTAL_ON`, `TIME_OK`.
- `esp_event` carries only IDF system events. App events use the `app_q` queue, which gives ordering and a payload-ownership rule: strings are allocated by the sender with `psram_strdup` and freed by the receiver, even when the event is dropped.
- Every AI, TTS and audio event carries `turn_id`. The app drops any event whose `turn_id != cur_turn` (the payload is still freed).

### 2.3 App state machine (orthogonal variables `mode`, `text_only`, `cur_turn`)

| State | Event | Guard | Action | Next |
|---|---|---|---|---|
| IDLE | PTT_DOWN / PTT_TOGGLE | net up, key set | `cur_turn++`; `net_set_low_latency(1)`; `ai_client_prewarm()`; `audio_in_start(turn, max_rec_s, vad)` | LISTENING |
| IDLE | PTT_* | no net or no key | `ui_post(UI_ERROR, "Нет сети"/"Нет ключа")` | ERROR |
| IDLE | TEXT_SUBMIT(s) | net + key | `ai_client_submit(text)` | THINKING |
| LISTENING | PTT_UP / PTT_TOGGLE / REC_LIMIT / REC_SILENCE | clip ≥ 400 ms | `audio_in_stop` → clip; `ai_client_submit(clip)` (or STT first if `stt_mode=1`) | UPLOADING |
| LISTENING | same | clip < 400 ms | release; toast "Слишком коротко" | IDLE |
| LISTENING | CANCEL | – | `audio_in_abort` | IDLE |
| UPLOADING | AI_UPLOAD_DONE | – | UI "thinking" | THINKING |
| UPLOADING/THINKING | AI_TRANSCRIPT | – | user bubble | same |
| THINKING | AI_SRC_LANG | translator | voice = (src==a) ? voice_b : voice_a | THINKING |
| THINKING/SPEAKING | AI_DELTA | – | `UI_BOT_APPEND` | same |
| THINKING/SPEAKING | AI_SENTENCE | !text_only | `tts_client_enqueue(turn, seq++, s, voice, false)` | same |
| THINKING | TTS_FIRST_AUDIO | – | UI "speaking" | SPEAKING |
| THINKING | AI_DONE | text_only or seq==0 | finish | IDLE |
| THINKING/SPEAKING | AI_DONE | voice | enqueue `last=true` marker; `ai_done=1` | same |
| SPEAKING | AUDIO_DRAINED | ai_done | – | IDLE |
| THINKING/SPEAKING | TTS_ERROR | – | toast "Голос недоступен"; `text_only` for this turn | wait AI_DONE → IDLE |
| SPEAKING/THINKING/UPLOADING | PTT_DOWN (barge-in) | – | cancel turn (below); `cur_turn++`; start listening | LISTENING |
| UPLOADING/THINKING/SPEAKING | CANCEL | – | cancel turn | IDLE |
| any busy | AI_ERROR(code) / NET_DOWN | – | cancel turn; `UI_ERROR(code)` | ERROR |
| any busy | deadline (UPLOADING 30 s, THINKING 45 s with no delta, SPEAKING 120 s) | – | cancel; error "timeout" | ERROR |
| ERROR | 4 s / tap / CANCEL | – | – | IDLE |
| ERROR | PTT_DOWN | ok | as from IDLE | LISTENING |
| any | SET_MODE / SET_TEXT_ONLY | – | if busy cancel first; update; UI | IDLE |

Cancel turn = `ai_client_cancel(t)`, `tts_client_cancel(t)`, `audio_out_stop()`, `audio_in_abort()`, `net_set_low_latency(0)`.
- Workers poll the cancel flag between 4 KB writes and on every 1 s read timeout, then `esp_http_client_close()` from their own task. Cancel latency for the user is 0 ms, because the app switches state at once. Worker cleanup takes ≤ 1 s.

Inputs:
- BOOT click = `PTT_TOGGLE` (in THINKING/SPEAKING it means CANCEL). BOOT hold ≥ 300 ms = `PTT_DOWN`, release = `PTT_UP`. BOOT double = mode. BOOT long 1.5 s = CANCEL and go home.
- PWR click = screen on/off. PWR long 3 s = `board_power_off()`.
- Tap on the orb while busy = CANCEL.

---

## 3. Memory budget

S3 internal SRAM is 512 KB, of which I-cache 32 KB and D-cache 32 KB are taken. Expected free internal heap at the start of `app_main` is **≈ 300 KB** *(measure, M1: `heap` → `int.free`)*, including 8 KB of RTC FAST memory. PSRAM usable ≈ **8,100 KB** (8,192 minus ≈ 40 KB of external BSS from lwIP/net80211/wpa via `SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`).

Allocation policy:
- `SPIRAM_MALLOC_ALWAYSINTERNAL=1024`: anything larger than 1 KB goes to PSRAM first.
- `SPIRAM_MALLOC_RESERVE_INTERNAL=98304` (96 KB): a pool for explicit `MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL`. It serves the LVGL buffers, I2S DMA and SPI.
- cJSON hooks point to PSRAM. mbedTLS uses `EXTERNAL_MEM_ALLOC`. Wi-Fi/lwIP use `SPIRAM_TRY_ALLOCATE_WIFI_LWIP`.

Worst case: a 30 s voice turn with TTS playing and the dev buffers allocated.

| Consumer | Internal KB | PSRAM KB | Notes |
|---|---|---|---|
| LVGL draw buffers 2 × 360×40×2 B | 56.3 | – | DMA, reserve pool |
| LVGL TLSF heap | – | 1024 | `LV_MEM_POOL_ALLOC` |
| I2S DMA TX 8×240×4 + RX 6×240×4 + descriptors | 13.7 | – | MONO slot → 4 B per frame |
| SPI2/LCD, I2C×2, GPIO ISR, LEDC, ADC drivers | 5 | – | |
| USB-Serial-JTAG driver rings 4+4 KB | 8 | – | |
| Our task stacks (audio_in 4, audio_out 4, app 6, ai 8, tts 6, net 4, devcon 6) + TCBs | 41 | – | taskLVGL counted below |
| taskLVGL stack | 8 | – | |
| IDF tasks (wifi, tiT, sys_evt, esp_timer, ipc×2, idle×2, Tmr Svc) | 24.6 | – | |
| Wi-Fi/PHY/supplicant control | 30 | 180 | 16 static RX + 32 dyn RX + 32 dyn TX + 32 TX cache × 1.6 KB, worst case |
| lwIP core + sockets | 6 | 60 | pbufs, 22.5 KB send buffer × 2 sockets |
| mbedTLS × 2 sessions (dynamic buffers) | 4 | 80 | peak during handshakes |
| esp_http_client × 2 (rx 4 KB / tx 2 KB) | – | 12 | |
| NVS cache, queues, event groups, timers | 8 | – | |
| Miscellaneous ≤ 1 KB mallocs | 15 | – | |
| Recording buffers 2 × 960,044 B | – | 1876 | 30 s × 16 kHz × 16-bit |
| Mic injection buffer | – | 938 | only after `mic load` |
| Request body: prefix/suffix only, base64 streamed | – | 16 | materialised body would be 1,251 |
| SSE line buffer, content, chat history (20 msgs) | – | 72 | |
| TTS ring | – | 256 | 5.3 s at 24 kHz, 2.9 s at 44.1 kHz |
| Snapshot buffer 259,200 B + header | – | 254 | only after `shot` |
| Model list page + cJSON tree (Settings picker) | – | 400 | transient, never during a turn |
| OTA | – | 16 | |
| **Total** | **≈ 228** | **≈ 5,184** | |
| **Headroom** | **≈ 72 KB** | **≈ 2.9 MB** | |

- The portal adds about +24 KB internal (httpd 6 + dns 4 + AP Wi-Fi ≈ 10 + sockets 4), leaving ≈ 48 KB. Voice turns stay allowed.
- Targets, checked by tests: free internal ≥ 48 KB, largest internal block ≥ 16 KB, DMA free ≥ 12 KB during a turn; minimum-ever internal ≥ 32 KB; PSRAM free ≥ 2 MB.
- `CONFIG_SPIRAM_XIP_FROM_PSRAM` stays **off**. It would copy about 2.3 MB of code and rodata into PSRAM, cutting PSRAM headroom to roughly 0.6 MB. Reconsider it at M6 only if OTA writes visibly stall the UI.
- Tuning ladder if the internal targets are missed, applied in order:
  1. `CONFIG_ESP_WIFI_RX_IRAM_OPT=n` (≈ +17 KB)
  2. `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=n` (≈ +10 KB)
  3. `CONFIG_RA_LVGL_BUF_LINES=30` (+14 KB)
  4. `CONFIG_ESP_WIFI_IRAM_OPT=n` (≈ +10 KB)
  5. USB rings 2+2 KB (+4 KB)
  6. xiaozhi's small Wi-Fi profile (`TRY_ALLOCATE=n`, `STATIC_RX 3`, `DYNAMIC_RX 6`, `RX_BA_WIN 3`) only if PSRAM Wi-Fi buffers turn out unstable
- Flash: estimated app ≈ 1.9–2.3 MB (IDF + Wi-Fi + TLS ≈ 1.0 MB, LVGL ≈ 0.35 MB, fonts ≈ 0.15 MB, cert bundle 65 KB, our code ≈ 0.2 MB). That is ≤ 60 % of a 4 MB slot. `tools/build.ps1` fails above 85 %.
- Upload size: a 30 s clip is 1,280,060 base64 characters. At ≥ 400 KB/s TLS upload *(measure, M4)* that is ≈ 3.2 s; a typical 5 s clip is 213 KB, ≈ 0.6 s. AAC via esp_audio_codec 2.6.2 is a later optimisation if upload dominates latency.

---

## 4. `sdkconfig.defaults`

Every symbol below was checked against the IDF v6.1 Kconfig files, LVGL v9.5.0 Kconfig, esp_lcd_touch 1.2.1, and our `main/Kconfig.projbuild`.

After editing this file, delete `sdkconfig` and run `idf.py reconfigure`. Defaults do not override values already in `sdkconfig`; the current untracked `sdkconfig` has CPU 160 MHz and `HZ=100`.

```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y
CONFIG_ESP32S3_DATA_CACHE_32KB=y
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y
CONFIG_COMPILER_OPTIMIZATION_PERF=y
CONFIG_FREERTOS_HZ=1000

CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=n
CONFIG_BOOTLOADER_LOG_LEVEL_WARN=y
CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y

CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MEMTEST=n
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=98304
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y

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

CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=16
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=32
CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=32
CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM=32
CONFIG_ESP_WIFI_RX_BA_WIN=16
CONFIG_ESP_WIFI_TX_BA_WIN=6
CONFIG_ESP_WIFI_DYNAMIC_RX_MGMT_BUFFER=y
CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=n
CONFIG_ESP_WIFI_NVS_ENABLED=n
CONFIG_LWIP_MAX_SOCKETS=16
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=23040
CONFIG_LWIP_TCP_WND_DEFAULT=11520
CONFIG_LWIP_TCP_RECVMBOX_SIZE=16
CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y

CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y
CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA=y
CONFIG_MBEDTLS_DYNAMIC_FREE_CA_CERT=y
CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE=n
CONFIG_MBEDTLS_SSL_RENEGOTIATION=n

CONFIG_HTTPD_MAX_REQ_HDR_LEN=2048
CONFIG_HTTPD_MAX_URI_LEN=1024

CONFIG_ESP_LCD_TOUCH_MAX_POINTS=1

CONFIG_LV_COLOR_DEPTH_16=y
CONFIG_LV_USE_BUILTIN_MALLOC=y
CONFIG_LV_MEM_SIZE_KILOBYTES=1024
CONFIG_LV_USE_CLIB_STRING=y
CONFIG_LV_USE_CLIB_SPRINTF=y
CONFIG_LV_DEF_REFR_PERIOD=15
CONFIG_LV_OS_NONE=y
CONFIG_LV_DRAW_SW_COMPLEX=y
CONFIG_LV_USE_DRAW_SW_COMPLEX_GRADIENTS=y
CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y
CONFIG_LV_USE_LOG=y
CONFIG_LV_LOG_LEVEL_WARN=y
CONFIG_LV_LOG_PRINTF=n
CONFIG_LV_USE_OBJ_NAME=y
CONFIG_LV_USE_OBSERVER=y
CONFIG_LV_USE_SNAPSHOT=y
CONFIG_LV_USE_QRCODE=y
CONFIG_LV_USE_SYSMON=y
CONFIG_LV_USE_PERF_MONITOR=y
CONFIG_LV_USE_MEM_MONITOR=y
CONFIG_LV_FONT_MONTSERRAT_14=y
CONFIG_LV_FONT_DEFAULT_MONTSERRAT_14=y
CONFIG_LV_USE_FONT_COMPRESSED=n
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

CONFIG_RA_LCD_PCLK_MHZ=80
CONFIG_RA_LVGL_BUF_LINES=40
CONFIG_RA_LCD_EVEN_ROUNDER=n
CONFIG_RA_DEVCONSOLE=y
CONFIG_RA_SELFTEST_FORCE_FAIL=n
CONFIG_RA_SELFTEST_FORCE_PANIC=n
```
- The last block is defined in `main/Kconfig.projbuild`, with int ranges 10..80 and 20..90.
- LVGL logging goes through `lv_log_register_print_cb()` → `ESP_LOGW("LVGL")`, which keeps console lines intact.
- The perf overlay is hidden at boot with `lv_sysmon_hide_performance()` unless `dev_flags` bit 0 is set.
- `sdkconfig.defaults.release` contains:
  ```
  CONFIG_LV_USE_SYSMON=n
  CONFIG_LV_USE_PERF_MONITOR=n
  CONFIG_LV_USE_MEM_MONITOR=n
  CONFIG_LOG_MAXIMUM_LEVEL_INFO=y
  ```
  Build with `-B build-rel -D SDKCONFIG=build-rel/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.release"`.

---

## 5. `partitions.csv` (16 MB, the repo's current table, kept unchanged from M1 on)

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
Check:
- Bootloader occupies 0x0–0x8000 and the table sits at 0x8000.
- App offsets are 64 KB aligned. otadata is exactly 0x2000. 0x12000–0x1FFFF is padding.
- 0xFE0000 + 0x20000 = 0x1000000.
- NVS is 6 pages (one is kept free for garbage collection). The 128 KB coredump fits an ELF dump of about 20 tasks.

The table never changes again after M1. Changing it would need a full flash and could relocate NVS.

---

## 6. `main/idf_component.yml` and CMake glue

```yaml
## Exact pins (they satisfy the decided ranges ^2.0.2 / ~1.1.2 / ~2.0.3 / ~2.9 / 9.5.x).
## Bump one at a time, rebuild, run tests, commit dependencies.lock.
dependencies:
  idf:
    version: ">=6.1.0,<6.2.0"
  espressif/esp_lcd_st77916: "==2.0.2"
  espressif/esp_lcd_touch_cst816s: "==1.1.2"
  espressif/esp_lcd_touch: "==1.2.1"
  espressif/esp_io_expander_tca9554: "==2.0.3"
  espressif/esp_lvgl_port: "==2.9.0"
  lvgl/lvgl: "==9.5.0"
  espressif/cjson: "~1.7.19"
  espressif/button: "==4.2.2"
  # later, only if AAC upload or MP3 TTS is needed:
  # espressif/esp_audio_codec: "==2.6.2"
```
The current `dependencies.lock` already resolves lvgl 9.5.0, esp_lvgl_port 2.9.0, esp_lcd_st77916 2.0.2, esp_lcd_touch 1.2.1, cst816s 1.1.2, tca9554 2.0.3, esp_io_expander 1.2.1 and cmake_utilities 0.5.3. Commit it (registry-only dependencies, no Kconfig rules).

Project `CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(round_assistant)   # PROJECT_VER = git describe --tags --dirty (no version.txt)
```

`main/CMakeLists.txt` (tail): puts the LVGL pool in PSRAM, as xiaozhi does:
```cmake
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_include_directories(${lvgl_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/ui")
target_compile_definitions(${lvgl_lib} PRIVATE "LV_MEM_POOL_INCLUDE=\"lvgl_psram_pool.h\"")
```
`main/ui/lvgl_psram_pool.h` defines `LV_MEM_POOL_ALLOC(size)` as `heap_caps_malloc(size, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)`. `lv_mem_core_builtin.c` (v9.5.0) calls it in `lv_init()`.

`.gitignore` additions: `sdkconfig`, `build-rel/`, `releases/`, `tests/out/`.

---

## 7. Dev console protocol (USB-Serial-JTAG, COM7)

**Transport.**
- CDC-ACM; the baud rate is ignored. UTF-8 text lines ending in LF (CR LF accepted). Command lines are ≤ 1024 B, data lines ≤ 4200 B.
- The device uses the driver API (`usb_serial_jtag_read_bytes`), not linenoise, so there is no echo or line editing.
- A writer mutex makes all output line-atomic. `esp_log_set_vprintf()` routes ESP_LOG through the same writer. When `!usb_serial_jtag_is_connected()`, writes time out after 50 ms and are dropped, so a battery-powered device never stalls.
- Open the port from the PC with `dtr=False, rts=False` set **before** `open()`, to avoid resetting the chip.

**Framing.**
```
PC  → #<id> <command> [args…]            id 1..65535; args split by esp_console_split_argv (double quotes, \ escapes)
DEV → @<id> OK <json>                     single-line compact JSON
DEV → @<id> ERR <esp_err_name> {"msg":"…"}
DEV → @<id> BEGIN <kind> {"w":360,"h":360,"fmt":"rgb565le","enc":"b64","bytes":259200,"crc32":"1a2b3c4d"}
DEV → @<id> D <seq> <base64>              3072 raw bytes → 4096 chars per line
DEV → @<id> END {"bytes":259200,"crc32":"1a2b3c4d"}
DEV → @! EVT {"t":12345,"ev":"state","from":"thinking","to":"speaking","turn":7}
DEV → @! READY {"ver":"m1-3-g1a2b3c","reset":"poweron","boot":42,"part":"ota_0"}
anything not starting with '@' = log line (shown, ignored by parsers)
```
Uploads (`mic load`, `ota push`):
1. PC sends `#9 mic load bytes=96044 crc32=…`.
2. Device answers `@9 READY {"chunk":3072,"window":8}`.
3. PC sends `D <seq> <b64>` lines and waits for `@9 ACK <seq>` after every 8 lines.
4. PC sends `END`; device answers `@9 OK {…}`. `ABORT` cancels; 5 s of silence aborts.

CRC is IEEE CRC-32 over the raw bytes. `crc 123456789` must return `cbf43926`; the PC tool checks this on connect.

**Commands.**

| Command | Purpose / response |
|---|---|
| `help`, `ping` | `{"uptime_ms":…}` |
| `info` | `ver, idf, part, ota_state, reset, boot_ms, panel_id "00027f7f", panel_init "new"/"default", pclk_mhz, touch_ok, touch_id, exio_ok, psram_kb, flash_mb, mac, gpio7, bl, selftest, safe_mode` |
| `state` | `{"state","mode","text_only","turn","net":{state,ssid,rssi,ip,portal},"ai":{upload_ms,ttfb_ms,total_ms,last_err},"tts":{rate,ttfb_ms,underruns},"ui":{"screen","fps_x10"}}` |
| `heap` | `int:{free,min,largest}, dma:{free,largest}, psram:{free,min,largest}, lvgl:{total,free,biggest,frag_pct}` (`heap_caps_*`, `lv_mem_monitor`) |
| `tasks` | `uxTaskGetSystemState` → `name, core, prio, state, hwm_bytes, cpu_pct` |
| `perf [on\|off\|dump]` | `display_stats` (fps_x10, render_us_avg/max); on/off toggles the sysmon overlay |
| `shot [fmt=raw\|rle] [x y w h]` | `ui_lock` → `lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, buf)` into a persistent 259,200 B PSRAM buffer (`lv_draw_buf_init`), then BEGIN/D/END. RGB565 little-endian, stride w·2, top-left first. `rle`: control byte c < 128 → c+1 literal pixels; c ≥ 128 → (c−126) repeats of the next pixel; CRC is over decoded bytes |
| `tap x y [ms=80]`, `press x y`, `move x y`, `release`, `swipe x0 y0 x1 y1 [ms=300] [steps=15]`, `longpress x y [ms=800]` | `touch_inject_*` → `@id OK {}` when done |
| `touch watch <s>` | streams `@! EVT {"ev":"touch","x":…,"y":…,"p":1}` from the real panel |
| `ui screens` / `ui goto <scr>` / `ui tree` / `ui get <name> <prop>` / `ui tap <name>` / `ui text <name> "<utf8>"` / `ui kb "<utf8>"` / `ui audit` | `ui tap` uses the object's centre from `lv_obj_get_coords`. `ui kb` maps each character to a key via `lv_buttonmatrix_private.h` `button_areas[]` and injects real taps, including page and language switches. `ui audit` returns violations |
| `bl <0-100>`, `lcd inv <0\|1>` | backlight / inversion |
| `btn boot\|pwr click\|double\|long\|down\|up` | simulated physical buttons (same callbacks) |
| `get <key>`, `set <key> <value>`, `settings [json]`, `settings reset [keep_wifi]` | secrets come back masked |
| `mic probe` | `{"right":{"rms_dbfs":-62.1,"zero":0.01},"left":{…}}` |
| `mic rec <ms>` / `mic dump` | record from the current source / download the last clip as WAV (BEGIN/D/END) |
| `mic load …` / `mic src mic\|inject [fast]` / `mic play` | upload WAV (16 kHz mono s16) / select source / run a full turn: PTT_DOWN, then PTT_UP at the end of the buffer |
| `spk tone <hz> <ms> [vol] [rate]`, `spk stat [reset]`, `spk vol <n>` | audio_out tests |
| `ptt down\|up\|toggle`, `cancel`, `mode assistant\|translator`, `textonly 0\|1`, `ask "<text>"`, `say "<text>"` | app events / text turn / TTS only |
| `ai last-req`, `ai models [page]`, `ai key` | redacted request prefix / picker data / per-key limits |
| `wifi scan`, `wifi connect <ssid> <pass>`, `wifi disconnect`, `wifi status`, `portal start\|stop` | net |
| `net get <https-url>` | HTTPS smoke test: `{"status":200,"ms":…,"bytes":…}` |
| `ota info`, `ota push bytes=N crc32=X`, `ota url <https-url>`, `ota boot ota_0\|ota_1`, `ota rollback`, `ota mark valid\|invalid` | §8 |
| `log <tag\|*> <none\|error\|warn\|info\|debug>` | `esp_log_level_set` |
| `crc <text>` | CRC self-check |
| `reboot [dl]` | `esp_restart()`. `dl` writes `RTC_CNTL_FORCE_DOWNLOAD_BOOT` into `RTC_CNTL_OPTION1_REG` first, which enters ROM download mode without pressing BOOT |

Events emitted: `boot, ready, selftest, state, screen, ui_click(name), touch, rec(ms,rms), ai(turn,phase,ms), tts(rate,ttfb), audio_drained, net, error(code,msg), ota(pct), heap_low`.

**PC side.** `tools/devcon.py` uses pyserial, which is already in the IDF venv as an esptool dependency, so nothing extra is installed.
- `Device(port)` starts a reader thread and routes lines into per-id queues and an event list.
- Methods: `cmd(line, timeout) → dict`, `shot(path, mask=True)`, `upload(kind, data)`, `wait_event(pred, timeout)`, `wav_prepare(path)` (stdlib `wave`, downmix, linear resample to 16 kHz).
- PNG writer in pure stdlib (`zlib`, `struct`). RGB565 expands to RGB888 as `(c<<3)|(c>>2)` for 5-bit and `(c<<2)|(c>>4)` for 6-bit channels. With `--mask`, pixels outside r=180 get alpha 0.

---

## 8. Boot sequence, self-test, rollback runbook

### 8.1 Boot (targets: UI visible < 1.5 s, self-test verdict < 3 s)
0. Bootloader hook: GPIO7 = 1. Then rollback logic: a `PENDING_VERIFY` image found at boot becomes `ABORTED` and the previous slot runs.
1. `app_main`: `board_power_latch()`. Record `esp_reset_reason()`, `esp_app_get_description()`, running partition and `esp_ota_get_state_partition()`.
2. `RTC_NOINIT_ATTR` crash counter. +1 if the reset reason is PANIC/INT_WDT/TASK_WDT and the stage is below `READY`. At 3 or more, and only when not `PENDING_VERIFY`, enter **safe mode**: devconsole plus a minimal UI, no Wi-Fi, audio or AI.
3. `util_init()`, `settings_init()` (NVS), `devconsole_start()`. The console comes this early so a device with a later failure is still reachable.
4. `board_init()` → `board_reset_lcd_touch()` → `display_init()` → `touch_init()` → `ui_init()` (boot screen). Backlight fades in after the first frame.
5. `audio_out_init()`, `audio_in_init()` (channels created, disabled).
6. `ota_selftest_and_confirm(3000)` (below). Runs under an `esp_timer` guard of 10 s: if the self-test hangs, `esp_restart()` is called, which leads to rollback.
7. `net_init()` and `net_start()` (non-blocking), `ai_client_init()`, `tts_client_init()`, `app_start()`, `ui_post(UI_GOTO home)`, `@! READY`. `app_main` returns.

### 8.2 Checks run before `esp_ota_mark_app_valid_cancel_rollback()`

| Check | Fatal |
|---|---|
| NVS: write and read back `sys/boot_cnt` | yes |
| TCA9554 input register readable | yes |
| ST77916 panel init returned ESP_OK; ID read OK and known pattern | init: yes; ID: warn |
| LVGL alive: at least 1 `LV_EVENT_REFR_READY` and at least 10 lv_timer ticks within 500 ms | yes |
| I2S0 and I2S1 channels created | yes |
| CST816 created (ID read) | no (warn; BOOT button and devconsole still work) |
| `esp_wifi_init` OK (association **not** required) | yes |
| Internal free ≥ 100 KB, DMA free ≥ 24 KB, PSRAM free ≥ 6 MB at this point | yes |
| devconsole task running | yes |
| `CONFIG_RA_SELFTEST_FORCE_FAIL` / `FORCE_PANIC` | test hooks |

- **Pass:** if `PENDING_VERIFY`, call `esp_ota_mark_app_valid_cancel_rollback()`; store `sys/ok_ver`; emit `EVT selftest ok`.
- **Fail:** store `sys/rb_reason`; if `PENDING_VERIFY`, call `esp_ota_mark_app_invalid_rollback_and_reboot()`. Otherwise (a USB-flashed image) show the failure on screen and stay up for debugging.
- Soft stage 2, up to 60 s after boot: if credentials exist and STA associates but 3 HTTPS attempts to `ai_url` fail, the UI offers "Откатить прошивку?". The button calls `ota_rollback_now()`, which works because the previous slot stays intact until the next OTA.

### 8.3 Developer rollback runbook

| Situation | Action |
|---|---|
| Milestone done | `git tag -a m<N>-<name> -m "…"` (via `tools\release.ps1 -Tag m1-display-touch`, which also archives `releases\<tag>\` with bootloader.bin, partition-table.bin, ota_data_initial.bin, round_assistant.bin/.elf/.map, flash_args, sdkconfig, dependencies.lock, SHA256SUMS.txt) |
| Go back to a tag (source) | `git switch --detach <tag>` → `.\tools\idf.ps1 build` → `.\tools\idf.ps1 -p COM7 flash` |
| Go back to a tag (binary, no build) | `.\tools\restore.ps1 -Tag <tag>` → `cd releases\<tag>; esptool --chip esp32s3 -p COM7 -b 921600 write-flash "@flash_args"`. NVS is preserved |
| A/B try a build without losing the current one | `.\tools\slot.ps1 -Write build\round_assistant.bin -Slot 1 -Boot` → `otatool.py -p COM7 write_ota_partition --slot 1 --input …`, then `erase_otadata`, then `switch_ota_partition --slot 1`. Back: `.\tools\slot.ps1 -Boot 0` |
| Exercise the real OTA + rollback path over USB | `devcon.py ota-push build\round_assistant.bin` (esp_ota API: image boots in `PENDING_VERIFY`) |
| Settings corrupt | `devcon.py cmd "settings reset"`, or `parttool.py -p COM7 erase_partition --partition-name=nvs` |
| Crash analysis | `.\tools\idf.ps1 -p COM7 coredump-info` (coredump partition, 128 KB) |
| App broken, USB still enumerates | `.\tools\idf.ps1 -p COM7 flash` (esptool resets through USB-JTAG), or `devcon.py cmd "reboot dl"` |
| Nothing responds / boot loop | Download mode: **hold BOOT, tap RESET, release BOOT** → ROM USB-JTAG-serial (VID 303a). Then `flash` or `restore.ps1` |
| Return to factory firmware | `esptool --chip esp32s3 -p COM7 -b 921600 write-flash 0 backups\factory_full_16MB.bin` (SHA-256 `90151CB9…2AE9`). This overwrites our NVS. Come back with a full `flash` plus `secrets_push.py` |

Notes:
- `idf.py flash` rewrites otadata (`ota_data_initial.bin`), so it always boots ota_0. `idf.py app-flash` writes only ota_0. If otadata currently selects ota_1, an `app-flash` has no visible effect.
- `otatool switch_ota_partition` writes only seq+CRC into the inactive otadata sector and keeps that sector's stale `ota_state` word. 0xFFFFFFFF (UNDEFINED) counts as valid, so there is no rollback protection for manual switches. A stale ABORTED/INVALID state can make the bootloader ignore the switch, which is why `slot.ps1` runs `erase_otadata` first.
- USB-flashed images are never `PENDING_VERIFY`. Bootloader rollback only guards images written through `esp_ota_*`.
- Never burn eFuses: no flash encryption, secure boot or HMAC NVS encryption. That keeps every state above recoverable.

---

## 9. Windows build / flash workflow (EIM, `C:\esp`, v6.1)

Facts on this PC:
- EIM is `C:\esp\tools-eim\eim.exe` and IDF is `C:\esp\v6.1\esp-idf`.
- `C:\Espressif\tools\eim_idf.json` lists the install `name: "v6.1"` with `activationScript: C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1` and Python `C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe`.
- EIM names the activation script `Microsoft.{version}.PowerShell_profile.ps1` in `activation_script_path_override`, which defaults to `C:\Espressif\tools`. A matching `…PowerShell_deactivate.ps1` sits next to it.
- The profile defines the global functions `idf.py`, `esptool`, `otatool.py` and `parttool.py`.

Activation:
```powershell
# interactive PowerShell session
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
# non-interactive one-shot (agent, CI, Git Bash) - wrapper removes MSYSTEM, which makes idf.py abort under Git Bash
powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\krupo\esp32-ai-assistant\tools\idf.ps1 build
# EIM alternative: sources the activation script of the given version, runs one command in the current directory
Set-Location C:\Users\krupo\esp32-ai-assistant; & 'C:\esp\tools-eim\eim.exe' run "idf.py build" v6.1
```

Commands (from the repo root):
```powershell
Remove-Item .\sdkconfig; .\tools\idf.ps1 reconfigure      # after editing sdkconfig.defaults
.\tools\idf.ps1 build
.\tools\idf.ps1 size                                       # and size-components
.\tools\idf.ps1 -p COM7 -b 921600 flash                    # bootloader + table + otadata + ota_0
.\tools\idf.ps1 -p COM7 -b 921600 app-flash                # ota_0 only (see §8.3 caveat)
.\tools\idf.ps1 -p COM7 storage-flash                      # spiffs assets, only when changed
.\tools\idf.ps1 -p COM7 monitor                            # humans only: needs a TTY, Ctrl+] exits
.\tools\py.ps1 tools\devcon.py -p COM7 log --seconds 20    # agent-friendly log capture
```
Only one program can hold COM7 at a time: close `monitor` or `devcon.py` before flashing.

Helper scripts to create:

| Script | Behaviour |
|---|---|
| `tools\idf.ps1` (exists) | dot-sources the activation script quietly, sets `$ErrorActionPreference='Continue'`, removes `Env:MSYSTEM`, runs `idf.py @args` from the repo root, returns its exit code |
| `tools\py.ps1` | `& 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' @args` (pyserial available) |
| `tools\build.ps1 [-Release]` | build to `build\` or `build-rel\` with the defaults chain; parse `idf.py size`; fail if the app is over 85 % of 0x400000 |
| `tools\flash.ps1 [-Port COM7] [-AppOnly]` | flash, then `devcon.py wait-ready --timeout 10`, then print `info` |
| `tools\release.ps1 -Tag <t>` | require a clean tree; `git tag -a`; build; archive to `releases\<t>\` + SHA256SUMS |
| `tools\restore.ps1 -Tag <t> \| -Factory [-EraseNvs]` | esptool `write-flash "@flash_args"` from the archive, or the factory image |
| `tools\slot.ps1 -Write <bin> -Slot 0\|1 [-Boot]` / `-Boot 0\|1` / `-Info` | otatool `write_ota_partition`, `erase_otadata` + `switch_ota_partition`, `read_otadata` |
| `tools\devcon.py` | library + CLI: `cmd`, `shot out.png [--mask]`, `tap`, `swipe`, `ui-tap name`, `wav in.wav`, `mic-dump out.wav`, `events --until state=idle`, `log`, `ota-push bin`, `wait-ready`, `reset` (`esptool --after hard-reset chip-id`) |
| `tools\secrets_push.py secrets.json` | `{"wifi":[{"ssid","pass"}],"ai_key":"…"}` → `set` / `wifi connect`; the file is gitignored |
| `tools\mkfonts.ps1` | runs `npx lv_font_conv …` (Node required, run manually); writes `main\ui\fonts\*.c` (committed) |
| `tests\acceptance.py -m m1..m6 [-p COM7]` | runs §10 tests and writes `tests\out\<ts>\report.json` and screenshots |

---

## 10. Milestones and acceptance tests (all through the dev console; a "finger" note means a touch is needed but no one has to look at the screen)

**M1: display + touch + console + rollback skeleton** (tag `m1-display-touch`)

| # | Test | Pass |
|---|---|---|
| 1.1 | `info` | `panel_id ∈ {007f7f7f, 00027f7f}`, `panel_init` matches it, `pclk_mhz=80`, `exio_ok`, `gpio7=1`, `selftest=ok` |
| 1.2 | `ui goto test_pattern`, `shot` | CRC OK. 8 vertical bars, each 45 px (white, yellow, cyan, green, magenta, red, blue, black), have exact RGB565 values at the bar centres. This proves snapshot and byte order |
| 1.3 | `ui goto test_anim` for 5 s, `perf` | `fps_x10 ≥ 300` for the orb animation; `render_us_max < 30000` |
| 1.4 | `ui goto test_touch` (9 named buttons), `ui tap tt_5` | `EVT ui_click tt_5`; `ui get tt_count text == "1"` |
| 1.5 | `swipe 180 300 180 80 300` on `tt_list` | `ui get tt_list scroll_y > 100` |
| 1.6 | `info.touch_ok` (+ finger: `touch watch 10`, tap the centre) | true; reported point within ±20 px of (180,180) → orientation correct |
| 1.7 | `heap`, `tasks` | int.free ≥ 150 KB (no Wi-Fi yet), dma.free ≥ 24 KB, every task HWM ≥ 512 B |
| 1.8 | `reboot` × 5 | `@! READY` within 3 s each time; same panel_id |
| 1.9 | `ui audit` on test screens | no violations |
| 1.10 | One-time human photo of `test_pattern` (the panel's own output cannot be read back) | correct colours, no stripes. Otherwise toggle `lcd inv`, try 40 MHz, enable the rounder |

**M2: audio in/out** (tag `m2-audio`)

| # | Test | Pass |
|---|---|---|
| 2.1 | `mic probe` | active slot rms > −75 dBFS and zero < 0.5; other slot zero > 0.95. The script writes `mic_slot` |
| 2.2 | `mic rec 3000`, `mic dump` | WAV 96,044 B, 16 kHz mono, CRC OK, not all zeros, peak < 32767 (no clipping in a quiet room) |
| 2.3 | `wav tests/audio/ru_hello_16k.wav`, `mic src inject fast`, `mic rec 2000`, `mic dump` | bit-exact match with the injected samples |
| 2.4 | `spk tone 1000 2000 50`, `spk stat` | bytes_played = 2 s × 24000 × 4 ± 1 DMA buffer, underruns 0, writes_err 0 |
| 2.5 | tones at rate 16000 / 44100 / 24000 | `spk stat.rate` follows; no error |
| 2.6 | 30 s tone during `test_anim` | fps drop ≤ 20 %, underruns 0 |
| 2.7 | `heap` | PSRAM shows the 2 × 960,044 B record buffers; internal unchanged within ±4 KB |

**M3: Wi-Fi + settings + keyboard + portal** (tag `m3-net-settings`)

| # | Test | Pass |
|---|---|---|
| 3.1 | for every row of `settings json`: `set` boundary values and out-of-range values, `get`, `reboot`, `get` | values persist; out-of-range → ERR; secrets masked (the response never contains the full value) |
| 3.2 | `wifi connect` (from secrets.json) | `EVT net up` ≤ 15 s; `state.net.ip` set; rssi present |
| 3.3 | `wifi scan` | ≥ 1 AP |
| 3.4 | `net get https://openrouter.ai/api/v1/models?limit=1` | status 200 (TLS with the bundle) |
| 3.5 | `ui goto kb_test`, `ui kb "привет"`, `ui kb "Hello 123"`, backspace | `ui get kb_ta text` equals the expected string |
| 3.6 | `set dev_flags 4`, `portal start`; PC GET `http://<sta_ip>/` and POST `/save ai_model=test/model` | 200 with the form; `get ai_model == test/model`; key unchanged when left blank |
| 3.7 | PC sends a UDP DNS query to `<sta_ip>:53` for `example.com` | A record 192.168.4.1 |
| 3.8 | `ui get portal_qr data` | `WIFI:T:WPA;S:RoundAI-XXXX;P:<8 digits>;;` |
| 3.9 | `heap` with STA up and the portal running | int.free ≥ 48 KB |

**M4: assistant AI + TTS** (tag `m4-assistant`)

| # | Test | Pass |
|---|---|---|
| 4.1 | `ask "Скажи одно слово: тест"` | `EVT ai done` ≤ 15 s; non-empty reply; then state idle |
| 4.2 | inject `ru_capital_question.wav` ("Какая столица Франции?"), `mic play` | events in order listening → uploading → thinking → idle; transcript contains "столиц"; reply contains "Париж" |
| 4.3 | `state.ai` timings over 5 runs | upload < 3 s for a 3 s clip; ttfb < 6 s (logged to CSV) |
| 4.4 | `textonly 0`, `say "Проверка связи"` | `tts.rate=24000`, bytes ≥ 24 KB, underruns 0, `EVT audio_drained` |
| 4.5 | voice turn with TTS: `EVT tts` before `EVT ai done` | first audio arrives before the reply ends (sentence pipelining) |
| 4.6 | `cancel` right after `EVT uploading`, ×20 | idle ≤ 500 ms; no events with the old turn id; int.free within ±8 KB of baseline |
| 4.7 | `ptt down` during speaking | listening ≤ 300 ms; `spk stat.queued_bytes = 0` |
| 4.8 | `set ai_key sk-invalid` → `ask`; `ai_model nonexistent/x`; `wifi disconnect` → `ask` | error 401 / 404 / "offline"; `ui get err_label text` non-empty; idle after 4 s |
| 4.9 | inject a 30 s WAV | success or 413 handled; no allocation failure |
| 4.10 | 3 turns, then `ui tree` + `ui audit` | 6 bubbles; nothing outside r=174; targets ≥ 48 px |
| 4.11 | soak: 50 text turns | no reboot (uptime monotonic); int.min ≥ 32 KB |

**M5: translator** (tag `m5-translator`)

| # | Test | Pass |
|---|---|---|
| 5.1 | `mode translator`, `set tr_lang_a ru`, `set tr_lang_b en`; inject "Доброе утро" | `EVT ai src_lang=ru`; translation contains "morning"; `state.tts.voice` starts `en-` (when voice is on) |
| 5.2 | inject "Where is the train station?" | src_lang=en; translation contains "вокзал"; voice `ru-RU-…` |
| 5.3 | `set tr_lang_a de`, `ai last-req` | schema enum `["de","en"]`; no audio or key in the output |
| 5.4 | `ui tap tr_swap` | `tr_lang_a` and `tr_lang_b` swapped in settings |
| 5.5 | `tr_vad 1`, 2 s speech + 2 s silence WAV, `mic play` without `ptt up` | `EVT rec` stops by silence ≤ 1.3 s after speech ends |
| 5.6 | tests 4.6–4.8 repeated in translator mode | same pass criteria |

**M6: polish + OTA** (tag `m6-release`, `v1.0.0`)

| # | Test | Pass |
|---|---|---|
| 6.1 | `ota-push build\round_assistant.bin` | reboot; `ota info` running=ota_1; `EVT selftest ok`; state valid |
| 6.2 | push a `RA_SELFTEST_FORCE_FAIL=y` build | automatic rollback; running=previous slot; `rb_reason` set |
| 6.3 | push a `RA_SELFTEST_FORCE_PANIC=y` build | bootloader rollback (ABORTED); previous slot runs |
| 6.4 | `ota url https://…/round_assistant.bin` | progress events to 100 %; boots and is marked valid |
| 6.5 | `ui audit` on every screen; golden `shot` per screen | 0 violations; pixel difference vs golden < 2 % |
| 6.6 | `info.boot_ms`, time to `EVT selftest` | < 1500 ms / < 3000 ms; home orb ≥ 30 fps |
| 6.7 | flash the m5 release over the m6 NVS, then back | settings readable both ways (`cfg_ver` migration) |
| 6.8 | `btn pwr long` | `EVT power_off` logged (stays on when powered from USB) |

Every milestone ends with `tests\acceptance.py -m mN` all green, then `tools\release.ps1 -Tag mN-…`.
