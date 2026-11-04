#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace drinkbot {

// Splits a byte stream into '\n'-terminated lines (SPEC_v3 §5.2).
// '\r' is dropped. Lines longer than kMaxLine are discarded whole and
// reported through onOverflow.
class LineFramer {
 public:
  static constexpr size_t kMaxLine = 256;

  using LineHandler = std::function<void(const std::string& line)>;
  using OverflowHandler = std::function<void()>;

  explicit LineFramer(LineHandler onLine, OverflowHandler onOverflow = nullptr)
      : onLine_(std::move(onLine)), onOverflow_(std::move(onOverflow)) {}

  void feed(const uint8_t* data, size_t len);
  void reset();

 private:
  LineHandler onLine_;
  OverflowHandler onOverflow_;
  std::string buf_;
  bool discarding_ = false;
};

}  // namespace drinkbot
