// App logic: dispense (SPEC_v3 §9.5), Prime/Purge All (§9.6.2), E-Stop (§9.7),
// errors (§17) and commands (§14.2), against the simulator.

#include <unity.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "core/App.h"
#include "sim/SimPumpLink.h"

using namespace drinkbot;

namespace {

const char kConfig[] = R"json({
  "schemaVersion": 2,
  "dispense": { "verifyTolerancePct": 15, "verifyToleranceMinMl": 5, "settleMs": 1500,
                "minVolumeMl": 10, "maxVolumeMl": 1000 },
  "defaults": { "volumeMl": 250, "recipeId": "margarita", "primeSec": 5, "purgeSec": 10 },
  "pumps": [
    { "pumpId": 1, "liquid": "Tequila", "msPerMl": 100, "densityGPerMl": 0.95, "color": [255,160,0], "autoPrimeSec": 4 },
    { "pumpId": 2, "liquid": "Triple Sec", "msPerMl": 100, "densityGPerMl": 1.05, "color": [255,120,0], "autoPrimeSec": 0 },
    { "pumpId": 4, "liquid": "Lime Juice", "msPerMl": 100, "densityGPerMl": 1.0, "color": [0,255,0], "autoPrimeSec": 2 }
  ],
  "recipes": [
    { "id": "margarita", "name": "Margarita", "ingredients": [
        { "liquid": "Tequila", "parts": 2, "order": 1 },
        { "liquid": "Triple Sec", "parts": 1, "order": 2 },
        { "liquid": "Lime Juice", "parts": 1, "order": 3 } ] },
    { "id": "paloma", "name": "Paloma", "ingredients": [
        { "liquid": "Tequila", "parts": 1, "order": 1 },
        { "liquid": "Grapefruit Soda", "parts": 3, "order": 2 } ] }
  ]
})json";

class FakeClock : public IClock {
 public:
  uint32_t t = 1000;
  uint32_t nowMs() const override { return t; }
};

class Recorder : public IAppListener {
 public:
  std::vector<DispenseProgress> dispense;
  std::vector<BulkProgress> bulk;
  int stateEvents = 0;
  void onDispenseProgress(const DispenseProgress& p) override { dispense.push_back(p); }
  void onBulkProgress(const BulkProgress& p) override { bulk.push_back(p); }
  void onStateChanged() override { stateEvents++; }
};

Config parsedConfig() {
  JsonDocument doc;
  deserializeJson(doc, kConfig);
  Config c;
  parseConfig(doc.as<JsonVariantConst>(), c);
  return c;
}

struct Rig {
  FakeClock clock;
  SimPumpLink sim{clock, SimPumpLink::specsFromConfig(parsedConfig())};
  App app{sim, clock, &sim};
  Recorder rec;
  std::string saved;
  bool rebooted = false;

  explicit Rig(bool boot = true) {
    app.addListener(&rec);
    app.setHooks({[this](const std::string& json) {
                    saved = json;
                    return true;
                  },
                  [this] { rebooted = true; }});
    if (boot) {
      start();
    }
  }

  void start() {
    app.begin(kConfig, false);
    for (int i = 0; i < 10000 && app.system().booting(); i++) {
      run(1);
    }
    TEST_ASSERT_FALSE_MESSAGE(app.system().booting(), "never finished booting");
  }

  void run(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
      clock.t++;
      app.tick();
    }
  }

  void runUntilIdle(uint32_t maxMs = 120000) {
    for (uint32_t i = 0; i < maxMs && app.system().busy(); i++) {
      run(1);
    }
  }

  // Posts a command and runs until it is answered.
  CommandResult send(Command cmd, uint32_t maxMs = 30000) {
    auto out = std::make_shared<CommandResult>();
    auto done = std::make_shared<bool>(false);
    cmd.reply = [out, done](const CommandResult& r) {
      *out = r;
      *done = true;
    };
    app.post(std::move(cmd));
    for (uint32_t i = 0; i < maxMs && !*done; i++) {
      run(1);
    }
    TEST_ASSERT_TRUE_MESSAGE(*done, "command never answered");
    return *out;
  }

  int count(const std::string& line) const {
    const auto& r = sim.received();
    return static_cast<int>(std::count(r.begin(), r.end(), line));
  }

  int countPrefix(const std::string& prefix) const {
    const auto& r = sim.received();
    return static_cast<int>(
        std::count_if(r.begin(), r.end(), [&](const std::string& l) { return l.rfind(prefix, 0) == 0; }));
  }

  bool hasError(ErrorCode code) const {
    for (const auto& e : app.system().errors()) {
      if (e.code == code) {
        return true;
      }
    }
    return false;
  }
};

Command make(Command::Type type) {
  Command c;
  c.type = type;
  c.source = "test";
  return c;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_boots_ready_with_defaults() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.app.system().state() == SysState::Ready);
  TEST_ASSERT_EQUAL_STRING("margarita", rig.app.activeRecipeId().c_str());
  TEST_ASSERT_EQUAL_FLOAT(250.0f, rig.app.activeVolumeMl());
  TEST_ASSERT_TRUE(rig.rec.stateEvents > 0);
}

void test_dispense_pours_each_ingredient_in_order() {
  Rig rig;
  float t0 = rig.sim.weight(1), s0 = rig.sim.weight(2), l0 = rig.sim.weight(4);
  CommandResult r = rig.send(make(Command::Type::Dispense));
  TEST_ASSERT_TRUE_MESSAGE(r.ok, r.message.c_str());
  TEST_ASSERT_TRUE(rig.app.system().state() == SysState::Dispensing);
  rig.runUntilIdle();
  TEST_ASSERT_TRUE(rig.app.system().state() == SysState::Ready);

  std::vector<DispenseProgress> done;
  for (const auto& p : rig.rec.dispense) {
    if (p.status == DispenseProgress::Status::Done) {
      done.push_back(p);
    }
  }
  TEST_ASSERT_EQUAL(3, done.size());
  TEST_ASSERT_EQUAL_STRING("Tequila", done[0].liquid.c_str());
  TEST_ASSERT_EQUAL_FLOAT(125.0f, done[0].targetMl);
  TEST_ASSERT_EQUAL_STRING("Triple Sec", done[1].liquid.c_str());
  TEST_ASSERT_EQUAL_STRING("Lime Juice", done[2].liquid.c_str());
  for (const auto& d : done) {
    TEST_ASSERT_TRUE(d.hasActual);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, d.targetMl, d.actualMl);
  }
  TEST_ASSERT_TRUE(rig.rec.dispense.back().status == DispenseProgress::Status::Complete);

  // 0.5 g: float rounding over 12500 one-millisecond simulator steps
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 125 * 0.95f, t0 - rig.sim.weight(1));
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 62.5f * 1.05f, s0 - rig.sim.weight(2));
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 62.5f, l0 - rig.sim.weight(4));
  // Pump firmware 0.4.0: dispense takes the run time (ml x msPerMl 100)
  TEST_ASSERT_EQUAL(0, rig.countPrefix("bridge 1 smilli") + rig.countPrefix("bridge 2 smilli"));
  TEST_ASSERT_EQUAL(1, rig.count("bridge 1 dispense 12500 3"));
  TEST_ASSERT_EQUAL(1, rig.count("bridge 2 dispense 6250 3"));
  TEST_ASSERT_EQUAL(0, rig.app.system().errors().size());
}

void test_precheck_insufficient_liquid_pours_nothing() {
  Rig rig;
  rig.sim.setWeight(2, 60 * 1.05f);  // 60 ml; needs 62.5 + 20 ml reserve
  CommandResult r = rig.send(make(Command::Type::Dispense));
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_EQUAL_STRING("PRECHECK_FAILED", r.code.c_str());
  TEST_ASSERT_TRUE(r.message.find("Triple Sec") != std::string::npos);
  TEST_ASSERT_EQUAL(0, rig.countPrefix("bridge 1 dispense") + rig.countPrefix("bridge 2 dispense"));
  TEST_ASSERT_TRUE(rig.app.system().state() == SysState::Ready);
}

void test_precheck_unloaded_liquid_and_bad_volume() {
  Rig rig;
  Command c = make(Command::Type::Dispense);
  c.text = "paloma";
  CommandResult r = rig.send(c);
  TEST_ASSERT_EQUAL_STRING("PRECHECK_FAILED", r.code.c_str());
  TEST_ASSERT_TRUE(r.message.find("Grapefruit Soda") != std::string::npos);

  c.text = "margarita";
  c.number = 5;
  TEST_ASSERT_EQUAL_STRING("INVALID", rig.send(c).code.c_str());
  c.text = "nope";
  c.number = 250;
  TEST_ASSERT_EQUAL_STRING("NOT_FOUND", rig.send(c).code.c_str());
}

void test_estop_mid_pour_stops_and_skips_rest() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.send(make(Command::Type::Dispense)).ok);
  rig.run(1500);  // inside the first pour (125 ml x 20 ms = 2.5 s)
  TEST_ASSERT_TRUE(rig.sim.running(1));
  rig.app.post(make(Command::Type::Estop));
  rig.run(500);
  TEST_ASSERT_FALSE(rig.sim.running(1));
  TEST_ASSERT_EQUAL(1, rig.count("bridge 255 stop"));
  rig.run(10000);
  TEST_ASSERT_EQUAL(0, rig.countPrefix("bridge 2 dispense"));
  TEST_ASSERT_TRUE(rig.rec.dispense.back().status == DispenseProgress::Status::Aborted);
  TEST_ASSERT_EQUAL(0, rig.app.system().errors().size());  // E-Stop is not a fault
  TEST_ASSERT_TRUE(rig.app.system().state() == SysState::Ready);
}

void test_prime_all_runs_auto_prime_times_in_id_order() {
  Rig rig;
  BulkPlan plan = rig.app.bulkPlan(BulkOp::PrimeAll);
  TEST_ASSERT_EQUAL(2, plan.steps.size());  // pump 2 has autoPrimeSec 0
  TEST_ASSERT_EQUAL_UINT32(4000 + 2000 + 500 + 2 * 300, plan.estimatedTotalMs);

  TEST_ASSERT_TRUE(rig.send(make(Command::Type::PrimeAll)).ok);
  TEST_ASSERT_TRUE(rig.app.system().state() == SysState::Manual);
  rig.runUntilIdle();
  TEST_ASSERT_EQUAL(1, rig.count("bridge 1 prime 4000"));
  TEST_ASSERT_EQUAL(1, rig.count("bridge 4 prime 2000"));
  TEST_ASSERT_EQUAL(0, rig.countPrefix("bridge 2 prime"));

  const auto& r = rig.sim.received();
  auto p1 = std::find(r.begin(), r.end(), "bridge 1 prime 4000");
  auto p4 = std::find(r.begin(), r.end(), "bridge 4 prime 2000");
  TEST_ASSERT_TRUE(p1 < p4);
  TEST_ASSERT_TRUE(rig.rec.bulk.back().status == BulkProgress::Status::Complete);
}

void test_purge_all_runs_one_and_a_half_times_prime() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.send(make(Command::Type::PurgeAll)).ok);
  rig.runUntilIdle();
  TEST_ASSERT_EQUAL(1, rig.count("bridge 1 purge 6000"));
  TEST_ASSERT_EQUAL(1, rig.count("bridge 4 purge 3000"));
}

void test_missing_pump_is_skipped_and_degraded() {
  Rig rig(false);
  rig.sim.setMissing(4, true);
  rig.start();
  TEST_ASSERT_TRUE(rig.app.system().state() == SysState::Error);
  TEST_ASSERT_TRUE(rig.app.system().pumpBlocked(4));

  TEST_ASSERT_TRUE(rig.send(make(Command::Type::PrimeAll)).ok);
  rig.runUntilIdle();
  bool skipped = false;
  for (const auto& p : rig.rec.bulk) {
    skipped |= p.status == BulkProgress::Status::Skipped && p.pumpId == 4;
  }
  TEST_ASSERT_TRUE(skipped);
  TEST_ASSERT_EQUAL(1, rig.count("bridge 1 prime 4000"));

  // A recipe that needs the missing pump is blocked; the error is degraded only
  Command d = make(Command::Type::Dispense);
  TEST_ASSERT_EQUAL_STRING("PRECHECK_FAILED", rig.send(d).code.c_str());
}

void test_busy_during_sequence() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.send(make(Command::Type::PrimeAll)).ok);
  CommandResult r = rig.send(make(Command::Type::Dispense));
  TEST_ASSERT_EQUAL_STRING("BUSY", r.code.c_str());
  Command save = make(Command::Type::SaveConfig);
  save.text = kConfig;
  TEST_ASSERT_EQUAL_STRING("BUSY", rig.send(save).code.c_str());
  rig.runUntilIdle();
}

void test_overrun_latches_dispense_timeout() {
  Rig rig;
  rig.sim.setStuck(4, true);
  TEST_ASSERT_TRUE(rig.send(make(Command::Type::PrimeAll)).ok);  // pump 4 primes for 2 s
  rig.runUntilIdle(20000);
  rig.run(200);  // let the queued stop go out
  TEST_ASSERT_TRUE(rig.hasError(ErrorCode::DispenseTimeout));
  TEST_ASSERT_TRUE(rig.count("bridge 4 stop") >= 1);
  TEST_ASSERT_EQUAL_STRING("PRECHECK_FAILED", rig.send(make(Command::Type::Dispense)).code.c_str());

  rig.sim.setStuck(4, false);
  TEST_ASSERT_TRUE(rig.send(make(Command::Type::ClearErrors)).ok);
  TEST_ASSERT_FALSE(rig.hasError(ErrorCode::DispenseTimeout));
  TEST_ASSERT_TRUE(rig.send(make(Command::Type::Dispense)).ok);
  rig.runUntilIdle();
}

void test_estop_never_resends_unanswered_motion() {
  Rig rig;
  Command c = make(Command::Type::Prime);
  c.pumpId = 2;
  rig.app.post(c);
  // Let the LED commands through, then lose the prime itself without running it
  for (int i = 0; i < 2000 && rig.count("bridge 2 ledmode 4 255") == 0; i++) {
    rig.run(1);
  }
  rig.sim.ignoreNextCommand(2);
  for (int i = 0; i < 2000 && rig.count("bridge 2 prime 120000") == 0; i++) {
    rig.run(1);
  }
  TEST_ASSERT_EQUAL(1, rig.count("bridge 2 prime 120000"));
  rig.run(1000);  // engine is still waiting for the bridge's R,TIMEOUT
  rig.app.post(make(Command::Type::Estop));
  rig.run(10000);
  TEST_ASSERT_EQUAL(1, rig.count("bridge 2 prime 120000"));
  TEST_ASSERT_FALSE(rig.sim.running(2));
}

void test_save_config_validates_and_applies() {
  Rig rig;
  JsonDocument doc;
  deserializeJson(doc, kConfig);
  doc["pumps"][0]["autoPrimeSec"] = 99;
  std::string bad;
  serializeJson(doc, bad);
  Command save = make(Command::Type::SaveConfig);
  save.text = bad;
  CommandResult r = rig.send(save);
  TEST_ASSERT_EQUAL_STRING("CONFIG_INVALID", r.code.c_str());
  TEST_ASSERT_EQUAL(1, r.configErrors.size());
  TEST_ASSERT_EQUAL_STRING("pumps[0].autoPrimeSec", r.configErrors[0].path.c_str());
  TEST_ASSERT_TRUE(rig.saved.empty());

  doc["pumps"][0]["autoPrimeSec"] = 1.5;
  std::string good;
  serializeJson(doc, good);
  save.text = good;
  TEST_ASSERT_TRUE(rig.send(save).ok);
  TEST_ASSERT_FALSE(rig.saved.empty());
  TEST_ASSERT_EQUAL_FLOAT(1.5f, rig.app.config().pumps[0].autoPrimeSec);
}

void test_selection_commands() {
  Rig rig;
  Command v = make(Command::Type::SetVolume);
  v.number = 350;
  TEST_ASSERT_TRUE(rig.send(v).ok);
  TEST_ASSERT_EQUAL_FLOAT(350.0f, rig.app.activeVolumeMl());
  v.number = 2000;
  TEST_ASSERT_EQUAL_STRING("INVALID", rig.send(v).code.c_str());

  Command s = make(Command::Type::SetRecipe);
  s.text = "paloma";
  TEST_ASSERT_TRUE(rig.send(s).ok);
  TEST_ASSERT_EQUAL_STRING("paloma", rig.app.activeRecipeId().c_str());
  TEST_ASSERT_TRUE(rig.app.recipeUnavailableReason(*rig.app.config().findRecipe("paloma")).find("Grapefruit") !=
                   std::string::npos);
  s.text = "nope";
  TEST_ASSERT_EQUAL_STRING("NOT_FOUND", rig.send(s).code.c_str());
}

void test_maintenance_raw_and_set_id() {
  Rig rig;
  Command raw = make(Command::Type::PumpRaw);
  raw.pumpId = 4;
  raw.text = "gid";
  CommandResult r = rig.send(raw);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_STRING("P,ID,4", r.lines[0].c_str());

  // Motion through the raw console would bypass the one-pump-at-a-time rule
  raw.text = "prime 1000";
  TEST_ASSERT_EQUAL_STRING("INVALID", rig.send(raw).code.c_str());
  TEST_ASSERT_EQUAL(0, rig.countPrefix("bridge 4 prime"));

  Command board = make(Command::Type::PumpSetId);  // the bridge board keeps ID 0
  board.pumpId = kBridgeBoardId;
  board.newId = 9;
  TEST_ASSERT_EQUAL_STRING("INVALID", rig.send(board).code.c_str());

  Command id = make(Command::Type::PumpSetId);
  id.pumpId = 4;
  id.newId = 9;
  TEST_ASSERT_TRUE(rig.send(id).ok);
  TEST_ASSERT_EQUAL(1, rig.count("bridge 4 sid 9"));
  TEST_ASSERT_EQUAL(1, rig.count("bridge 9 scfg"));  // saved at the new address
  TEST_ASSERT_NOT_NULL(rig.app.config().findPumpById(9));
  rig.run(5000);
  TEST_ASSERT_TRUE(rig.app.pumps().find(9)->present);
}

// tare / cal block a board for ~1.8 s; the direct request used
// to time out after 500 ms
void test_tare_and_cal_wait_for_slow_scale() {
  Rig rig;
  Command tare = make(Command::Type::PumpTare);
  tare.pumpId = kBridgeBoardId;  // the board on USB: direct
  CommandResult r = rig.send(tare);
  TEST_ASSERT_TRUE_MESSAGE(r.ok, r.message.c_str());
  tare.pumpId = 2;  // bridged
  r = rig.send(tare);
  TEST_ASSERT_TRUE_MESSAGE(r.ok, r.message.c_str());
  Command cal = make(Command::Type::PumpCalibrate);
  cal.pumpId = kBridgeBoardId;
  cal.number = 100;
  r = rig.send(cal);
  TEST_ASSERT_TRUE_MESSAGE(r.ok, r.message.c_str());
}

void test_reboot_requires_force_while_busy() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.send(make(Command::Type::PrimeAll)).ok);
  Command reboot = make(Command::Type::Reboot);
  TEST_ASSERT_EQUAL_STRING("BUSY", rig.send(reboot).code.c_str());
  reboot.flag = true;
  TEST_ASSERT_TRUE(rig.send(reboot).ok);
  rig.run(600);
  TEST_ASSERT_TRUE(rig.rebooted);
  TEST_ASSERT_FALSE(rig.app.system().busy());
}

Command manual(Command::Type type, int pumpId) {
  Command c = make(type);
  c.pumpId = pumpId;
  return c;
}

void test_manual_prime_runs_until_stopped() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.send(manual(Command::Type::Prime, 4)).ok);
  rig.run(1000);
  TEST_ASSERT_EQUAL(1, rig.count("bridge 4 prime 120000"));  // the pump's longest allowed run
  TEST_ASSERT_TRUE(rig.sim.running(4));
  rig.run(30000);
  TEST_ASSERT_TRUE(rig.sim.running(4));
  TEST_ASSERT_TRUE(rig.app.system().busy());

  TEST_ASSERT_TRUE(rig.send(manual(Command::Type::PumpStop, 4)).ok);
  rig.run(500);
  TEST_ASSERT_FALSE(rig.sim.running(4));
  TEST_ASSERT_FALSE(rig.app.system().busy());
  TEST_ASSERT_TRUE(rig.rec.bulk.back().status == BulkProgress::Status::Complete);
  TEST_ASSERT_EQUAL(0, static_cast<int>(rig.app.system().errors().size()));
}

void test_manual_prime_ends_at_max_run_time() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.send(manual(Command::Type::Purge, 2)).ok);
  rig.run(1000);
  TEST_ASSERT_TRUE(rig.sim.reversing(2));
  rig.runUntilIdle(130000);
  TEST_ASSERT_FALSE(rig.app.system().busy());
  TEST_ASSERT_FALSE(rig.sim.running(2));
  TEST_ASSERT_TRUE(rig.rec.bulk.back().status == BulkProgress::Status::Complete);
  TEST_ASSERT_EQUAL(0, static_cast<int>(rig.app.system().errors().size()));
}

void test_manual_switch_stops_previous_pump_first() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.send(manual(Command::Type::Prime, 1)).ok);
  rig.run(1000);
  TEST_ASSERT_TRUE(rig.sim.running(1));

  rig.app.post(manual(Command::Type::Purge, 4));
  bool overlap = false;
  for (int i = 0; i < 3000; i++) {
    rig.run(1);
    overlap |= rig.sim.running(1) && rig.sim.running(4);
  }
  TEST_ASSERT_FALSE_MESSAGE(overlap, "two pumps ran at once");
  TEST_ASSERT_FALSE(rig.sim.running(1));
  TEST_ASSERT_TRUE(rig.sim.reversing(4));

  const auto& r = rig.sim.received();
  auto stop1 = std::find(r.begin(), r.end(), "bridge 1 stop");
  auto purge4 = std::find(r.begin(), r.end(), "bridge 4 purge 120000");
  TEST_ASSERT_TRUE(stop1 != r.end() && purge4 != r.end());
  TEST_ASSERT_TRUE(stop1 < purge4);

  // Same pump, other direction: also stop then start
  rig.app.post(manual(Command::Type::Prime, 4));
  rig.run(3000);
  TEST_ASSERT_TRUE(rig.sim.running(4));
  TEST_ASSERT_FALSE(rig.sim.reversing(4));
  TEST_ASSERT_TRUE(rig.count("bridge 4 stop") >= 1);

  TEST_ASSERT_TRUE(rig.send(manual(Command::Type::PumpStop, 4)).ok);
  rig.run(500);
  TEST_ASSERT_FALSE(rig.sim.running(4));
  TEST_ASSERT_FALSE(rig.app.system().busy());
  TEST_ASSERT_EQUAL(0, static_cast<int>(rig.app.system().errors().size()));
}

void test_manual_prime_busy_during_prime_all() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.send(make(Command::Type::PrimeAll)).ok);
  rig.run(500);
  TEST_ASSERT_EQUAL_STRING("BUSY", rig.send(manual(Command::Type::Prime, 2)).code.c_str());
  TEST_ASSERT_EQUAL_STRING("BUSY", rig.send(manual(Command::Type::PumpStop, 1)).code.c_str());
  rig.runUntilIdle();
  TEST_ASSERT_EQUAL(0, rig.countPrefix("bridge 2 prime"));
}

void test_dispense_rejects_run_over_pump_limit() {
  Rig rig;
  // Paloma at 1000 ml: 750 ml of Grapefruit Soda is not loaded, so use Margarita at
  // a slow flow rate: 500 ml Tequila x 300 ms/ml = 150 s, over the 120 s limit
  JsonDocument doc;
  deserializeJson(doc, kConfig);
  doc["pumps"][0]["msPerMl"] = 300;
  doc["dispense"]["maxVolumeMl"] = 1000;
  std::string json;
  serializeJson(doc, json);
  Command save = make(Command::Type::SaveConfig);
  save.text = json;
  TEST_ASSERT_TRUE(rig.send(save).ok);
  Command d = make(Command::Type::Dispense);
  d.text = "margarita";
  d.number = 1000;
  CommandResult r = rig.send(d);
  TEST_ASSERT_EQUAL_STRING("PRECHECK_FAILED", r.code.c_str());
  TEST_ASSERT_TRUE(r.message.find("120 s") != std::string::npos);
  TEST_ASSERT_EQUAL(0, rig.countPrefix("bridge 1 dispense"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_boots_ready_with_defaults);
  RUN_TEST(test_dispense_pours_each_ingredient_in_order);
  RUN_TEST(test_precheck_insufficient_liquid_pours_nothing);
  RUN_TEST(test_precheck_unloaded_liquid_and_bad_volume);
  RUN_TEST(test_estop_mid_pour_stops_and_skips_rest);
  RUN_TEST(test_prime_all_runs_auto_prime_times_in_id_order);
  RUN_TEST(test_purge_all_runs_one_and_a_half_times_prime);
  RUN_TEST(test_missing_pump_is_skipped_and_degraded);
  RUN_TEST(test_busy_during_sequence);
  RUN_TEST(test_overrun_latches_dispense_timeout);
  RUN_TEST(test_estop_never_resends_unanswered_motion);
  RUN_TEST(test_manual_prime_runs_until_stopped);
  RUN_TEST(test_manual_prime_ends_at_max_run_time);
  RUN_TEST(test_manual_switch_stops_previous_pump_first);
  RUN_TEST(test_manual_prime_busy_during_prime_all);
  RUN_TEST(test_dispense_rejects_run_over_pump_limit);
  RUN_TEST(test_save_config_validates_and_applies);
  RUN_TEST(test_selection_commands);
  RUN_TEST(test_maintenance_raw_and_set_id);
  RUN_TEST(test_tare_and_cal_wait_for_slow_scale);
  RUN_TEST(test_reboot_requires_force_while_busy);
  return UNITY_END();
}
