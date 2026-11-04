#include "core/MotionMonitor.h"

namespace drinkbot {

void MotionMonitor::start(int pumpId, uint32_t expectedMs) {
  generation_++;
  pumpId_ = pumpId;
  uint32_t now = clock_.nowMs();
  firstCheckAt_ = now + expectedMs + kStartMarginMs;
  deadline_ = firstCheckAt_ + kOverrunMs;
  nextStatusAt_ = firstCheckAt_;
  inFlight_ = false;
  idle_ = false;
  commFailed_ = false;
}

void MotionMonitor::reset() {
  generation_++;
  inFlight_ = false;
}

MotionMonitor::Result MotionMonitor::poll() {
  if (idle_) {
    return Result::Idle;
  }
  if (commFailed_) {
    return Result::CommFailure;
  }
  uint32_t now = clock_.nowMs();
  if (!timeReached(now, firstCheckAt_) || inFlight_) {
    return Result::Running;
  }
  if (timeReached(now, deadline_)) {
    return Result::Overrun;
  }
  if (!timeReached(now, nextStatusAt_)) {
    return Result::Running;
  }

  inFlight_ = true;
  uint32_t gen = generation_;
  engine_.submit(pumpId_, "status", [this, gen](const TxnResult& r) {
    if (gen != generation_) {
      return;
    }
    inFlight_ = false;
    PumpMotion motion;
    uint32_t remaining = 0;
    if (!r.ok() || !parseStatus(r, motion, remaining)) {
      commFailed_ = true;
      return;
    }
    if (motion == PumpMotion::Idle) {
      idle_ = true;
    } else {
      nextStatusAt_ = clock_.nowMs() + kStatusPollMs;
    }
  });
  return Result::Running;
}

}  // namespace drinkbot
