#pragma once

#include <cstddef>
#include <cstdint>

namespace drinkbot {

// CRC-32/ISO-HDLC, bit-for-bit the same as calculateCRC32() in
// Software/DrinkBotPump/DrinkBotPump.ino (reflected, poly 0xEDB88320,
// init and xorout 0xFFFFFFFF).
uint32_t crc32(const char* data, size_t length);

}  // namespace drinkbot
