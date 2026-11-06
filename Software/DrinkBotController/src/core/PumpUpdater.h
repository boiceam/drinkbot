#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Clock.h"
#include "core/Operations.h"
#include "core/PumpImage.h"
#include "core/PumpManager.h"
#include "core/SystemState.h"
#include "core/TransactionEngine.h"

namespace drinkbot {

// Updates pumps and the bridge from the stored image, one at a time, when
// asked (SPEC_v3 §9.10). Each board must come back on the new version before
// the next one starts, so a bad image stops at the first board. The bridge
// goes last: while it reboots, every pump is out of reach.
class PumpUpdater {
 public:
  static constexpr int kChunkAttempts = 5;
  static constexpr int kCommandAttempts = 3;
  static constexpr uint32_t kBootGraceMs = 1500;
  static constexpr uint32_t kBootPollMs = 1500;
  static constexpr uint32_t kInstallWaitMs = 60000;
  static constexpr uint32_t kLoaderBytes = 0x3000;  // pumps compare these with their own flash

  struct Target {
    int pumpId = 0;  // 0 = the bridge board
    std::string version;
    bool bridge = false;
  };

  // What an update would do with the current image and pumps.
  struct Plan {
    std::vector<Target> targets;  // in update order
    std::vector<int> needsUsb;    // too old to update over the bus
    std::vector<int> missing;     // not answering, so left alone
  };

  enum class Phase : uint8_t { Idle, Transfer, Install };

  struct Status {
    Phase phase = Phase::Idle;
    int pumpId = 0;
    int index = 0;  // 1-based board in this run
    int total = 0;
    uint32_t bytes = 0;
    uint32_t size = 0;
    std::string result;   // "" while running or never run; COMPLETE, FAILED, CANCELLED
    std::string message;  // why it failed, or what was done
  };

  PumpUpdater(TransactionEngine& engine, PumpManager& pumps, SystemState& system, const IClock& clock);

  void setStore(IPumpImageStore* store) { store_ = store; }
  IPumpImageStore* store() const { return store_; }
  Plan plan() const;
  const Status& status() const { return status_; }
  bool active() const { return status_.phase != Phase::Idle; }

  OpResult start();
  // Stops before the next step and cancels what the current board received.
  // An install already under way can't be interrupted.
  OpResult cancel();
  // E-Stop or USB loss: stop right away, whatever the phase.
  void abort(const std::string& reason);
  void tick();

  // The bridge is rebooting on purpose, so its USB link is expected to drop.
  bool expectingUsbLoss() const;

  void setChangeHandler(std::function<void()> handler) { onChange_ = std::move(handler); }

 private:
  void startTarget();
  void sendBegin(int attempt);
  void sendChunk(int attempt);
  void sendEnd(int attempt);
  void sendApply();
  void pollBoot();
  void targetDone();
  void finish(const char* result, const std::string& message);
  void fail(const std::string& message);
  void changed();
  const Target& target() const { return targets_[index_]; }
  std::string targetName() const;

  TransactionEngine& engine_;
  PumpManager& pumps_;
  SystemState& system_;
  const IClock& clock_;
  IPumpImageStore* store_ = nullptr;
  std::function<void()> onChange_;

  Status status_;
  PumpImageInfo image_;
  std::vector<Target> targets_;
  size_t index_ = 0;
  uint32_t generation_ = 0;
  bool cancelRequested_ = false;
  bool installing_ = false;
  bool polling_ = false;
  uint32_t installDeadline_ = 0;
  uint32_t nextPoll_ = 0;
  std::string lastSeenVersion_;
};

}  // namespace drinkbot
