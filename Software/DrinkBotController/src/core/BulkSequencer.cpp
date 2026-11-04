#include "core/BulkSequencer.h"

#include <algorithm>
#include <cmath>

#include "core/Log.h"

namespace drinkbot {

BulkSequencer::BulkSequencer(TransactionEngine& engine, PumpManager& pumps, SystemState& system,
                             const IClock& clock)
    : engine_(engine), pumps_(pumps), system_(system), clock_(clock), monitor_(engine, clock) {}

BulkPlan BulkSequencer::plan(BulkOp op) const {
  BulkPlan plan;
  plan.op = op;
  std::vector<const PumpConfig*> pumps;
  for (const auto& pc : pumps_.config().pumps) {
    if (pc.autoPrimeSec > 0) {
      pumps.push_back(&pc);
    }
  }
  std::sort(pumps.begin(), pumps.end(), [](const PumpConfig* a, const PumpConfig* b) { return a->pumpId < b->pumpId; });

  float factor = bulkOpIsReverse(op) ? kPurgeFactor : 1.0f;
  uint32_t total = 0;
  for (const PumpConfig* pc : pumps) {
    BulkStep s;
    s.pumpId = pc->pumpId;
    s.liquid = pc->liquid;
    s.durationMs = static_cast<uint32_t>(std::lround(factor * pc->autoPrimeSec * 1000.0f));
    const PumpState* ps = pumps_.find(pc->pumpId);
    if (!ps || !ps->present) {
      s.skip = true;
      s.skipReason = "not responding";
    } else if (!ps->fwOk) {
      s.skip = true;
      s.skipReason = "firmware too old";
    }
    total += s.durationMs + MotionMonitor::kStartMarginMs;
    plan.steps.push_back(s);
  }
  if (!plan.steps.empty()) {
    total += static_cast<uint32_t>(plan.steps.size() - 1) * kGapMs;
  }
  plan.estimatedTotalMs = total;
  return plan;
}

OpResult BulkSequencer::startAll(BulkOp op) {
  BulkPlan p = plan(op);
  if (p.steps.empty()) {
    return OpResult::fail(ErrCode::kPrecheckFailed, "No pump has an auto prime time set");
  }
  return begin(std::move(p));
}

OpResult BulkSequencer::startSingle(BulkOp op, int pumpId) {
  if (active() && !singleActive()) {
    return OpResult::fail(ErrCode::kBusy, std::string(bulkOpName(plan_.op)) + " is running");
  }
  const PumpConfig* pc = pumps_.pumpConfig(pumpId);
  const PumpState* ps = pumps_.find(pumpId);
  if (!pc || !ps) {
    return OpResult::fail(ErrCode::kNotFound, "No pump with ID " + std::to_string(pumpId));
  }
  if (!ps->usable()) {
    return OpResult::fail(ErrCode::kPrecheckFailed, "Pump " + std::to_string(pumpId) + " is not available");
  }
  BulkPlan p;
  p.op = op;
  BulkStep s;
  s.pumpId = pumpId;
  s.liquid = pc->liquid;
  s.durationMs = kManualRunMs;
  p.steps.push_back(s);
  p.estimatedTotalMs = s.durationMs + MotionMonitor::kStartMarginMs;
  if (singleActive()) {
    // One pump at a time: stop the running one, then start this one
    LOG_I("PUMP", "Stopping pump %d before %s pump %d", step().pumpId,
          bulkOpIsReverse(op) ? "purging" : "priming", pumpId);
    pending_ = std::move(p);
    hasPending_ = true;
    if (phase_ != Phase::Stopping) {
      stopCurrent();
    }
    return OpResult::success();
  }
  return begin(std::move(p));
}

OpResult BulkSequencer::stopSingle() {
  if (!singleActive()) {
    return OpResult::fail(ErrCode::kInvalid, "No prime or purge is running");
  }
  hasPending_ = false;
  if (phase_ != Phase::Stopping) {
    stopCurrent();
  }
  return OpResult::success();
}

void BulkSequencer::stopCurrent() {
  generation_++;
  monitor_.reset();
  phase_ = Phase::Stopping;
  uint32_t gen = generation_;
  int pumpId = step().pumpId;
  engine_.submit(
      pumpId, "stop",
      [this, gen, pumpId](const TxnResult& r) {
        if (gen != generation_) {
          return;
        }
        if (!r.ok()) {
          hasPending_ = false;
          fail(ErrorCode::PumpComm, "Pump " + std::to_string(pumpId) + " did not acknowledge stop (" +
                                        txnStatusName(r.status) + ")");
          return;
        }
        LOG_I("PUMP", "Pump %d stopped", pumpId);
        pumps_.setActivity(pumpId, PumpActivity::Idle);
        pumps_.restoreIdleLed(pumpId);
        emit(BulkProgress::Status::Done, "stopped");
        complete();
        if (hasPending_) {
          hasPending_ = false;
          begin(std::move(pending_));
        }
      },
      true);
}

OpResult BulkSequencer::begin(BulkPlan plan) {
  generation_++;
  plan_ = std::move(plan);
  stepIndex_ = 0;
  system_.setActivity(Activity::Manual);
  if (bulkOpIsAll(plan_.op)) {
    LOG_I("PUMP", "%s: %u pumps, about %.1f s", bulkOpName(plan_.op), static_cast<unsigned>(plan_.steps.size()),
          plan_.estimatedTotalMs / 1000.0f);
  }
  emit(BulkProgress::Status::Starting);
  nextStep();
  return OpResult::success();
}

void BulkSequencer::nextStep() {
  while (stepIndex_ < plan_.steps.size() && step().skip) {
    LOG_W("PUMP", "%s: skipping pump %d (%s)", bulkOpName(plan_.op), step().pumpId, step().skipReason.c_str());
    emit(BulkProgress::Status::Skipped, step().skipReason);
    stepIndex_++;
  }
  if (stepIndex_ >= plan_.steps.size()) {
    complete();
    return;
  }

  const BulkStep& s = step();
  phase_ = Phase::Command;
  const PumpState* ps = pumps_.find(s.pumpId);
  if (ps && ps->levelState == LevelState::Empty) {
    LOG_W("PUMP", "Pump %d (%s) is EMPTY", s.pumpId, s.liquid.c_str());
  }
  bool reverse = bulkOpIsReverse(plan_.op);
  if (bulkOpIsAll(plan_.op)) {
    LOG_I("PUMP", "%s pump %d (%s) for %.1f s", reverse ? "Purging" : "Priming", s.pumpId, s.liquid.c_str(),
          s.durationMs / 1000.0f);
  } else {
    LOG_I("PUMP", "%s pump %d (%s) until stopped (at most %.0f s)", reverse ? "Purging" : "Priming", s.pumpId,
          s.liquid.c_str(), s.durationMs / 1000.0f);
  }
  emit(BulkProgress::Status::Running);

  pumps_.showManualLed(s.pumpId);
  uint32_t gen = generation_;
  int pumpId = s.pumpId;
  uint32_t durationMs = s.durationMs;
  std::string cmd = std::string(reverse ? "purge " : "prime ") + std::to_string(durationMs);
  engine_.submit(pumpId, cmd, [this, gen, pumpId, durationMs, reverse](const TxnResult& r) {
    if (gen != generation_) {
      return;
    }
    if (!r.ok()) {
      fail(ErrorCode::PumpComm, "Pump " + std::to_string(pumpId) + " did not start (" + txnStatusName(r.status) + ")");
      return;
    }
    pumps_.setActivity(pumpId, reverse ? PumpActivity::Purging : PumpActivity::Priming);
    monitor_.start(pumpId, durationMs);
    phase_ = Phase::Running;
  });
}

void BulkSequencer::tick() {
  if (phase_ == Phase::Running) {
    switch (monitor_.poll()) {
      case MotionMonitor::Result::Running:
        return;
      case MotionMonitor::Result::Idle:
        stepFinished();
        return;
      case MotionMonitor::Result::Overrun:
        engine_.submit(step().pumpId, "stop", nullptr, true);
        fail(ErrorCode::DispenseTimeout,
             "Pump " + std::to_string(step().pumpId) + " was still running 5 s after it should have stopped");
        return;
      case MotionMonitor::Result::CommFailure:
        fail(ErrorCode::PumpComm, "Lost contact with pump " + std::to_string(step().pumpId));
        return;
    }
  }
  if (phase_ == Phase::Gap && timeReached(clock_.nowMs(), gapUntil_)) {
    nextStep();
  }
}

void BulkSequencer::stepFinished() {
  int pumpId = step().pumpId;
  pumps_.setActivity(pumpId, PumpActivity::Idle);
  pumps_.restoreIdleLed(pumpId);
  emit(BulkProgress::Status::Done);
  stepIndex_++;
  bool more = false;
  for (size_t i = stepIndex_; i < plan_.steps.size(); i++) {
    more |= !plan_.steps[i].skip;
  }
  if (more) {
    phase_ = Phase::Gap;
    gapUntil_ = clock_.nowMs() + kGapMs;
  } else {
    nextStep();  // emits any trailing skips, then completes
  }
}

void BulkSequencer::complete() {
  if (bulkOpIsAll(plan_.op)) {
    LOG_I("PUMP", "%s complete", bulkOpName(plan_.op));
  }
  phase_ = Phase::Idle;
  generation_++;
  emit(BulkProgress::Status::Complete);
  system_.setActivity(Activity::Idle);
}

void BulkSequencer::fail(ErrorCode code, const std::string& message) {
  system_.raiseError(code, step().pumpId, message);
  abort(message, true);
}

void BulkSequencer::abort(const std::string& reason, bool sendStop) {
  if (!active()) {
    return;
  }
  Phase phase = phase_;
  generation_++;
  monitor_.reset();
  phase_ = Phase::Idle;
  hasPending_ = false;
  if (phase == Phase::Command || phase == Phase::Running || phase == Phase::Stopping) {
    int pumpId = step().pumpId;
    if (sendStop) {
      engine_.submit(pumpId, "stop", nullptr, true);
    }
    pumps_.setActivity(pumpId, PumpActivity::Idle);
    pumps_.restoreIdleLed(pumpId);
  }
  LOG_W("PUMP", "%s aborted: %s", bulkOpName(plan_.op), reason.c_str());
  emit(BulkProgress::Status::Aborted, reason);
  system_.setActivity(Activity::Idle);
}

void BulkSequencer::emit(BulkProgress::Status status, const std::string& reason) {
  if (!onProgress_) {
    return;
  }
  BulkProgress p;
  p.op = plan_.op;
  p.steps = static_cast<int>(plan_.steps.size());
  p.estimatedTotalMs = plan_.estimatedTotalMs;
  p.status = status;
  p.reason = reason;
  bool perStep = status == BulkProgress::Status::Running || status == BulkProgress::Status::Done ||
                 status == BulkProgress::Status::Skipped || status == BulkProgress::Status::Aborted;
  if (perStep && stepIndex_ < plan_.steps.size()) {
    const BulkStep& s = step();
    p.step = static_cast<int>(stepIndex_ + 1);
    p.pumpId = s.pumpId;
    p.liquid = s.liquid;
    p.durationMs = s.durationMs;
  }
  onProgress_(p);
}

}  // namespace drinkbot
