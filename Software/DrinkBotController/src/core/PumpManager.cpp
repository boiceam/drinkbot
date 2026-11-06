#include "core/PumpManager.h"

#include <algorithm>
#include <cstdio>

#include "core/Log.h"

namespace drinkbot {

namespace {

const Rgb kYellow{255, 180, 0};
const Rgb kRed{255, 0, 0};

std::string ledColorCommand(const Rgb& c) {
  char buf[32];
  snprintf(buf, sizeof(buf), "ledcolor %u %u %u", c.r, c.g, c.b);
  return buf;
}

}  // namespace

const char* levelStateName(LevelState state) {
  switch (state) {
    case LevelState::Unknown: return "UNKNOWN";
    case LevelState::Ok: return "OK";
    case LevelState::Low: return "LOW";
    case LevelState::Empty: return "EMPTY";
  }
  return "UNKNOWN";
}

const char* pumpActivityName(PumpActivity activity) {
  switch (activity) {
    case PumpActivity::Idle: return "IDLE";
    case PumpActivity::Dispensing: return "DISPENSING";
    case PumpActivity::Priming: return "PRIMING";
    case PumpActivity::Purging: return "PURGING";
  }
  return "IDLE";
}

PumpManager::PumpManager(TransactionEngine& engine, const IClock& clock, IErrorReporter& errors)
    : engine_(engine), clock_(clock), errors_(errors) {}

LevelState PumpManager::classifyLevel(float levelMl, const LevelThresholds& levels) {
  if (levelMl <= levels.emptyMl) {
    return LevelState::Empty;
  }
  if (levelMl <= levels.lowMl) {
    return LevelState::Low;
  }
  return LevelState::Ok;
}

const PumpState* PumpManager::find(int pumpId) const {
  for (const auto& p : pumps_) {
    if (p.pumpId == pumpId) {
      return &p;
    }
  }
  return nullptr;
}

PumpState* PumpManager::findMutable(int pumpId) {
  return const_cast<PumpState*>(find(pumpId));
}

void PumpManager::changed() {
  if (onChange_) {
    onChange_();
  }
}

void PumpManager::applyConfig(const Config& config) {
  // Errors for pumps that are no longer configured go away with them
  for (const auto& old : pumps_) {
    if (!config.findPumpById(old.pumpId)) {
      errors_.clearError(ErrorCode::PumpMissing, old.pumpId);
      errors_.clearError(ErrorCode::PumpFw, old.pumpId);
    }
  }
  config_ = config;
  std::vector<PumpState> next;
  for (const auto& pc : config_.pumps) {
    PumpState s;
    if (const PumpState* prev = find(pc.pumpId)) {
      s = *prev;
    }
    s.pumpId = pc.pumpId;
    s.liquid = pc.liquid;
    if (s.hasWeight) {
      s.levelMl = std::max(0.0f, s.weightG / pc.densityGPerMl);
      s.levelState = classifyLevel(s.levelMl, config_.levels);
    }
    next.push_back(s);
  }
  pumps_ = std::move(next);
  changed();
  startDiscovery();
}

void PumpManager::startDiscovery() {
  discovering_ = true;
  discoveryGeneration_++;
  discoveryPending_ = 0;
  uint32_t gen = discoveryGeneration_;
  LOG_I("PUMP", "Discovering pumps (%u configured)", static_cast<unsigned>(pumps_.size()));

  engine_.submit(TransactionEngine::kDirect, "version", [this, gen](const TxnResult& r) {
    if (gen != discoveryGeneration_) {
      return;
    }
    std::string version;
    if (!r.ok() || !parseVersion(r, version)) {
      bridgeResponding_ = false;
      errors_.raiseError(ErrorCode::BridgeUnresponsive, 0, "Bridge did not answer 'version'");
      for (auto& p : pumps_) {
        p.present = false;
      }
      discoveryDone();
      return;
    }
    engine_.submit(TransactionEngine::kDirect, "gid", [this, gen, version](const TxnResult& g) {
      if (gen != discoveryGeneration_) {
        return;
      }
      int id = 0;
      if (!g.ok() || !parseId(g, id)) {
        bridgeResponding_ = false;
        errors_.raiseError(ErrorCode::BridgeUnresponsive, 0, "Bridge did not answer 'gid'");
        discoveryDone();
        return;
      }
      bridgeResponding_ = true;
      errors_.clearError(ErrorCode::BridgeUnresponsive, 0);
      engine_.setBridgeId(id);
      if (id == kBridgeBoardId) {
        LOG_I("PUMP", "Dedicated bridge board, firmware %s", version.c_str());
      } else {
        LOG_I("PUMP", "Bridge is pump ID %d, firmware %s", id, version.c_str());
      }
      // A pump's own version is checked below; a bridge board isn't in the pump
      // list, and if it's too old (a different bus speed) no pump would answer.
      if (compareVersions(version, kMinPumpFirmware) < 0) {
        errors_.raiseError(ErrorCode::PumpFw, kBridgeBoardId,
                           "Bridge firmware " + version + " is older than the required " + kMinPumpFirmware);
      } else {
        errors_.clearError(ErrorCode::PumpFw, kBridgeBoardId);
      }
      bool inList = false;
      for (auto& p : pumps_) {
        p.isBridge = p.pumpId == id;
        inList |= p.isBridge;
      }
      if (!inList && id != kBridgeBoardId) {
        LOG_W("PUMP", "Bridge pump ID %d is not in the pump list; it is used as a relay only", id);
      }
      for (const auto& p : pumps_) {
        discoverPump(p.pumpId);
      }
      if (discoveryPending_ == 0) {
        discoveryDone();
      }
    });
  });
}

void PumpManager::discoverPump(int pumpId) {
  uint32_t gen = discoveryGeneration_;
  discoveryPending_++;
  engine_.submit(pumpId, "version", [this, gen, pumpId](const TxnResult& r) {
    if (gen != discoveryGeneration_) {
      return;
    }
    PumpState* p = findMutable(pumpId);
    if (p) {
      handleVersion(*p, r, true);
    }
    if (p && p->usable()) {
      discoveryPending_++;
      engine_.submit(pumpId, "weight", [this, gen, pumpId](const TxnResult& w) {
        if (gen != discoveryGeneration_) {
          return;
        }
        float grams = 0;
        if (w.ok() && parseWeight(w, grams)) {
          updateWeight(pumpId, grams);
        }
        restoreIdleLed(pumpId);
        if (--discoveryPending_ == 0) {
          discoveryDone();
        }
      });
    }
    if (--discoveryPending_ == 0) {
      discoveryDone();
    }
  });
}

void PumpManager::discoveryDone() {
  discovering_ = false;
  int present = 0;
  for (const auto& p : pumps_) {
    present += p.present ? 1 : 0;
  }
  LOG_I("PUMP", "Discovery done: %d of %u pumps present", present, static_cast<unsigned>(pumps_.size()));
  nextPollMs_ = clock_.nowMs() + kPollIntervalMs;
  changed();
}

void PumpManager::handleVersion(PumpState& pump, const TxnResult& result, bool fromDiscovery) {
  std::string version;
  if (!result.ok() || !parseVersion(result, version)) {
    if (fromDiscovery || pump.present) {
      LOG_W("PUMP", "Pump %d did not answer", pump.pumpId);
    }
    pump.present = false;
    errors_.raiseError(ErrorCode::PumpMissing, pump.pumpId,
                       "Pump " + std::to_string(pump.pumpId) + " is not responding");
    changed();
    return;
  }
  bool wasPresent = pump.present;
  pump.present = true;
  pump.pollFailures = 0;
  pump.fwVersion = version;
  pump.fwOk = compareVersions(version, kMinPumpFirmware) >= 0;
  errors_.clearError(ErrorCode::PumpMissing, pump.pumpId);
  if (pump.fwOk) {
    errors_.clearError(ErrorCode::PumpFw, pump.pumpId);
  } else {
    errors_.raiseError(ErrorCode::PumpFw, pump.pumpId,
                       "Pump " + std::to_string(pump.pumpId) + " firmware " + version + " is older than " +
                           kMinPumpFirmware);
  }
  if (!wasPresent) {
    LOG_I("PUMP", "Pump %d present, firmware %s", pump.pumpId, version.c_str());
  }
  changed();
}

void PumpManager::updateWeight(int pumpId, float grams) {
  PumpState* p = findMutable(pumpId);
  const PumpConfig* pc = config_.findPumpById(pumpId);
  if (!p || !pc) {
    return;
  }
  p->hasWeight = true;
  p->weightG = grams;
  p->levelMl = std::max(0.0f, grams / pc->densityGPerMl);
  LevelState next = classifyLevel(p->levelMl, config_.levels);
  if (next != p->levelState) {
    LevelState prev = p->levelState;
    p->levelState = next;
    if (prev != LevelState::Unknown || next != LevelState::Ok) {
      LOG_I("PUMP", "Pump %d (%s) level %s: %.0f ml", pumpId, p->liquid.c_str(), levelStateName(next),
            p->levelMl);
    }
    if (p->activity == PumpActivity::Idle) {
      restoreIdleLed(pumpId);
    }
  }
  changed();
}

void PumpManager::refreshWeight(int pumpId, LevelCallback done) {
  engine_.submit(pumpId, "weight", [this, pumpId, done](const TxnResult& r) {
    float grams = 0;
    if (r.ok() && parseWeight(r, grams)) {
      updateWeight(pumpId, grams);
      const PumpState* p = find(pumpId);
      if (done) {
        done(true, p ? p->levelMl : 0);
      }
    } else if (done) {
      done(false, 0);
    }
  });
}

void PumpManager::setActivity(int pumpId, PumpActivity activity) {
  if (PumpState* p = findMutable(pumpId)) {
    if (p->activity != activity) {
      p->activity = activity;
      changed();
    }
  }
}

void PumpManager::sendLed(int pumpId, const std::vector<std::string>& commands, const PumpLed& led) {
  for (const auto& c : commands) {
    engine_.submit(pumpId, c, nullptr);
  }
  noteLed(pumpId, led);
}

void PumpManager::noteLed(int pumpId, const PumpLed& led) {
  if (PumpState* p = findMutable(pumpId)) {
    p->led = led;
    changed();
  }
}

void PumpManager::restoreIdleLed(int pumpId) {
  const PumpState* p = find(pumpId);
  if (!p || !p->usable()) {
    return;
  }
  switch (p->levelState) {
    case LevelState::Low:
      sendLed(pumpId, {"ledtime 0", ledColorCommand(kYellow), "ledfade 1500 500 1500 500", "ledmode 2 255"},
              {kYellow, 2, 255});
      break;
    case LevelState::Empty:
      sendLed(pumpId, {"ledtime 0", ledColorCommand(kRed), "ledfade 1500 500 1500 500", "ledmode 2 255"},
              {kRed, 2, 255});
      break;
    case LevelState::Ok:
    case LevelState::Unknown:
      // D-3: idle pumps with liquid are dark
      sendLed(pumpId, {"ledtime 0", "ledmode 1 0"}, {p->led.color, 1, 0});
      break;
  }
}

void PumpManager::prepareDispenseLed(int pumpId) {
  const PumpConfig* pc = config_.findPumpById(pumpId);
  if (!pc) {
    return;
  }
  sendLed(pumpId, {"ledtime 0", ledColorCommand(pc->color), "ledchase 50 4"}, {pc->color, 3, 255});
}

void PumpManager::showManualLed(int pumpId) {
  const PumpConfig* pc = config_.findPumpById(pumpId);
  if (!pc) {
    return;
  }
  sendLed(pumpId, {"ledtime 0", ledColorCommand(pc->color), "ledchase 50 4", "ledmode 4 255"},
          {pc->color, 4, 255});
}

void PumpManager::tick() {
  if (!pollingEnabled_ || discovering_ || pollInFlight_ || !bridgeResponding_ || pumps_.empty() ||
      !engine_.idle()) {
    return;
  }
  uint32_t now = clock_.nowMs();
  if (!timeReached(now, nextPollMs_)) {
    return;
  }
  nextPollMs_ = now + kPollIntervalMs / static_cast<uint32_t>(pumps_.size());
  pollNext();
}

void PumpManager::pollNext() {
  pollIndex_ %= pumps_.size();
  int pumpId = pumps_[pollIndex_++].pumpId;
  const PumpState* p = find(pumpId);
  pollInFlight_ = true;

  if (!p->present) {
    // A missing pump is asked for its version, so it can come back on its own
    engine_.submit(pumpId, "version", [this, pumpId](const TxnResult& r) {
      pollInFlight_ = false;
      if (PumpState* ps = findMutable(pumpId)) {
        if (r.ok()) {
          handleVersion(*ps, r, false);
        }
      }
    });
    return;
  }

  engine_.submit(pumpId, "weight", [this, pumpId](const TxnResult& r) {
    pollInFlight_ = false;
    PumpState* ps = findMutable(pumpId);
    if (!ps) {
      return;
    }
    float grams = 0;
    if (r.ok() && parseWeight(r, grams)) {
      ps->pollFailures = 0;
      updateWeight(pumpId, grams);
      return;
    }
    if (++ps->pollFailures >= kPollFailuresForMissing && ps->present) {
      ps->present = false;
      LOG_W("PUMP", "Pump %d missed %d polls in a row", pumpId, kPollFailuresForMissing);
      errors_.raiseError(ErrorCode::PumpMissing, pumpId, "Pump " + std::to_string(pumpId) + " is not responding");
      changed();
    }
  });
}

}  // namespace drinkbot
