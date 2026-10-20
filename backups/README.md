# Backups

`factory_full_16MB.bin` is a full dump of the board's flash as it came from the factory
(Waveshare ESP32-S3-Touch-LCD-1.85 rev 2.1), taken 2026-10-17.

- SHA-256: `90151CB92E6D224D02B19A6956D8EC9AD5F7ADE0008EE24E97012210F8802AE9`
- Factory app: `ESP32-S3-Touch-LCD-1.85-Test` v1, ESP-IDF v5.3.1, built Feb 27 2025

Factory partition table:

| name       | type | subtype | offset   | size   |
|------------|------|---------|----------|--------|
| nvs        | data | nvs     | 0x009000 | 24 KB  |
| factory    | app  | factory | 0x010000 | 3 MB   |
| flash_test | data | 0x81    | 0x310000 | 528 KB |
| model      | data | 0x82    | 0x394000 | 5.8 MB |

Verified against the chip with `verify-flash`: everything matches except the NVS
region (0x9000-0xAFFF), which the factory firmware rewrites on every boot.

Restore the factory firmware:

    python -m esptool --port COM7 write-flash 0 backups/factory_full_16MB.bin
