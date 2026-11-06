#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace drinkbot {

// Wire protocol to DrinkBotPump firmware >= 0.6.0 (SPEC_v3 §5, §6).

enum class ResultCode : uint8_t { Ok, Err, Invalid, Timeout };

struct ParsedLine {
  enum class Kind : uint8_t { Result, Data, Ignored, BadCrc };
  Kind kind = Kind::Ignored;
  bool bridged = false;            // arrived wrapped in $[...|crc]
  ResultCode result = ResultCode::Ok;
  std::string payload;             // "R,OK", "P,WEIGHT,1.0" (unwrapped if bridged)
};

// Classifies one received line. "$[payload|CRC]" lines are CRC-checked
// and unwrapped; a bad CRC yields Kind::BadCrc.
ParsedLine parseLine(const std::string& line);

// Status of a finished (or failed) request/response exchange.
enum class TxnStatus : uint8_t {
  Ok,
  Err,          // pump (or bridge) answered R,ERR
  Invalid,      // R,INVLD
  Timeout,      // no terminal result in time, or bridge reported R,TIMEOUT
  BadCrc,       // a bridged packet failed its CRC
  LinkError,    // USB write failed / device gone / request rejected locally
};

const char* txnStatusName(TxnStatus status);

struct TxnResult {
  TxnStatus status = TxnStatus::Timeout;
  std::vector<std::string> data;   // "P,..." payloads in arrival order
  bool ok() const { return status == TxnStatus::Ok; }
};

// Collects lines for one in-flight request until it is complete.
class TransactionParser {
 public:
  void begin(bool bridged);
  // Returns true once the transaction is complete; result() is then final.
  bool feed(const std::string& line);
  const TxnResult& result() const { return result_; }
  bool active() const { return active_; }

 private:
  void finish(TxnStatus status);

  bool bridged_ = false;
  bool active_ = false;
  TxnResult result_;
};

// Builds the line to send (without '\n'): the command itself for the
// directly attached pump, or "bridge <id> <command>" for any other.
// Returns false (and leaves `out` empty) when the command breaks the
// pump firmware's limits: <= 63 chars, first token <= 15 chars, at most
// 8 tokens on the pump's line (so 6 inside a bridge), no '|', ']' or newline.
bool buildWireCommand(bool direct, int pumpId, const std::string& command, std::string& out);

// --- Response helpers ---------------------------------------------------

// Finds the first "P,<key>,..." data line and returns the fields after the key.
bool findDataFields(const TxnResult& result, const char* key, std::vector<std::string>& fields);

bool parseWeight(const TxnResult& result, float& grams);
bool parseVersion(const TxnResult& result, std::string& version);
bool parseId(const TxnResult& result, int& id);

enum class PumpMotion : uint8_t { Idle, Forward, Reverse };
bool parseStatus(const TxnResult& result, PumpMotion& motion, uint32_t& remainingMs);

// Compares dotted versions numerically, ignoring any "-suffix"
// ("0.3.0-sim" == "0.3.0"). Returns <0, 0 or >0.
int compareVersions(const std::string& a, const std::string& b);

// 0.4.0: `dispense <ms>` takes a run time, not a volume; an older pump would
// read the run time as millilitres. 0.5.0: a 3 s bridge timeout, which
// kBridgeTimeoutMs relies on. 0.6.0: RS485 at 50 kbaud (all pumps on a bus must
// match) and firmware update commands.
constexpr const char* kMinPumpFirmware = "0.6.0";
// Longest single run the pump firmware accepts (kMaxMotionMs, PF-3)
constexpr uint32_t kMaxPumpRunMs = 120000;
constexpr int kBroadcastId = 255;

}  // namespace drinkbot
