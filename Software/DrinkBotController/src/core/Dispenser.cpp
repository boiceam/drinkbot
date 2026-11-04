#include "core/Dispenser.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/Log.h"

namespace drinkbot {

namespace {

std::string mlText(float ml) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%.1f", ml);
  return buf;
}

}  // namespace

Dispenser::Dispenser(TransactionEngine& engine, PumpManager& pumps, SystemState& system, const IClock& clock)
    : engine_(engine), pumps_(pumps), system_(system), clock_(clock), monitor_(engine, clock) {}

OpResult Dispenser::start(const std::string& recipeId, float volumeMl, StartedHandler started) {
  const Config& cfg = pumps_.config();
  const Recipe* recipe = cfg.findRecipe(recipeId);
  if (!recipe) {
    return OpResult::fail(ErrCode::kNotFound, "No recipe with id '" + recipeId + "'");
  }
  if (!(volumeMl >= cfg.dispense.minVolumeMl && volumeMl <= cfg.dispense.maxVolumeMl)) {
    return OpResult::fail(ErrCode::kInvalid, "Volume must be between " + mlText(cfg.dispense.minVolumeMl) +
                                                 " and " + mlText(cfg.dispense.maxVolumeMl) + " ml");
  }

  std::vector<PourStep> steps = computePours(cfg, *recipe, volumeMl);
  for (const auto& s : steps) {
    if (s.pumpId == 0) {
      return OpResult::fail(ErrCode::kPrecheckFailed, s.liquid + " is not loaded on any pump");
    }
    const PumpState* p = pumps_.find(s.pumpId);
    if (!p || !p->present) {
      return OpResult::fail(ErrCode::kPrecheckFailed,
                            s.liquid + " pump " + std::to_string(s.pumpId) + " is not responding");
    }
    if (!p->fwOk) {
      return OpResult::fail(ErrCode::kPrecheckFailed,
                            s.liquid + " pump " + std::to_string(s.pumpId) + " firmware is too old");
    }
    if (system_.pumpBlocked(s.pumpId)) {
      return OpResult::fail(ErrCode::kPrecheckFailed,
                            s.liquid + " pump " + std::to_string(s.pumpId) + " has an active error");
    }
  }

  generation_++;
  recipeId_ = recipeId;
  volumeMl_ = volumeMl;
  steps_ = std::move(steps);
  stepIndex_ = 0;
  started_ = std::move(started);
  precheckFailed_ = false;
  precheckMessage_.clear();
  phase_ = Phase::Precheck;
  system_.setActivity(Activity::Dispensing);
  LOG_I("DISP", "Dispense %s, %.0f ml: checking levels", recipe->name.c_str(), volumeMl);

  // Precheck 5: fresh level for each pump, against the summed amount
  std::vector<LiquidTotal> totals = totalsByPump(steps_);
  precheckPending_ = static_cast<int>(totals.size());
  uint32_t gen = generation_;
  for (const auto& t : totals) {
    pumps_.refreshWeight(t.pumpId, [this, gen, t](bool ok, float levelMl) {
      if (gen != generation_) {
        return;
      }
      if (!ok) {
        precheckFailed_ = true;
        precheckMessage_ = "Could not read the level of " + t.liquid;
      } else {
        float reserve = pumps_.config().levels.actualEmptyMl;
        float available = levelMl - reserve;
        if (levelMl - t.ml < reserve && !precheckFailed_) {
          precheckFailed_ = true;
          precheckMessage_ = "Not enough " + t.liquid + ": needs " + mlText(t.ml) + " ml, " +
                             mlText(std::max(0.0f, available)) + " ml available above the reserve";
        }
      }
      if (--precheckPending_ == 0) {
        finishPrecheck();
      }
    });
  }
  return OpResult::success();
}

void Dispenser::finishPrecheck() {
  StartedHandler started = std::move(started_);
  started_ = nullptr;
  if (precheckFailed_) {
    LOG_W("DISP", "Dispense not started: %s", precheckMessage_.c_str());
    phase_ = Phase::Idle;
    system_.setActivity(Activity::Idle);
    if (started) {
      started(OpResult::fail(ErrCode::kPrecheckFailed, precheckMessage_));
    }
    return;
  }
  if (started) {
    started(OpResult::success());
  }
  beginStep();
}

void Dispenser::beginStep() {
  if (stepIndex_ >= steps_.size()) {
    complete();
    return;
  }
  phase_ = Phase::Step;
  hasActual_ = false;
  const PourStep& s = step();
  const PumpConfig* pc = pumps_.pumpConfig(s.pumpId);
  LOG_I("DISP", "Step %u/%u: %s %s ml on pump %d", static_cast<unsigned>(stepIndex_ + 1),
        static_cast<unsigned>(steps_.size()), s.liquid.c_str(), mlText(s.ml).c_str(), s.pumpId);
  emit(DispenseProgress::Status::Starting);

  uint32_t gen = generation_;
  int pumpId = s.pumpId;
  int msPerMl = pc ? pc->msPerMl : 100;
  pumps_.refreshWeight(pumpId, [this, gen, pumpId, msPerMl](bool ok, float) {
    if (gen != generation_) {
      return;
    }
    if (!ok) {
      fail(ErrorCode::PumpComm, "Pump " + std::to_string(pumpId) + " did not report its weight");
      return;
    }
    weightBeforeG_ = pumps_.find(pumpId)->weightG;
    engine_.submit(pumpId, "smilli " + std::to_string(msPerMl), [this, gen, pumpId](const TxnResult& r) {
      if (gen != generation_) {
        return;
      }
      if (!r.ok()) {
        fail(ErrorCode::PumpComm, "Pump " + std::to_string(pumpId) + " rejected its flow rate");
        return;
      }
      pumps_.prepareDispenseLed(pumpId);
      engine_.submit(pumpId, "dispense " + mlText(step().ml) + " 3", [this, gen, pumpId](const TxnResult& d) {
        if (gen != generation_) {
          return;
        }
        if (!d.ok()) {
          fail(ErrorCode::PumpComm, "Pump " + std::to_string(pumpId) + " did not start the pour (" +
                                        txnStatusName(d.status) + ")");
          return;
        }
        stepCommandsDone();
      });
    });
  });
}

void Dispenser::stepCommandsDone() {
  const PourStep& s = step();
  const PumpConfig* pc = pumps_.pumpConfig(s.pumpId);
  uint32_t expectedMs = static_cast<uint32_t>(s.ml * (pc ? pc->msPerMl : 100) + 0.5f);
  pumps_.setActivity(s.pumpId, PumpActivity::Dispensing);
  monitor_.start(s.pumpId, expectedMs);
  phase_ = Phase::Running;
}

void Dispenser::tick() {
  if (phase_ == Phase::Running) {
    switch (monitor_.poll()) {
      case MotionMonitor::Result::Running:
        return;
      case MotionMonitor::Result::Idle:
        pumps_.setActivity(step().pumpId, PumpActivity::Idle);
        settleUntil_ = clock_.nowMs() + pumps_.config().dispense.settleMs;
        phase_ = Phase::Settle;
        return;
      case MotionMonitor::Result::Overrun:
        engine_.submit(step().pumpId, "stop", nullptr, true);
        fail(ErrorCode::DispenseTimeout,
             "Pump " + std::to_string(step().pumpId) + " was still running 5 s after its pour should have ended");
        return;
      case MotionMonitor::Result::CommFailure:
        fail(ErrorCode::PumpComm, "Lost contact with pump " + std::to_string(step().pumpId) + " during a pour");
        return;
    }
  }
  if (phase_ == Phase::Settle && timeReached(clock_.nowMs(), settleUntil_)) {
    verify();
  }
}

void Dispenser::verify() {
  phase_ = Phase::Verify;
  uint32_t gen = generation_;
  int pumpId = step().pumpId;
  pumps_.refreshWeight(pumpId, [this, gen, pumpId](bool ok, float) {
    if (gen != generation_) {
      return;
    }
    if (!ok) {
      fail(ErrorCode::PumpComm, "Pump " + std::to_string(pumpId) + " did not report its weight after pouring");
      return;
    }
    const PourStep& s = step();
    const PumpConfig* pc = pumps_.pumpConfig(pumpId);
    float density = pc ? pc->densityGPerMl : 1.0f;
    actualMl_ = (weightBeforeG_ - pumps_.find(pumpId)->weightG) / density;
    hasActual_ = true;
    const DispenseSettings& d = pumps_.config().dispense;
    float tolerance = std::max(s.ml * d.verifyTolerancePct / 100.0f, d.verifyToleranceMinMl);
    if (std::fabs(actualMl_ - s.ml) > tolerance) {
      LOG_W("DISP", "%s: poured %.1f ml, expected %.1f ml (off by more than %.1f ml)", s.liquid.c_str(), actualMl_,
            s.ml, tolerance);
    } else {
      LOG_I("DISP", "%s: poured %.1f ml of %.1f ml", s.liquid.c_str(), actualMl_, s.ml);
    }
    pumps_.restoreIdleLed(pumpId);
    emit(DispenseProgress::Status::Done);
    stepIndex_++;
    beginStep();
  });
}

void Dispenser::complete() {
  LOG_I("DISP", "Dispense of %s complete (%u steps)", recipeId_.c_str(), static_cast<unsigned>(steps_.size()));
  phase_ = Phase::Idle;
  generation_++;
  emit(DispenseProgress::Status::Complete);
  system_.setActivity(Activity::Idle);
}

void Dispenser::fail(ErrorCode code, const std::string& message) {
  system_.raiseError(code, phase_ == Phase::Precheck ? 0 : step().pumpId, message);
  abort(message, true);
}

void Dispenser::abort(const std::string& reason, bool sendStop) {
  if (!active()) {
    return;
  }
  Phase phase = phase_;
  generation_++;
  monitor_.reset();
  phase_ = Phase::Idle;

  if (phase == Phase::Precheck) {
    StartedHandler started = std::move(started_);
    started_ = nullptr;
    system_.setActivity(Activity::Idle);
    if (started) {
      started(OpResult::fail(ErrCode::kPrecheckFailed, reason));
    }
    return;
  }

  int pumpId = step().pumpId;
  if (sendStop) {
    engine_.submit(pumpId, "stop", nullptr, true);
  }
  pumps_.setActivity(pumpId, PumpActivity::Idle);
  pumps_.restoreIdleLed(pumpId);
  LOG_W("DISP", "Dispense of %s aborted after %u of %u steps: %s", recipeId_.c_str(),
        static_cast<unsigned>(stepIndex_), static_cast<unsigned>(steps_.size()), reason.c_str());
  emit(DispenseProgress::Status::Aborted, reason);
  system_.setActivity(Activity::Idle);
}

void Dispenser::emit(DispenseProgress::Status status, const std::string& reason) {
  if (!onProgress_) {
    return;
  }
  DispenseProgress p;
  p.recipeId = recipeId_;
  p.volumeMl = volumeMl_;
  p.steps = static_cast<int>(steps_.size());
  p.status = status;
  p.reason = reason;
  bool perStep = status == DispenseProgress::Status::Starting || status == DispenseProgress::Status::Done;
  if (perStep) {
    const PourStep& s = step();
    p.step = static_cast<int>(stepIndex_ + 1);
    p.liquid = s.liquid;
    p.pumpId = s.pumpId;
    p.targetMl = s.ml;
    p.hasActual = hasActual_;
    p.actualMl = actualMl_;
  } else if (status == DispenseProgress::Status::Aborted) {
    p.step = static_cast<int>(std::min(stepIndex_ + 1, steps_.size()));
  } else {
    p.step = static_cast<int>(steps_.size());
  }
  onProgress_(p);
}

}  // namespace drinkbot
