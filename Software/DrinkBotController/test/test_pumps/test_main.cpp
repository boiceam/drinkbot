// Transaction engine, simulator and pump manager (SPEC_v3 §5.5, §9.3, §9.4, §11, §16)

#include <unity.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "core/PumpManager.h"
#include "core/TransactionEngine.h"
#include "sim/SimPumpLink.h"

using namespace drinkbot;

namespace {

class FakeClock : public IClock {
 public:
  uint32_t t = 1000;
  uint32_t nowMs() const override { return t; }
};

class ErrorRecorder : public IErrorReporter {
 public:
  struct Active {
    ErrorCode code;
    int pumpId;
  };
  std::vector<Active> active;

  void raiseError(ErrorCode code, int pumpId, const std::string&) override {
    if (!has(code, pumpId)) {
      active.push_back({code, pumpId});
    }
  }
  void clearError(ErrorCode code, int pumpId) override {
    active.erase(std::remove_if(active.begin(), active.end(),
                                [&](const Active& a) { return a.code == code && a.pumpId == pumpId; }),
                 active.end());
  }
  bool has(ErrorCode code, int pumpId) const {
    return std::any_of(active.begin(), active.end(),
                       [&](const Active& a) { return a.code == code && a.pumpId == pumpId; });
  }
};

Config testConfig() {
  Config c;
  c.pumps = {
      {1, "Tequila", 100, 1.0f, {255, 160, 0}, 4},
      {2, "Triple Sec", 100, 1.05f, {255, 120, 0}, 0},
      {4, "Lime Juice", 110, 1.0f, {0, 255, 0}, 2.5f},
  };
  return c;
}

struct Rig {
  FakeClock clock;
  Config config = testConfig();
  SimPumpLink sim{clock, SimPumpLink::specsFromConfig(config)};
  TransactionEngine engine{sim, clock};
  ErrorRecorder errors;
  PumpManager manager{engine, clock, errors};

  void run(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
      clock.t++;
      sim.tick();
      engine.tick();
      manager.tick();
    }
  }

  // Runs until the engine has nothing left to do (bounded).
  void settle(uint32_t maxMs = 20000) {
    for (uint32_t i = 0; i < maxMs && (!engine.idle() || manager.discovering()); i++) {
      run(1);
    }
  }

  void discover() {
    manager.applyConfig(config);
    settle();
  }

  TxnResult request(int pumpId, const std::string& cmd) {
    auto out = std::make_shared<TxnResult>();
    bool done = false;
    engine.submit(pumpId, cmd, [&, out](const TxnResult& r) {
      *out = r;
      done = true;
    });
    for (int i = 0; i < 20000 && !done; i++) {
      run(1);
    }
    TEST_ASSERT_TRUE_MESSAGE(done, "request never completed");
    return *out;
  }

  int count(const std::string& line) const {
    const auto& r = sim.received();
    return static_cast<int>(std::count(r.begin(), r.end(), line));
  }
};

}  // namespace

void setUp() {}
void tearDown() {}

void test_sim_pads_to_eight_pumps_with_first_as_bridge() {
  Rig rig;
  auto ids = rig.sim.pumpIds();
  TEST_ASSERT_EQUAL(8, ids.size());
  TEST_ASSERT_EQUAL(1, rig.sim.bridgeId());
  TEST_ASSERT_EQUAL(4, ids[2]);
  TEST_ASSERT_EQUAL(3, ids[3]);  // padding fills unused IDs from 1
}

void test_discovery_routes_bridge_direct_and_others_bridged() {
  Rig rig;
  rig.discover();
  TEST_ASSERT_EQUAL(1, rig.manager.bridgeId());
  TEST_ASSERT_TRUE(rig.manager.bridgeResponding());
  for (const auto& p : rig.manager.pumps()) {
    TEST_ASSERT_TRUE_MESSAGE(p.present, "pump not present");
    TEST_ASSERT_TRUE(p.fwOk);
    TEST_ASSERT_TRUE(p.hasWeight);
    TEST_ASSERT_TRUE(p.levelState == LevelState::Ok);
  }
  TEST_ASSERT_TRUE(rig.manager.find(1)->isBridge);
  TEST_ASSERT_EQUAL(0, rig.count("bridge 1 version"));  // the bridge pump is never bridged
  TEST_ASSERT_EQUAL(1, rig.count("bridge 2 version"));
  TEST_ASSERT_EQUAL(1, rig.count("bridge 4 weight"));
  TEST_ASSERT_TRUE(rig.count("ledtime 0") >= 1);  // cancels the pump boot animation timer
  TEST_ASSERT_EQUAL(0, rig.errors.active.size());
}

void test_missing_pump_is_degraded_and_recovers_by_polling() {
  Rig rig;
  rig.sim.setMissing(4, true);
  rig.discover();
  TEST_ASSERT_FALSE(rig.manager.find(4)->present);
  TEST_ASSERT_TRUE(rig.manager.find(2)->present);
  TEST_ASSERT_TRUE(rig.errors.has(ErrorCode::PumpMissing, 4));

  rig.sim.setMissing(4, false);
  rig.manager.setPollingEnabled(true);
  rig.run(10000);
  TEST_ASSERT_TRUE(rig.manager.find(4)->present);
  TEST_ASSERT_FALSE(rig.errors.has(ErrorCode::PumpMissing, 4));
}

void test_three_failed_polls_mark_pump_missing() {
  Rig rig;
  rig.discover();
  rig.manager.setPollingEnabled(true);
  rig.sim.setMissing(2, true);
  rig.run(30000);
  TEST_ASSERT_FALSE(rig.manager.find(2)->present);
  TEST_ASSERT_TRUE(rig.errors.has(ErrorCode::PumpMissing, 2));
}

void test_old_firmware_raises_pump_fw() {
  Rig rig;
  rig.sim.setFirmwareVersion(2, "0.2.0");
  rig.discover();
  TEST_ASSERT_TRUE(rig.manager.find(2)->present);
  TEST_ASSERT_FALSE(rig.manager.find(2)->usable());
  TEST_ASSERT_TRUE(rig.errors.has(ErrorCode::PumpFw, 2));
}

void test_unresponsive_bridge_is_blocking() {
  Rig rig;
  for (int i = 0; i < 3; i++) {  // first try + 2 retries
    rig.sim.ignoreNextCommand(1);
  }
  rig.discover();
  TEST_ASSERT_FALSE(rig.manager.bridgeResponding());
  TEST_ASSERT_TRUE(rig.errors.has(ErrorCode::BridgeUnresponsive, 0));
  TEST_ASSERT_TRUE(errorSeverity(ErrorCode::BridgeUnresponsive) == ErrorSeverity::Blocking);
}

void test_level_states_drive_pump_leds() {
  Rig rig;
  rig.discover();
  rig.manager.setPollingEnabled(true);

  rig.sim.setWeight(2, 100 * 1.05f);  // 100 ml: LOW
  rig.run(4000);
  TEST_ASSERT_TRUE(rig.manager.find(2)->levelState == LevelState::Low);
  rig.settle();
  auto led = rig.sim.led(2);
  TEST_ASSERT_EQUAL(2, led.mode);  // slow pulse
  TEST_ASSERT_EQUAL(255, led.r);
  TEST_ASSERT_EQUAL(180, led.g);

  rig.sim.setWeight(2, 40 * 1.05f);  // 40 ml: EMPTY
  rig.run(4000);
  rig.settle();
  TEST_ASSERT_TRUE(rig.manager.find(2)->levelState == LevelState::Empty);
  led = rig.sim.led(2);
  TEST_ASSERT_EQUAL(2, led.mode);
  TEST_ASSERT_EQUAL(0, led.g);

  rig.sim.setWeight(2, 500 * 1.05f);  // refilled: off
  rig.run(4000);
  rig.settle();
  TEST_ASSERT_TRUE(rig.manager.find(2)->levelState == LevelState::Ok);
  TEST_ASSERT_EQUAL(0, rig.sim.led(2).brightness);
  TEST_ASSERT_EQUAL(0, rig.manager.find(2)->led.brightness);
}

void test_sim_dispense_lowers_weight_by_density() {
  Rig rig;
  rig.discover();
  float before = rig.sim.weight(2);
  TEST_ASSERT_TRUE(rig.request(2, "dispense 10").ok());
  rig.run(1200);
  TEST_ASSERT_FALSE(rig.sim.running(2));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 10 * 1.05f, before - rig.sim.weight(2));

  TEST_ASSERT_TRUE(rig.request(2, "purge 500").ok());  // reverse adds liquid back
  rig.run(600);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 5 * 1.05f, before - rig.sim.weight(2));
}

void test_busy_pump_rejects_motion() {
  Rig rig;
  rig.discover();
  TEST_ASSERT_TRUE(rig.request(2, "prime 2000").ok());
  TEST_ASSERT_TRUE(rig.request(2, "dispense 5").status == TxnStatus::Err);
}

void test_lost_motion_reply_is_confirmed_by_status_not_resent() {
  Rig rig;
  rig.discover();
  rig.sim.clearReceived();
  rig.sim.dropNextResponse(2);
  TEST_ASSERT_TRUE(rig.request(2, "dispense 50").ok());
  TEST_ASSERT_EQUAL(1, rig.count("bridge 2 dispense 50"));  // no double pour
  TEST_ASSERT_EQUAL(1, rig.count("bridge 2 status"));
  TEST_ASSERT_TRUE(rig.sim.running(2));
}

void test_unexecuted_motion_is_retried_once() {
  Rig rig;
  rig.discover();
  rig.sim.clearReceived();
  rig.sim.ignoreNextCommand(4);
  TEST_ASSERT_TRUE(rig.request(4, "prime 1000").ok());
  TEST_ASSERT_EQUAL(2, rig.count("bridge 4 prime 1000"));
  TEST_ASSERT_TRUE(rig.sim.running(4));
}

void test_read_only_commands_retry_twice() {
  Rig rig;
  rig.discover();
  rig.sim.clearReceived();
  rig.sim.ignoreNextCommand(2);
  rig.sim.ignoreNextCommand(2);
  TEST_ASSERT_TRUE(rig.request(2, "weight").ok());
  TEST_ASSERT_EQUAL(3, rig.count("bridge 2 weight"));
}

void test_broadcast_stop_halts_every_pump() {
  Rig rig;
  rig.discover();
  TEST_ASSERT_TRUE(rig.request(1, "prime 5000").ok());
  TEST_ASSERT_TRUE(rig.request(2, "prime 5000").ok());
  TEST_ASSERT_TRUE(rig.sim.running(1) && rig.sim.running(2));
  TEST_ASSERT_TRUE(rig.request(kBroadcastId, "stop").ok());  // answered by the bridge itself
  TEST_ASSERT_EQUAL(1, rig.count("bridge 255 stop"));
  TEST_ASSERT_FALSE(rig.sim.running(1));
  TEST_ASSERT_FALSE(rig.sim.running(2));
}

void test_urgent_request_jumps_queue_and_cancel_fails_queued() {
  Rig rig;
  rig.discover();
  std::vector<std::string> order;
  rig.engine.submit(2, "weight", [&](const TxnResult&) { order.push_back("w1"); });
  rig.engine.submit(4, "weight", [&](const TxnResult&) { order.push_back("w2"); });
  rig.engine.submit(kBroadcastId, "stop", [&](const TxnResult&) { order.push_back("stop"); }, true);
  rig.settle();
  TEST_ASSERT_EQUAL(3, order.size());
  TEST_ASSERT_EQUAL_STRING("stop", order[0].c_str());

  int cancelled = 0;
  rig.engine.submit(2, "weight", [&](const TxnResult& r) { cancelled += r.status == TxnStatus::LinkError; });
  rig.engine.submit(4, "weight", [&](const TxnResult& r) { cancelled += r.status == TxnStatus::LinkError; });
  rig.engine.cancelAll(TxnStatus::LinkError);
  TEST_ASSERT_EQUAL(2, cancelled);
  TEST_ASSERT_TRUE(rig.engine.idle());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_sim_pads_to_eight_pumps_with_first_as_bridge);
  RUN_TEST(test_discovery_routes_bridge_direct_and_others_bridged);
  RUN_TEST(test_missing_pump_is_degraded_and_recovers_by_polling);
  RUN_TEST(test_three_failed_polls_mark_pump_missing);
  RUN_TEST(test_old_firmware_raises_pump_fw);
  RUN_TEST(test_unresponsive_bridge_is_blocking);
  RUN_TEST(test_level_states_drive_pump_leds);
  RUN_TEST(test_sim_dispense_lowers_weight_by_density);
  RUN_TEST(test_busy_pump_rejects_motion);
  RUN_TEST(test_lost_motion_reply_is_confirmed_by_status_not_resent);
  RUN_TEST(test_unexecuted_motion_is_retried_once);
  RUN_TEST(test_read_only_commands_retry_twice);
  RUN_TEST(test_broadcast_stop_halts_every_pump);
  RUN_TEST(test_urgent_request_jumps_queue_and_cancel_fails_queued);
  return UNITY_END();
}
