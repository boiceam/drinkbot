#pragma once

#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/BulkSequencer.h"
#include "core/Clock.h"
#include "core/Config.h"
#include "core/Dispenser.h"
#include "core/Operations.h"
#include "core/PumpManager.h"
#include "core/PumpUpdater.h"
#include "core/PumpTransport.h"
#include "core/SystemState.h"
#include "core/TransactionEngine.h"

namespace drinkbot {

class SimPumpLink;

// Outcome reported back to whoever posted a command.
struct CommandResult {
  bool ok = true;
  std::string code;                   // ErrCode::* when !ok
  std::string message;
  std::vector<std::string> lines;     // pumpRaw reply lines
  std::vector<ConfigError> configErrors;
};

// One request to the App, from the web UI, a button or the system.
struct Command {
  enum class Type : uint8_t {
    SetRecipe,      // text = recipeId
    SetVolume,      // number = ml
    Dispense,       // text = recipeId (optional), number = ml (optional)
    Prime,          // pumpId; runs until PumpStop (at most 120 s), stopping any other manual run first
    Purge,          // pumpId; as Prime, in reverse
    PumpStop,       // pumpId; stops the manual prime/purge
    PrimeAll,
    PurgeAll,
    Estop,
    ClearErrors,
    Rescan,
    Reboot,         // flag = force
    SaveConfig,     // text = config JSON
    PumpTare,       // pumpId
    PumpCalibrate,  // pumpId, number = grams
    PumpSetId,      // pumpId, newId
    PumpLedTest,    // pumpId
    PumpRaw,        // pumpId, text = command
    PumpFwUpdate,   // update every outdated board from the stored image
    PumpFwCancel,
    PumpFwDelete,   // forget the stored image
    PumpFwChanged,  // a new image was stored
    SimFault,       // pumpId, text = missing | present | timeout | overrun | weight, number = grams
  };

  Type type = Type::Estop;
  std::string text;
  float number = NAN;
  int pumpId = 0;
  int newId = 0;
  bool flag = false;
  std::string source = "web";  // for the log ("web 192.168.1.20", "button 6")
  std::function<void(const CommandResult&)> reply;
};

// Receives App events, always on the App task.
class IAppListener {
 public:
  virtual ~IAppListener() = default;
  virtual void onStateChanged() {}
  virtual void onPumpsChanged() {}
  virtual void onConfigChanged() {}
  virtual void onDispenseProgress(const DispenseProgress&) {}
  virtual void onBulkProgress(const BulkProgress&) {}
  virtual void onPumpFwChanged() {}
};

// Owns all controller logic and runs on one task (SPEC_v3 §4.4). Other
// tasks only post() commands; tick() drains them (E-Stop first) and
// drives the pump link, sequencers and polling.
class App {
 public:
  struct Hooks {
    // Persists validated config JSON. Returns false on a storage failure.
    std::function<bool(const std::string& json)> saveConfig;
    std::function<void()> reboot;
  };

  static constexpr uint32_t kStateMinIntervalMs = 100;   // state <= 10/s
  static constexpr uint32_t kPumpsMinIntervalMs = 250;
  static constexpr uint32_t kPumpsPeriodicMs = 3000;
  static constexpr uint32_t kRebootDelayMs = 500;
  static constexpr uint32_t kPumpFwMinIntervalMs = 500;

  App(IPumpTransport& transport, const IClock& clock, SimPumpLink* simulator = nullptr);

  void setHooks(Hooks hooks) { hooks_ = std::move(hooks); }
  void setPumpImageStore(IPumpImageStore* store) { updater_.setStore(store); }
  void addListener(IAppListener* listener) { listeners_.push_back(listener); }

  // Loads config JSON (falling back to the factory default and raising
  // E_CONFIG if it is invalid or `usedFallback`), then starts discovery.
  void begin(const std::string& configJson, bool usedFallback);

  // Thread-safe. E-Stop jumps ahead of everything already queued.
  void post(Command command);

  void tick();

  // --- Read access for listeners (App task only) ---
  const SystemState& system() const { return system_; }
  const PumpManager& pumps() const { return pumps_; }
  const Config& config() const { return config_; }
  const std::string& configJson() const { return configJson_; }
  const std::string& activeRecipeId() const { return activeRecipeId_; }
  float activeVolumeMl() const { return activeVolumeMl_; }
  bool simulator() const { return sim_ != nullptr; }
  bool dispensing() const { return dispenser_.active(); }
  BulkPlan bulkPlan(BulkOp op) const { return bulk_.plan(op); }
  const PumpUpdater& updater() const { return updater_; }
  // Why a recipe can't be poured right now ("" = available).
  std::string recipeUnavailableReason(const Recipe& recipe) const;

 private:
  void handle(Command& cmd);
  void reply(Command& cmd, const CommandResult& result);
  void replyOp(Command& cmd, const OpResult& result);
  void estop(const std::string& source);
  void applyConfig(const std::string& json, const Config& config);
  bool guardIdle(Command& cmd);
  void runMaintenance(Command& cmd, const std::vector<std::string>& commands);
  void handleSetId(Command& cmd);
  void handleLedTest(Command& cmd);
  void handleSimFault(Command& cmd);
  void checkUsb();
  void handlePumpFw(Command& cmd);
  void emitEvents();

  IPumpTransport& transport_;
  const IClock& clock_;
  SimPumpLink* sim_;
  Hooks hooks_;
  std::vector<IAppListener*> listeners_;

  SystemState system_;
  TransactionEngine engine_;
  PumpManager pumps_;
  Dispenser dispenser_;
  BulkSequencer bulk_;
  PumpUpdater updater_;
  bool pumpFwDirty_ = true;
  uint32_t lastPumpFwEmit_ = 0;
  bool usbLossExpected_ = false;  // the bridge is rebooting into new firmware

  Config config_;
  std::string configJson_;
  std::string activeRecipeId_;
  float activeVolumeMl_ = 250;

  std::mutex queueMutex_;
  std::deque<Command> queue_;

  bool started_ = false;
  bool wasConnected_ = false;
  bool maintenanceBusy_ = false;
  bool rebootPending_ = false;
  uint32_t rebootAt_ = 0;

  // LED test: timed commands to one pump
  struct TimedCommand {
    uint32_t atMs;
    std::string command;
  };
  int ledTestPump_ = 0;
  std::deque<TimedCommand> ledTest_;

  bool stateDirty_ = true;
  bool pumpsDirty_ = true;
  uint32_t lastStateEmit_ = 0;
  uint32_t lastPumpsEmit_ = 0;
};

}  // namespace drinkbot
