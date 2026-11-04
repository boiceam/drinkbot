#pragma once

#include <cstdint>
#include <string>

namespace drinkbot {

// SPEC_v3 §17
enum class ErrorCode : uint8_t {
  UsbLost,
  BridgeUnresponsive,
  PumpMissing,
  PumpFw,
  Config,
  DispenseTimeout,
  PumpComm,
  StopUnconfirmed,
};

enum class ErrorSeverity : uint8_t {
  Blocking,  // no dispense, prime or purge
  Degraded,  // only operations involving the affected pump are blocked
  Latched,   // dispensing is blocked until Clear Errors
};

inline const char* errorCodeName(ErrorCode code) {
  switch (code) {
    case ErrorCode::UsbLost: return "E_USB_LOST";
    case ErrorCode::BridgeUnresponsive: return "E_BRIDGE_UNRESPONSIVE";
    case ErrorCode::PumpMissing: return "E_PUMP_MISSING";
    case ErrorCode::PumpFw: return "E_PUMP_FW";
    case ErrorCode::Config: return "E_CONFIG";
    case ErrorCode::DispenseTimeout: return "E_DISPENSE_TIMEOUT";
    case ErrorCode::PumpComm: return "E_PUMP_COMM";
    case ErrorCode::StopUnconfirmed: return "E_STOP_UNCONFIRMED";
  }
  return "E_UNKNOWN";
}

inline ErrorSeverity errorSeverity(ErrorCode code) {
  switch (code) {
    case ErrorCode::UsbLost:
    case ErrorCode::BridgeUnresponsive:
      return ErrorSeverity::Blocking;
    case ErrorCode::PumpMissing:
    case ErrorCode::PumpFw:
    case ErrorCode::Config:
      return ErrorSeverity::Degraded;
    case ErrorCode::DispenseTimeout:
    case ErrorCode::PumpComm:
    case ErrorCode::StopUnconfirmed:
      return ErrorSeverity::Latched;
  }
  return ErrorSeverity::Blocking;
}

// Implemented by SystemState. pumpId is 0 for errors not tied to a pump.
class IErrorReporter {
 public:
  virtual ~IErrorReporter() = default;
  virtual void raiseError(ErrorCode code, int pumpId, const std::string& message) = 0;
  virtual void clearError(ErrorCode code, int pumpId) = 0;
};

}  // namespace drinkbot
