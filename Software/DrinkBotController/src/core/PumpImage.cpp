#include "core/PumpImage.h"

#include <cstdlib>
#include <cstring>

#include "core/PumpProtocol.h"

namespace drinkbot {

namespace {
const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool isTagValueChar(uint8_t c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '-';
}
}  // namespace

void PumpImageScanner::Tag::feed(uint8_t byte) {
  if (!value.empty() && !collecting) {
    return;  // the first complete tag wins
  }
  if (collecting) {
    if (isTagValueChar(byte) && value.size() < 32) {
      value.push_back(static_cast<char>(byte));
      return;
    }
    collecting = false;
    matched = 0;
    // A bare prefix (the firmware's own search string) isn't a tag: keep looking
    if (!value.empty()) {
      return;
    }
  }
  if (byte == static_cast<uint8_t>(prefix[matched])) {
    matched++;
    if (prefix[matched] == '\0') {
      collecting = true;
      matched = 0;
    }
  } else {
    matched = byte == static_cast<uint8_t>(prefix[0]) ? 1 : 0;
  }
}

void PumpImageScanner::add(const uint8_t* data, size_t len) {
  md5_.add(data, len);
  for (size_t i = 0; i < len; i++) {
    if (size_ + i < sizeof(first_)) {
      first_[size_ + i] = data[i];
    }
    version_.feed(data[i]);
    bus_.feed(data[i]);
  }
  size_ += static_cast<uint32_t>(len);
}

bool PumpImageScanner::finish(PumpImageInfo& info, std::string& why) {
  info = PumpImageInfo();
  if (size_ == 0) {
    why = "The file is empty";
    return false;
  }
  if (size_ > kMaxPumpImageBytes) {
    why = "The file is larger than a pump can store";
    return false;
  }
  if (first_[0] == 0x1f && first_[1] == 0x8b) {
    why = "Compressed images aren't accepted; use the plain .bin";
    return false;
  }
  if (memcmp(first_, "UF2\n", 4) == 0) {
    why = "That's a UF2 file; use the .bin from the same build";
    return false;
  }
  if (version_.value.empty()) {
    why = "Not DrinkBotPump firmware (no DBP-IMAGE tag)";
    return false;
  }
  if (compareVersions(version_.value, kMinPumpFirmware) < 0) {
    why = "Firmware " + version_.value + " is older than the required " + kMinPumpFirmware;
    return false;
  }
  if (bus_.value.empty()) {
    why = "Firmware " + version_.value + " doesn't declare its bus speed (DBP-BUS tag); it needs 0.7.1 or newer";
    return false;
  }
  if (strtoul(bus_.value.c_str(), nullptr, 10) != kPumpBusBaud) {
    why = "Firmware " + version_.value + " uses bus speed " + bus_.value + ", not " + std::to_string(kPumpBusBaud) +
          ". Pumps would lose contact during the update, so flash it over USB";
    return false;
  }
  info.present = true;
  info.version = version_.value;
  info.size = size_;
  info.md5 = md5_.finish();
  return true;
}

std::string base64Encode(const uint8_t* data, size_t len) {
  std::string out;
  out.reserve((len + 2) / 3 * 4);
  for (size_t i = 0; i < len; i += 3) {
    uint32_t v = static_cast<uint32_t>(data[i]) << 16;
    if (i + 1 < len) {
      v |= static_cast<uint32_t>(data[i + 1]) << 8;
    }
    if (i + 2 < len) {
      v |= data[i + 2];
    }
    out.push_back(kBase64[(v >> 18) & 63]);
    out.push_back(kBase64[(v >> 12) & 63]);
    out.push_back(i + 1 < len ? kBase64[(v >> 6) & 63] : '=');
    out.push_back(i + 2 < len ? kBase64[v & 63] : '=');
  }
  return out;
}

int base64Decode(const std::string& text, uint8_t* out, size_t maxOut) {
  size_t n = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : text) {
    if (c == '=') {
      break;
    }
    const char* p = c ? strchr(kBase64, c) : nullptr;
    if (!p) {
      return -1;
    }
    acc = (acc << 6) | static_cast<uint32_t>(p - kBase64);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n >= maxOut) {
        return -1;
      }
      out[n++] = static_cast<uint8_t>((acc >> bits) & 0xFF);
    }
  }
  return static_cast<int>(n);
}

}  // namespace drinkbot
