#pragma once

#include <cstddef>
#include <cstdint>

namespace eeprom {

/// CRC function signature used by Store. A hardware CRC unit can be plugged in as long as it
/// computes the same CRC-32 (IEEE 802.3, as in zlib/PNG: reflected, poly 0x04C11DB7,
/// init 0xFFFFFFFF, final XOR 0xFFFFFFFF) over one contiguous buffer.
using Crc32Fn = uint32_t (*)(const uint8_t* data, size_t len);

/// Default software CRC-32 (16-entry table, 64 bytes of flash). crc32("123456789") == 0xCBF43926.
uint32_t soft_crc32(const uint8_t* data, size_t len);

} // namespace eeprom
