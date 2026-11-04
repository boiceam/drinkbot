#pragma once

#include <cstdint>

#include "core/Clock.h"
#include "core/TransactionEngine.h"

namespace drinkbot {

// Waits for a timed pump run to finish: the expected run time + 300 ms,
// then `status` every 250 ms until IDLE, for at most 5 s more
// (SPEC_v3 §9.5, §9.6). Used by the dispenser and the prime/purge sequencer.
class MotionMonitor {
 public:
  enum class Result : uint8_t { Running, Idle, Overrun, CommFailure };

  static constexpr uint32_t kStartMarginMs = 300;
  static constexpr uint32_t kStatusPollMs = 250;
  static constexpr uint32_t kOverrunMs = 5000;

  MotionMonitor(TransactionEngine& engine, const IClock& clock) : engine_(engine), clock_(clock) {}

  void start(int pumpId, uint32_t expectedMs);
  // Call every tick; returns Running until a final result.
  Result poll();
  // Ignores any status reply still in flight.
  void reset();

 private:
  TransactionEngine& engine_;
  const IClock& clock_;
  int pumpId_ = 0;
  uint32_t firstCheckAt_ = 0;
  uint32_t deadline_ = 0;
  uint32_t nextStatusAt_ = 0;
  uint32_t generation_ = 0;
  bool inFlight_ = false;
  bool idle_ = false;
  bool commFailed_ = false;
};

}  // namespace drinkbot
