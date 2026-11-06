#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "core/Md5.h"

namespace drinkbot {

// The pump firmware image the controller keeps for updating pumps (SPEC_v3 §9.10).

// RS485 speed all pumps must keep: an image with another speed would strand
// pumps from the bus partway through an update, so it is refused (PF-15).
constexpr uint32_t kPumpBusBaud = 50000;
// Pumps stage an image in a 512 KB file system (PF-13).
constexpr size_t kMaxPumpImageBytes = 400 * 1024;
// Bytes per fwdata chunk: a multiple of 3 (no base64 padding) that fits the
// 200-character bridged command limit.
constexpr size_t kPumpImageChunk = 120;

struct PumpImageInfo {
  bool present = false;
  std::string version;
  uint32_t size = 0;
  std::string md5;
};

// Checks a pump image as it streams in, without holding it in memory.
class PumpImageScanner {
 public:
  void add(const uint8_t* data, size_t len);
  // False with `why` set if this isn't an acceptable pump image.
  bool finish(PumpImageInfo& info, std::string& why);

 private:
  struct Tag {
    const char* prefix;
    size_t matched = 0;
    bool collecting = false;
    std::string value;
    explicit Tag(const char* p) : prefix(p) {}
    void feed(uint8_t byte);
  };

  Md5 md5_;
  uint32_t size_ = 0;
  uint8_t first_[4] = {0, 0, 0, 0};
  Tag version_{"DBP-IMAGE:"};
  Tag bus_{"DBP-BUS:"};
};

// Where the image lives: LittleFS on hardware, memory in tests.
class IPumpImageStore {
 public:
  virtual ~IPumpImageStore() = default;
  virtual PumpImageInfo info() const = 0;
  // An upload is being written; the current image can't be used right now.
  virtual bool busy() const = 0;
  virtual bool read(uint32_t offset, uint8_t* out, size_t len) = 0;
  virtual bool remove() = 0;
};

// Standard base64 for fwdata chunks. Decode returns the byte count, or -1 on
// bad input or overflow.
std::string base64Encode(const uint8_t* data, size_t len);
int base64Decode(const std::string& text, uint8_t* out, size_t maxOut);

}  // namespace drinkbot
