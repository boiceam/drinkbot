#include "core/Crc32.h"

namespace drinkbot {

uint32_t crc32(const char* data, size_t length) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < length; i++) {
    uint8_t ch = static_cast<uint8_t>(data[i]);
    for (int j = 0; j < 8; j++) {
      uint32_t b = (ch ^ crc) & 1;
      crc >>= 1;
      if (b) {
        crc ^= 0xEDB88320;
      }
      ch >>= 1;
    }
  }
  return ~crc;
}

}  // namespace drinkbot
