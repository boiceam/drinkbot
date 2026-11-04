#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Errors.h"

namespace drinkbot {

// SPEC_v3 §8.1
enum class SysState : uint8_t { Booting, Ready, Dispensing, Manual, Error };

const char* sysStateName(SysState state);

// What the system is busy doing; the published state is derived from it.
enum class Activity : uint8_t { Idle, Dispensing, Manual };

struct ActiveError {
  ErrorCode code;
  int pumpId = 0;
  std::string message;
  bool latched() const { return errorSeverity(code) == ErrorSeverity::Latched; }
};

// Global state machine and error catalog (§8, §17).
class SystemState : public IErrorReporter {
 public:
  using ChangeHandler = std::function<void()>;

  void setBooting(bool booting);
  bool booting() const { return booting_; }

  void setActivity(Activity activity);
  Activity activity() const { return activity_; }
  bool busy() const { return activity_ != Activity::Idle; }

  // DISPENSING / MANUAL while busy, otherwise ERROR if any error is
  // active, otherwise READY. BOOTING wins over everything.
  SysState state() const;

  // IErrorReporter
  void raiseError(ErrorCode code, int pumpId, const std::string& message) override;
  void clearError(ErrorCode code, int pumpId) override;

  // Clears latched errors only (Clear Errors command, 3 s E-Stop hold).
  // Returns how many were cleared.
  int clearLatched();

  const std::vector<ActiveError>& errors() const { return errors_; }
  bool hasErrors() const { return !errors_.empty(); }
  bool hasBlockingError() const;
  bool hasLatchedError() const;
  // A degraded error tied to this pump (missing, old firmware).
  bool pumpBlocked(int pumpId) const;

  void setChangeHandler(ChangeHandler handler) { onChange_ = std::move(handler); }

 private:
  void changed();

  bool booting_ = true;
  Activity activity_ = Activity::Idle;
  std::vector<ActiveError> errors_;
  ChangeHandler onChange_;
};

}  // namespace drinkbot
