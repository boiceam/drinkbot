#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/Config.h"

namespace drinkbot {

// Error codes returned to a requester (SPEC_v3 §14.2)
namespace ErrCode {
constexpr const char* kBusy = "BUSY";
constexpr const char* kInvalid = "INVALID";
constexpr const char* kNotFound = "NOT_FOUND";
constexpr const char* kPrecheckFailed = "PRECHECK_FAILED";
constexpr const char* kPumpTimeout = "PUMP_TIMEOUT";
constexpr const char* kPumpError = "PUMP_ERROR";
constexpr const char* kConfigInvalid = "CONFIG_INVALID";
constexpr const char* kInternal = "INTERNAL";
}  // namespace ErrCode

struct OpResult {
  bool ok = true;
  std::string code;     // ErrCode::* when !ok
  std::string message;

  static OpResult success() { return {}; }
  static OpResult fail(const char* code, const std::string& message) { return {false, code, message}; }
};

// §14.1 dispenseProgress
struct DispenseProgress {
  enum class Status : uint8_t { Starting, Done, Complete, Aborted };
  std::string recipeId;
  float volumeMl = 0;
  int step = 0;   // 1-based; on COMPLETE = steps, on ABORTED = the step that was running
  int steps = 0;
  std::string liquid;
  int pumpId = 0;
  float targetMl = 0;
  bool hasActual = false;
  float actualMl = 0;
  Status status = Status::Starting;
  std::string reason;
};

const char* dispenseStatusName(DispenseProgress::Status status);

// §9.6
enum class BulkOp : uint8_t { Prime, Purge, PrimeAll, PurgeAll };

const char* bulkOpName(BulkOp op);
inline bool bulkOpIsReverse(BulkOp op) { return op == BulkOp::Purge || op == BulkOp::PurgeAll; }
inline bool bulkOpIsAll(BulkOp op) { return op == BulkOp::PrimeAll || op == BulkOp::PurgeAll; }

struct BulkStep {
  int pumpId = 0;
  std::string liquid;
  uint32_t durationMs = 0;
  bool skip = false;
  std::string skipReason;
};

struct BulkPlan {
  BulkOp op = BulkOp::PrimeAll;
  std::vector<BulkStep> steps;
  uint32_t estimatedTotalMs = 0;
};

// §14.1 bulkProgress
struct BulkProgress {
  enum class Status : uint8_t { Starting, Running, Done, Skipped, Complete, Aborted };
  BulkOp op = BulkOp::PrimeAll;
  int step = 0;   // 1-based; 0 on STARTING / COMPLETE, the running step on ABORTED
  int steps = 0;
  int pumpId = 0;
  std::string liquid;
  uint32_t durationMs = 0;
  uint32_t estimatedTotalMs = 0;
  Status status = Status::Starting;
  std::string reason;
};

const char* bulkStatusName(BulkProgress::Status status);

}  // namespace drinkbot
