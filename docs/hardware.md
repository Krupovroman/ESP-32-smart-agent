# Hardware reference — Waveshare ESP32-S3-Touch-LCD-1.85 (rev 2.1)

Compiled 2026-10-17 from the Waveshare docs (docs.waveshare.com/ESP32-S3-Touch-LCD-1.85),
the archived official schematic (2024-09-18), Espressif ESP32_Display_Panel v1.0.4 board file,
xiaozhi-esp32 board config and Waveshare demo code; each fact cross-checked by a second agent.

> Not to be confused with the **1.85C** (touch on GPIO10/11, ES8311 codec on V2) or the **1.85B**.
> Code for those boards is not drop-in for this one.

## Chip readout (esptool, this unit)

ESP32-S3R8 (QFN56) rev v0.2, 8 MB in-package **octal** PSRAM (3.3 V), 16 MB external quad flash
(JEDEC 0x20/0x4018 — second-source part, schematic says W25Q128JVSI), 40 MHz crystal,
native USB-Serial/JTAG on **COM7**, MAC 7c:0c:5f:56:75:68.

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
10. **No charge status to the MCU.** The ETA6098 drives only its charge LED. The firmware infers charging from the
    USB host (`usb_serial_jtag_is_connected()`, start-of-frame packets; a plain wall charger sends none) or from the
    battery voltage rising at rest (+15 mV in 2 min below 4.15 V, readings within 60 s of a load step ignored),
    docs/ui.md 3.16. With USB connected the ×3 divider read about 4065 mV (≈ 83 %) on this unit. What the divider
    reads with USB and no cell is not measured yet (the model hides the indicator if the readings swing > 250 mV).
