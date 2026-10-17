#pragma once
#include <stdint.h>
// Same contract as the ROM function: equal to zlib.crc32(buf, crc).
uint32_t esp_rom_crc32_le(uint32_t crc, uint8_t const *buf, uint32_t len);
