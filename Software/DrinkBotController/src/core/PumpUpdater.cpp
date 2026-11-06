#include "core/PumpUpdater.h"

#include <algorithm>

#include "core/Log.h"

namespace drinkbot {

PumpUpdater::PumpUpdater(TransactionEngine& engine, PumpManager& pumps, SystemState& system, const IClock& clock)
    : engine_(engine), pumps_(pumps), system_(system), clock_(clock) {}

PumpUpdater::Plan PumpUpdater::plan() const {
  Plan plan;
  if (!store_) {
    return plan;
  }
  PumpImageInfo image = store_->info();
  if (!image.present) {
    return plan;
  }
  Target bridgeTarget;
  bool bridgeOutdated = false;
  for (const auto& p : pumps_.pumps()) {
    if (!p.present) {
      plan.missing.push_back(p.pumpId);
    } else if (compareVersions(p.fwVersion, image.version) >= 0) {
      continue;
    } else if (!p.fwOk) {
      plan.needsUsb.push_back(p.pumpId);
    } else if (p.isBridge) {
      bridgeTarget = {p.pumpId, p.fwVersion, true};
      bridgeOutdated = true;
    } else {
      plan.targets.push_back({p.pumpId, p.fwVersion, false});
    }
  }
  std::sort(plan.targets.begin(), plan.targets.end(),
            [](const Target& a, const Target& b) { return a.pumpId < b.pumpId; });
  // A dedicated bridge board isn't in the pump list
  const std::string& bridgeVersion = pumps_.bridgeVersion();
  if (pumps_.bridgeId() == kBridgeBoardId && pumps_.bridgeResponding() && !bridgeVersion.empty() &&
      compareVersions(bridgeVersion, image.version) < 0) {
    if (compareVersions(bridgeVersion, kMinPumpFirmware) < 0) {
      plan.needsUsb.push_back(kBridgeBoardId);
    } else {
      bridgeTarget = {kBridgeBoardId, bridgeVersion, true};
      bridgeOutdated = true;
    }
  }
  if (bridgeOutdated) {
    plan.targets.push_back(bridgeTarget);
  }
  return plan;
}

OpResult PumpUpdater::start() {
  if (active()) {
    return OpResult::fail(ErrCode::kBusy, "An update is already running");
  }
  if (!store_ || store_->busy()) {
    return OpResult::fail(ErrCode::kBusy, "A firmware file is still being uploaded");
  }
  image_ = store_->info();
  if (!image_.present) {
    return OpResult::fail(ErrCode::kInvalid, "Upload a pump firmware file first");
  }
  if (!pumps_.bridgeResponding()) {
    return OpResult::fail(ErrCode::kPrecheckFailed, "The bridge isn't responding");
  }
  targets_ = plan().targets;
  if (targets_.empty()) {
    return OpResult::fail(ErrCode::kInvalid, "Every reachable board already has firmware " + image_.version +
                                                 " or newer");
  }
  generation_++;
  index_ = 0;
  cancelRequested_ = false;
  status_ = Status();
  status_.total = static_cast<int>(targets_.size());
  status_.size = image_.size;
  system_.setActivity(Activity::Manual);
  LOG_W("PUMP", "Updating %d board(s) to firmware %s", status_.total, image_.version.c_str());
  startTarget();
  return OpResult::success();
}

std::string PumpUpdater::targetName() const {
  if (target().pumpId == kBridgeBoardId) {
    return "The bridge";
  }
  return "Pump " + std::to_string(target().pumpId);
}

void PumpUpdater::startTarget() {
  if (cancelRequested_) {
    finish("CANCELLED", "Stopped before " + targetName());
    return;
  }
  status_.phase = Phase::Transfer;
  status_.pumpId = target().pumpId;
  status_.index = static_cast<int>(index_) + 1;
  status_.bytes = 0;
  installing_ = false;
  polling_ = false;
  LOG_I("PUMP", "%s: firmware %s -> %s", targetName().c_str(), target().version.c_str(), image_.version.c_str());
  changed();
  sendBegin(1);
}

void PumpUpdater::sendBegin(int attempt) {
  uint32_t gen = generation_;
  std::string cmd = "fwbegin " + std::to_string(image_.size) + " " + image_.md5;
  engine_.submit(target().pumpId, cmd, [this, gen, attempt](const TxnResult& r) {
    if (gen != generation_) {
      return;
    }
    if (r.ok()) {
      sendChunk(1);
    } else if (r.status == TxnStatus::Err) {
      fail(targetName() + " refused the update: it may be running or short of space");
    } else if (attempt < kCommandAttempts) {
      sendBegin(attempt + 1);
    } else {
      fail(targetName() + " didn't answer fwbegin");
    }
  });
}

void PumpUpdater::sendChunk(int attempt) {
  if (cancelRequested_) {
    engine_.submit(target().pumpId, "fwabort", nullptr);
    finish("CANCELLED", targetName() + " keeps its current firmware");
    return;
  }
  uint32_t offset = status_.bytes;
  if (offset >= image_.size) {
    sendEnd(1);
    return;
  }
  size_t len = std::min<size_t>(kPumpImageChunk, image_.size - offset);
  uint8_t chunk[kPumpImageChunk];
  if (!store_->read(offset, chunk, len)) {
    fail("Couldn't read the stored firmware file");
    return;
  }
  uint32_t gen = generation_;
  std::string cmd = "fwdata " + std::to_string(offset) + " " + base64Encode(chunk, len);
  engine_.submit(target().pumpId, cmd, [this, gen, attempt, offset, len](const TxnResult& r) {
    if (gen != generation_) {
      return;
    }
    if (r.ok()) {
      status_.bytes = static_cast<uint32_t>(offset + len);
      changed();
      sendChunk(1);
    } else if (r.status == TxnStatus::Err && offset < kLoaderBytes) {
      // The pump ended the update: this image doesn't match its flash
      fail(targetName() + " refused this image: it was built with a different Arduino core or "
                          "flash layout, so flash it over USB");
    } else if (attempt < kChunkAttempts) {
      LOG_D("PUMP", "%s: retrying chunk at %u", targetName().c_str(), static_cast<unsigned>(offset));
      sendChunk(attempt + 1);
    } else {
      fail(targetName() + " stopped responding during the transfer");
    }
  });
}

void PumpUpdater::sendEnd(int attempt) {
  uint32_t gen = generation_;
  engine_.submit(target().pumpId, "fwend", [this, gen, attempt](const TxnResult& r) {
    if (gen != generation_) {
      return;
    }
    if (r.ok()) {
      sendApply();
    } else if (r.status == TxnStatus::Err) {
      fail(targetName() + " rejected the image at the final check");
    } else if (attempt < kCommandAttempts) {
      sendEnd(attempt + 1);  // safe to repeat
    } else {
      fail(targetName() + " didn't answer fwend");
    }
  });
}

void PumpUpdater::sendApply() {
  status_.phase = Phase::Install;
  installing_ = true;
  installDeadline_ = clock_.nowMs() + kInstallWaitMs;
  nextPoll_ = clock_.nowMs() + kBootGraceMs;
  lastSeenVersion_.clear();
  changed();
  LOG_I("PUMP", "%s: image verified; installing", targetName().c_str());
  // No need to look at the reply: the version check below decides
  engine_.submit(target().pumpId, "fwapply", nullptr);
}

void PumpUpdater::tick() {
  if (!installing_ || polling_) {
    return;
  }
  uint32_t now = clock_.nowMs();
  if (timeReached(now, installDeadline_)) {
    std::string seen = lastSeenVersion_.empty() ? "didn't come back" : "is still on " + lastSeenVersion_;
    fail(targetName() + " " + seen + " after the install. Check it, and flash it over USB if needed");
    return;
  }
  if (timeReached(now, nextPoll_)) {
    pollBoot();
  }
}

void PumpUpdater::pollBoot() {
  polling_ = true;
  uint32_t gen = generation_;
  engine_.submit(target().pumpId, "version", [this, gen](const TxnResult& r) {
    if (gen != generation_) {
      return;
    }
    polling_ = false;
    nextPoll_ = clock_.nowMs() + kBootPollMs;
    std::string version;
    if (!r.ok() || !parseVersion(r, version)) {
      return;  // still rebooting
    }
    lastSeenVersion_ = version;
    if (compareVersions(version, image_.version) == 0) {
      targetDone();
    } else {
      // Harmless if nothing is staged, and installs it if the fwapply was lost
      engine_.submit(target().pumpId, "fwapply", nullptr);
    }
  });
}

void PumpUpdater::targetDone() {
  LOG_I("PUMP", "%s is on firmware %s", targetName().c_str(), image_.version.c_str());
  installing_ = false;
  index_++;
  if (index_ >= targets_.size()) {
    finish("COMPLETE", std::to_string(targets_.size()) + " board(s) on firmware " + image_.version);
    return;
  }
  startTarget();
}

OpResult PumpUpdater::cancel() {
  if (!active()) {
    return OpResult::fail(ErrCode::kInvalid, "No update is running");
  }
  cancelRequested_ = true;
  LOG_W("PUMP", "Pump firmware update cancel requested");
  return OpResult::success();
}

void PumpUpdater::abort(const std::string& reason) {
  if (!active()) {
    return;
  }
  bool transferring = status_.phase == Phase::Transfer;
  int pumpId = target().pumpId;
  finish("CANCELLED", reason);
  if (transferring) {
    engine_.submit(pumpId, "fwabort", nullptr, true);
  }
}

bool PumpUpdater::expectingUsbLoss() const {
  return installing_ && target().bridge;
}

void PumpUpdater::fail(const std::string& message) {
  LOG_E("PUMP", "Pump firmware update stopped: %s", message.c_str());
  bool transferring = status_.phase == Phase::Transfer;
  int pumpId = target().pumpId;
  system_.raiseError(ErrorCode::PumpUpdate, pumpId, message);
  finish("FAILED", message);
  if (transferring) {
    engine_.submit(pumpId, "fwabort", nullptr);
  }
}

void PumpUpdater::finish(const char* result, const std::string& message) {
  generation_++;
  installing_ = false;
  polling_ = false;
  status_.phase = Phase::Idle;
  status_.result = result;
  status_.message = message;
  system_.setActivity(Activity::Idle);
  LOG_I("PUMP", "Pump firmware update %s: %s", result, message.c_str());
  changed();
  // Refreshes every board's version, and notices any that didn't come back
  pumps_.startDiscovery();
}

void PumpUpdater::changed() {
  if (onChange_) {
    onChange_();
  }
}

}  // namespace drinkbot
