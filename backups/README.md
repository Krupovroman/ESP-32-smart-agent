# Backups

`factory_full_16MB.bin` is a full dump of the board's flash as it came from the factory
(Waveshare ESP32-S3-Touch-LCD-1.85 rev 2.1, MAC 7c:0c:5f:56:75:68).

Restore the factory firmware:

    python -m esptool --port COM7 write-flash 0 backups/factory_full_16MB.bin
