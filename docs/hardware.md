# Hardware reference — Waveshare ESP32-S3-Touch-LCD-1.85 (rev 2.1)

Compiled 2026-10-17 from the Waveshare docs (docs.waveshare.com/ESP32-S3-Touch-LCD-1.85),
the archived official schematic (2024-09-18), Espressif ESP32_Display_Panel v1.0.4 board file,
xiaozhi-esp32 board config and Waveshare demo code; each fact cross-checked by a second agent.

> Not to be confused with the **1.85C** (touch on GPIO10/11, ES8311 codec on V2) or the **1.85B**.
> Code for those boards is not drop-in for this one.

## Chip readout (esptool, this unit)

ESP32-S3R8 (QFN56) rev v0.2, 8 MB in-package **octal** PSRAM (3.3 V), 16 MB external quad flash
(JEDEC 0x20/0x4018 — second-source part, schematic says W25Q128JVSI), 40 MHz crystal,
native USB-Serial/JTAG on **COM7** (MAC address omitted: it is unique per unit).

## Onboard parts

| Function | Part | Bus |
|---|---|---|
| Display | 1.85" round IPS 360×360, ST77916 | QSPI |
| Touch | CST816S (addr 0x15) | I2C1 on GPIO1/3 (own bus, no PCB pull-ups → enable internal) |
| IO expander | TCA9554PWR (addr 0x20) | I2C0 on GPIO10/11 |
| RTC | PCF85063ATL (addr 0x51), INT → GPIO9 | I2C0 |
| IMU | QMI8658 (addr 0x6B) | I2C0 |
| Speaker DAC | PCM5101A — no MCLK, XSMT tied high (always unmuted), no I2C | I2S0 |
| Speaker amp | APA2068KAI (stereo), 4-pin speaker connector J10 (VORN/VORP/VOLP/VOLN), 8 Ω 2 W | — |
| Microphone | ICS-43434 I2S MEMS (rev2.x; older boards MSM261S4030H0R) | I2S1 |
| TF card | SDMMC 1-bit only (D1/D2 not wired) | SDMMC |
| Charger / LDO | ETA6098 / ME6217C33M5G (800 mA) | — |
| Battery header | MX1.25 2-pin for a 3.7 V Li-ion cell, charge and discharge (Waveshare wiki); a separate header takes a rechargeable RTC cell | — |
| Antenna | ceramic + IPEX1 (resistor-selectable) | — |

## Pin map

| Signal | GPIO |
|---|---|
| LCD QSPI CS / SCK | 21 / 40 |
| LCD QSPI D0 / D1 / D2 / D3 | 46 / 45 / 42 / 41 |
| LCD TE | 18 |
| LCD backlight (active high, PWM) | 5 |
| LCD reset | **EXIO2** (TCA9554 P1) |
| Touch SDA / SCL / INT | 1 / 3 / 4 |
| Touch reset | **EXIO1** (TCA9554 P0) |
| Internal I2C SCL / SDA (+ 4-pin header, 10 k pull-ups) | 10 / 11 |
| RTC INT | 9 |
| IMU INT1 / INT2 | EXIO5 / EXIO4 (configure as inputs if used) |
| Speaker I2S BCLK / LRCK / DOUT | 48 / 38 / 47 |
| Mic I2S SCK / WS / SD | 15 / 2 / 39 |
| SD CLK / CMD / D0 | 14 / 17 / 16 |
| SD D3/CS | EXIO3 (pulled high, leave high for SDMMC) |
| Battery voltage (ADC1 ch7, ×3 divider) | 8 |
| BOOT button (active low) | 0 |
| PWR button input (active low) | 6 |
| Power latch (drive HIGH to stay on when on battery) | 7 |
| UART header TX / RX (free GPIO) | 43 / 44 |
| Test pads TP1 / TP2 (free GPIO, no header) | 12 / 13 |
| USB D- / D+ | 19 / 20 |
| **Do not use** — octal PSRAM | 33–37 |

EXIOn = TCA9554 port P(n-1). Output register defaults to 0xFF.

## Gotchas

1. **Two LCD panel batches.** Read ST77916 register 0x04 at boot: `00 7F 7F 7F` → default init,
   `00 02 7F 7F` → `vendor_specific_init_new` (184 commands; in xiaozhi-esp32
   `main/boards/waveshare/esp32-s3-touch-lcd-1.85/` and ESP32_Display_Panel ≥ v1.0.4).
   Wrong table → stripes / wrong colours. Some units also need colour inversion.
2. **QSPI clock.** Espressif's board file runs 80 MHz; one user with a Rev2.0 needed to drop to 10 MHz
   with a generic driver. Start at 40 MHz, measure.
3. **Mic slot.** Reference code reads I2S1 Philips 32-bit, **right** slot, 16 kHz. If silent on rev 2.1 try left.
4. **Speaker** works with plain I2S std (Philips, 16-bit); stereo amp, so mono must be duplicated to both slots.
5. **Power latch.** On battery the board turns off unless GPIO7 is driven high early in boot.
6. **Memory.** Waveshare FAQ: Wi-Fi + BT + audio together run out of internal RAM — keep BT off, put
   big buffers in PSRAM.
7. **Toolchain.** Waveshare tests ESP-IDF ≥ 5.3.1 (FAQ: if boot-looping, try 5.3.2); install paths must
   be ASCII without spaces.
8. **Download mode** if auto-reset fails: hold BOOT, tap RESET, release BOOT.
9. Strapping pins in use: GPIO0 (BOOT), GPIO3 (TP_SCL), GPIO45/46 (LCD D1/D0) — no external pull-ups.
10. **No charge status to the MCU.** The ETA6098 drives only its charge LED (no status pin, no VBUS sense reaches
    the MCU), and the system load hangs on the cell: USB power is not the same as charging. Measured on this unit on
    a PC port (~0.5 A): idle, the ×3 divider read 4083 → 4086 mV over 40 s and barely rose, the LED green; with the
    speaker loud (speech boost, volume 100) the board takes more than the port gives and the cell sags and drains.
    User report (v0.7): "on different sources it does not charge but the green LED is on; on a MacBook it charges
    for a couple of seconds and stops". Likely: the cell was nearly full and the divider reads ~50-100 mV low, so the
    charger terminated (or never started) and its LED showed its full / standby state; model v2 showed "charging"
    only because a USB host was there, needed a 2-min rise that a full cell never makes on a wall charger, and lost
    the power when macOS stopped the start-of-frame packets of the idle CDC port (or the charger stopped at once).
    **Model v3** (docs/ui.md 3.16, `components/board/battery.c`): external power = the USB host
    (`usb_serial_jtag_is_connected()`), or a **voltage step**: a settled reading 35 mV off the median of the settled
    run just before it (consecutive settled readings ≤ 30 s apart), confirmed by the next two taken 2 s apart, so a
    wall charger is seen 5-9 s after plugging in. A step down without the host = unplugged; the host's packets
    stopping without one = still plugged in (`kept`). Charging or full: the plug-in's step against the level before
    (under 15 mV near v_full = the charger did not start: FULL at once), then the settled level: flat for 4 min
    (≤ +5 mV, the highest settled reading of the last minute against that of the minute 4-5 min before) near v_full,
    or for 15 min at ≥ 3.95 V, = FULL, whatever the absolute voltage. With the host, the settled level falling
    near v_full with no long session (≥ 60 s of skipped readings) in that time is the charger stopping
    (terminated: the cell relaxes 30-80 mV and the load moves onto it) = FULL too, and FULL holds while the load
    drains the cell to the charger's recharge level (down to v_full − 160 mV); a fall across a long session is
    USB_DRAIN at any level (this unit's PC port with the speaker at volume 100).
    The CV level is this divider's reading of a full cell on the charger (the highest it reads there): it is learned
    as **v_full** (NVS `bat_vfull`, default 4150, accepted 4000-4250) once a FULL has held 15 min, from its highest
    settled level — lower only after a 4-min plateau and by ≤ 30 mV per charge, higher from any FULL — and the
    discharge curve is scaled by 4200 / v_full, so 100 % is what this unit reads when full (at rest a full cell
    reads ~v_full − 30 mV ≈ 97 %). A divider reading more than ~100 mV low is not learned downwards: `set bat_vfull`.
    Readings are not taken while the speaker plays (and 15 s after), during a turn (and 20 s after) or right after
    any backlight change (on battery at ≤ 20 % they are, for the low-battery toasts), and no step is measured
    across a load step (also Wi-Fi joining or leaving, the portal); USB_DRAIN = the host there and the settled
    level 20 mV below 3 min ago (or 30 mV below its peak since charging began) under v_full − 120 mV or across a
    long session, or FULL's level under v_full − 160 mV. Leaving any state for BATTERY or USB_DRAIN the percent glides to the curve (1 point
    per 15 s). While charging the cell reads higher than at rest (charge current × internal
    resistance + polarisation): the model takes the plug-in step as that offset (60 mV when it has no reference,
    e.g. booted on USB) and removes all of it before the curve. Every decision and one record a minute go into a
    12 h log in PSRAM (`bat log`, `bat log all`). To check on a port: `bat` on battery after 3 min idle (`mv`),
    plug in, `bat` after 2 min (`src`, `ofs` = the measured step, `why`), charge 10 min, unplug, `bat` after 3 min:
    the percent should not have jumped; then `bat log 60`. Known limits without a VBUS pin: a wall charger plugged
    into a cell within ~1-3 % of full may step less than 35 mV (logged as `step_small`; taken as power through the
    trend only when the readings are calm, not under 15 mV); a charger that has terminated makes no step when
    unplugged either (no current flowed): the model notices only when the cell has drained 60 mV (35 mV while
    charging) under the level it held. Not measured yet: the ETA6098's LED states and its recharge threshold (the
    model assumes ≤ 150 mV under the CV level), whether it terminates at all with the board's idle load on the cell
    (it senses its whole output current) or feeds the load after termination, and what the divider reads with USB and
    no cell (the model hides the indicator if the readings swing > 250 mV).
