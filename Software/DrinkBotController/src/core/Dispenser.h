#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Clock.h"
#include "core/MotionMonitor.h"
#include "core/Operations.h"
#include "core/PumpManager.h"
#include "core/RecipeMath.h"
#include "core/SystemState.h"
#include "core/TransactionEngine.h"

namespace drinkbot {

// Pours a recipe one ingredient at a time (SPEC_v3 §9.5). Non-blocking:
// every step is driven from tick() and pump callbacks, so abort() can
// stop it at any point.
class Dispenser {
 public:
  using ProgressHandler = std::function<void(const DispenseProgress&)>;
  using StartedHandler = std::function<void(const OpResult&)>;

  Dispenser(TransactionEngine& engine, PumpManager& pumps, SystemState& system, const IClock& clock);

  // Runs prechecks 2-4 synchronously; on success reads every involved
  // pump's level (precheck 5) and then pours. `started` reports the final
  // precheck outcome: success once pouring begins, or PRECHECK_FAILED.
  OpResult start(const std::string& recipeId, float volumeMl, StartedHandler started);

  void tick();

  // Stops the sequence. `sendStop` stops the active pump (E-Stop sends its
  // own stops to every pump instead).
  void abort(const std::string& reason, bool sendStop);

  bool active() const { return phase_ != Phase::Idle; }

  void setProgressHandler(ProgressHandler handler) { onProgress_ = std::move(handler); }

 private:
  enum class Phase : uint8_t { Idle, Precheck, Step, Running, Settle, Verify };

  void finishPrecheck();
  void beginStep();
  void stepCommandsDone();
  void verify();
  void complete();
  void fail(ErrorCode code, const std::string& message);
  void emit(DispenseProgress::Status status, const std::string& reason = "");
  const PourStep& step() const { return steps_[stepIndex_]; }
  // Pump run time for a pour, from the pump's calibrated ms/ml
  uint32_t runMs(const PourStep& s) const;

  TransactionEngine& engine_;
  PumpManager& pumps_;
  SystemState& system_;
  const IClock& clock_;
  MotionMonitor monitor_;
  ProgressHandler onProgress_;

  Phase phase_ = Phase::Idle;
  uint32_t generation_ = 0;
  std::string recipeId_;
  float volumeMl_ = 0;
  std::vector<PourStep> steps_;
  size_t stepIndex_ = 0;
  StartedHandler started_;
  int precheckPending_ = 0;
  bool precheckFailed_ = false;
  std::string precheckMessage_;
  float weightBeforeG_ = 0;
  uint32_t settleUntil_ = 0;
  bool hasActual_ = false;
  float actualMl_ = 0;
};

}  // namespace drinkbot
