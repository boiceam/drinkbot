#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace drinkbot {

// Incremental MD5, here only to match the check pump firmware does on an
// image (not for security). Host-testable, unlike the ESP-IDF one.
class Md5 {
 public:
  Md5() { reset(); }
  void reset();
  void add(const uint8_t* data, size_t len);
  // Lowercase hex digest. Call once; reset() before reusing.
  std::string finish();

 private:
  void block(const uint8_t* p);

  uint32_t state_[4];
  uint64_t bytes_ = 0;
  uint8_t buffer_[64];
  size_t used_ = 0;
};

}  // namespace drinkbot
