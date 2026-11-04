#pragma once

#include <cstdint>
#include <functional>

#include "core/Clock.h"
#include "core/MotionMonitor.h"
#include "core/Operations.h"
#include "core/PumpManager.h"
#include "core/SystemState.h"
#include "core/TransactionEngine.h"

namespace drinkbot {

// Single-pump prime/purge (SPEC_v3 §9.6.1) and the sequential
// Prime All / Purge All (§9.6.2). Like the dispenser, it is a
// non-blocking state machine driven by tick().
class BulkSequencer {
 public:
  using ProgressHandler = std::function<void(const BulkProgress&)>;

  static constexpr uint32_t kGapMs = 500;          // BULK_GAP_MS
  static constexpr float kPurgeFactor = 1.5f;
  static constexpr float kMinManualSec = 1;
  static constexpr float kMaxManualSec = 60;

  BulkSequencer(TransactionEngine& engine, PumpManager& pumps, SystemState& system, const IClock& clock);

  // The pumps, durations and skips Prime All / Purge All would use.
  BulkPlan plan(BulkOp op) const;

  OpResult startAll(BulkOp op);
  OpResult startSingle(BulkOp op, int pumpId, float seconds);

  void tick();
  void abort(const std::string& reason, bool sendStop);
  bool active() const { return phase_ != Phase::Idle; }

  void setProgressHandler(ProgressHandler handler) { onProgress_ = std::move(handler); }

 private:
  enum class Phase : uint8_t { Idle, Command, Running, Gap };

  OpResult begin(BulkPlan plan);
  void nextStep();
  void stepFinished();
  void complete();
  void fail(ErrorCode code, const std::string& message);
  void emit(BulkProgress::Status status, const std::string& reason = "");
  const BulkStep& step() const { return plan_.steps[stepIndex_]; }

  TransactionEngine& engine_;
  PumpManager& pumps_;
  SystemState& system_;
  const IClock& clock_;
  MotionMonitor monitor_;
  ProgressHandler onProgress_;

  Phase phase_ = Phase::Idle;
  uint32_t generation_ = 0;
  BulkPlan plan_;
  size_t stepIndex_ = 0;
  uint32_t gapUntil_ = 0;
};

}  // namespace drinkbot
