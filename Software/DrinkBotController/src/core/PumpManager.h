#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Clock.h"
#include "core/Config.h"
#include "core/Errors.h"
#include "core/TransactionEngine.h"

namespace drinkbot {

enum class LevelState : uint8_t { Unknown, Ok, Low, Empty };
enum class PumpActivity : uint8_t { Idle, Dispensing, Priming, Purging };

const char* levelStateName(LevelState state);
const char* pumpActivityName(PumpActivity activity);

// Last LED state the controller commanded on a pump (shown in the web UI).
struct PumpLed {
  Rgb color;
  int mode = 1;        // pump firmware modes 1-4
  int brightness = 0;  // 0 = off
};

struct PumpState {
  int pumpId = 0;
  std::string liquid;
  bool isBridge = false;
  bool present = false;
  bool fwOk = false;
  std::string fwVersion;
  bool hasWeight = false;
  float weightG = 0;
  float levelMl = 0;
  LevelState levelState = LevelState::Unknown;
  PumpActivity activity = PumpActivity::Idle;
  PumpLed led;
  int pollFailures = 0;

  // Can be used for dispense / prime / purge
  bool usable() const { return present && fwOk; }
};

// Owns the per-pump model: discovery (SPEC_v3 §9.3), background weight
// polling (§9.4) and the pump LED states (§11).
class PumpManager {
 public:
  using ChangeHandler = std::function<void()>;
  using LevelCallback = std::function<void(bool ok, float levelMl)>;

  static constexpr uint32_t kPollIntervalMs = 3000;
  static constexpr int kPollFailuresForMissing = 3;

  PumpManager(TransactionEngine& engine, const IClock& clock, IErrorReporter& errors);

  // Replaces the pump list and runs discovery.
  void applyConfig(const Config& config);
  void startDiscovery();
  bool discovering() const { return discovering_; }
  bool bridgeResponding() const { return bridgeResponding_; }
  const std::string& bridgeVersion() const { return bridgeVersion_; }
  int bridgeId() const { return engine_.bridgeId(); }

  // Background polling only runs while enabled and the link is idle.
  void setPollingEnabled(bool enabled) { pollingEnabled_ = enabled; }
  void tick();

  const std::vector<PumpState>& pumps() const { return pumps_; }
  const PumpState* find(int pumpId) const;
  const PumpConfig* pumpConfig(int pumpId) const { return config_.findPumpById(pumpId); }
  const Config& config() const { return config_; }

  // Reads a fresh weight and updates the level.
  void refreshWeight(int pumpId, LevelCallback done);

  void setActivity(int pumpId, PumpActivity activity);

  // §11: idle state from the level (off / slow yellow / slow red).
  void restoreIdleLed(int pumpId);
  // Liquid color + chase timing; the dispense command itself selects mode 3.
  void prepareDispenseLed(int pumpId);
  // Liquid color bounce (mode 4) for prime / purge.
  void showManualLed(int pumpId);
  // Records an LED change made by another module (e.g. LED test).
  void noteLed(int pumpId, const PumpLed& led);

  void setChangeHandler(ChangeHandler handler) { onChange_ = std::move(handler); }

  static LevelState classifyLevel(float levelMl, const LevelThresholds& levels);

 private:
  PumpState* findMutable(int pumpId);
  void changed();
  void discoverPump(int pumpId);
  void discoveryDone();
  void handleVersion(PumpState& pump, const TxnResult& result, bool fromDiscovery);
  void updateWeight(int pumpId, float grams);
  void pollNext();
  void sendLed(int pumpId, const std::vector<std::string>& commands, const PumpLed& led);

  TransactionEngine& engine_;
  const IClock& clock_;
  IErrorReporter& errors_;
  Config config_;
  std::vector<PumpState> pumps_;
  ChangeHandler onChange_;

  bool discovering_ = false;
  bool bridgeResponding_ = false;
  std::string bridgeVersion_;
  int discoveryPending_ = 0;
  uint32_t discoveryGeneration_ = 0;

  bool pollingEnabled_ = false;
  bool pollInFlight_ = false;
  size_t pollIndex_ = 0;
  uint32_t nextPollMs_ = 0;
};

}  // namespace drinkbot
