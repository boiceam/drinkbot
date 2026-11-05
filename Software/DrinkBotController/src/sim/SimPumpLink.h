#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/Clock.h"
#include "core/Config.h"
#include "core/PumpTransport.h"

namespace drinkbot {

// Simulated bridge pump plus RS485 bus of DrinkBotPump 0.4.0 nodes
// (SPEC_v3 §16). Speaks the same line protocol as the real firmware,
// including bridged $[..|crc] replies, R,TIMEOUT and broadcast 255.
class SimPumpLink : public IPumpTransport {
 public:
  static constexpr int kPumpCount = 8;
  static constexpr uint32_t kDirectLatencyMs = 5;
  static constexpr uint32_t kBridgedLatencyMs = 25;
  static constexpr uint32_t kBridgeTimeoutMs = 2000;
  static constexpr uint32_t kMaxMotionMs = 120000;
  static constexpr float kStartLevelMl = 750;
  // Nominal flow of the physical pump: 600 ml/min = 100 ms/ml. The simulated
  // pumps move liquid at this rate whatever the config's msPerMl says, so a
  // miscalibrated msPerMl shows up in pour verification as on real hardware.
  static constexpr float kNominalMlPerMin = 600;
  static constexpr int kNominalMsPerMl = static_cast<int>(60000 / kNominalMlPerMin);

  struct PumpSpec {
    int id = 0;
    float densityGPerMl = 1.0f;
    int msPerMl = kNominalMsPerMl;  // true flow rate, used to move liquid while running
  };

  struct LedState {
    uint8_t r = 255, g = 0, b = 0;
    int mode = 1;
    int brightness = 0;
  };

  SimPumpLink(const IClock& clock, std::vector<PumpSpec> pumps);

  // IDs from the config's pumps, padded to 8 with IDs 1-8 not already in
  // use. The first configured pump is the bridge.
  static std::vector<PumpSpec> specsFromConfig(const Config& config);

  // IPumpTransport
  bool connected() const override { return true; }
  bool writeLine(const std::string& line) override;
  bool readLine(std::string& line) override;
  void tick() override;

  int bridgeId() const { return pumps_.empty() ? 0 : pumps_.front().id; }
  std::vector<int> pumpIds() const;

  // --- Fault injection & inspection (tests and the Maintenance tab) ---
  void setMissing(int id, bool missing);
  void setWeight(int id, float grams);
  void setFirmwareVersion(int id, const std::string& version);
  // The next command runs but its reply is lost.
  void dropNextResponse(int id);
  // The next command is neither run nor answered.
  void ignoreNextCommand(int id);
  // `status` keeps reporting FWD, as if the pump never stopped, until a `stop`.
  void setStuck(int id, bool stuck);

  float weight(int id) const;
  bool running(int id) const;
  bool reversing(int id) const;
  int msPerMl(int id) const;
  LedState led(int id) const;
  const std::vector<std::string>& received() const { return received_; }
  void clearReceived() { received_.clear(); }

 private:
  struct Pump {
    PumpSpec spec;
    int id = 0;
    std::string version = "0.4.0-sim";
    float weightG = 0;
    bool missing = false;
    int dropResponses = 0;
    int ignoreCommands = 0;
    bool stuck = false;
    bool active = false;
    bool reverse = false;
    uint32_t startMs = 0;
    uint32_t durationMs = 0;
    uint32_t lastUpdateMs = 0;
    LedState led;
  };

  Pump* find(int id);
  const Pump* find(int id) const;
  void advance(Pump& p, uint32_t now);
  // Runs one pump command, returning its reply lines ("P,..", "R,..").
  std::vector<std::string> execute(Pump& p, const std::string& command);
  void schedule(uint32_t delayMs, const std::string& line);
  static std::string wrapBridged(const std::string& payload);
  static bool isBroadcastCommand(const std::string& command);

  const IClock& clock_;
  std::vector<Pump> pumps_;
  std::multimap<uint32_t, std::string> outbox_;
  std::vector<std::string> received_;
  uint32_t bridgePendingUntil_ = 0;
  bool bridgePending_ = false;
};

}  // namespace drinkbot
