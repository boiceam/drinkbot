#pragma once

#include <cstdint>

namespace drinkbot {

// Monotonic millisecond clock. Injected everywhere so logic can run
// against a fake clock in native unit tests.
class IClock {
 public:
  virtual ~IClock() = default;
  virtual uint32_t nowMs() const = 0;
};

// Wrap-safe "has `deadline` been reached" check for millis()-style counters.
inline bool timeReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

}  // namespace drinkbot
