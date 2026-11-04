#include "core/SystemState.h"

#include <algorithm>

#include "core/Log.h"

namespace drinkbot {

const char* sysStateName(SysState state) {
  switch (state) {
    case SysState::Booting: return "BOOTING";
    case SysState::Ready: return "READY";
    case SysState::Dispensing: return "DISPENSING";
    case SysState::Manual: return "MANUAL";
    case SysState::Error: return "ERROR";
  }
  return "ERROR";
}

void SystemState::changed() {
  if (onChange_) {
    onChange_();
  }
}

void SystemState::setBooting(bool booting) {
  if (booting_ != booting) {
    booting_ = booting;
    changed();
  }
}

void SystemState::setActivity(Activity activity) {
  if (activity_ != activity) {
    activity_ = activity;
    changed();
  }
}

SysState SystemState::state() const {
  if (booting_) {
    return SysState::Booting;
  }
  if (activity_ == Activity::Dispensing) {
    return SysState::Dispensing;
  }
  if (activity_ == Activity::Manual) {
    return SysState::Manual;
  }
  return errors_.empty() ? SysState::Ready : SysState::Error;
}

void SystemState::raiseError(ErrorCode code, int pumpId, const std::string& message) {
  for (auto& e : errors_) {
    if (e.code == code && e.pumpId == pumpId) {
      if (e.message != message) {
        e.message = message;
        changed();
      }
      return;
    }
  }
  errors_.push_back({code, pumpId, message});
  LOG_E("SYS", "%s raised: %s", errorCodeName(code), message.c_str());
  changed();
}

void SystemState::clearError(ErrorCode code, int pumpId) {
  auto it = std::find_if(errors_.begin(), errors_.end(),
                         [&](const ActiveError& e) { return e.code == code && e.pumpId == pumpId; });
  if (it == errors_.end()) {
    return;
  }
  LOG_I("SYS", "%s cleared%s", errorCodeName(code),
        pumpId ? (" (pump " + std::to_string(pumpId) + ")").c_str() : "");
  errors_.erase(it);
  changed();
}

int SystemState::clearLatched() {
  size_t before = errors_.size();
  errors_.erase(std::remove_if(errors_.begin(), errors_.end(), [](const ActiveError& e) { return e.latched(); }),
                errors_.end());
  int cleared = static_cast<int>(before - errors_.size());
  if (cleared > 0) {
    LOG_I("SYS", "Cleared %d latched error(s)", cleared);
    changed();
  }
  return cleared;
}

bool SystemState::hasBlockingError() const {
  return std::any_of(errors_.begin(), errors_.end(),
                     [](const ActiveError& e) { return errorSeverity(e.code) == ErrorSeverity::Blocking; });
}

bool SystemState::hasLatchedError() const {
  return std::any_of(errors_.begin(), errors_.end(), [](const ActiveError& e) { return e.latched(); });
}

bool SystemState::pumpBlocked(int pumpId) const {
  return std::any_of(errors_.begin(), errors_.end(), [&](const ActiveError& e) {
    return e.pumpId == pumpId && errorSeverity(e.code) == ErrorSeverity::Degraded;
  });
}

}  // namespace drinkbot
