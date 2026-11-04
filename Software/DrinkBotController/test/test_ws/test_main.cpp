// WebSocket message format (SPEC_v3 §14) as the web app in web/app.js reads it.

#include <unity.h>

#include <string>

#include "core/App.h"
#include "core/UiController.h"
#include "core/UiModel.h"
#include "core/WsProtocol.h"
#include "sim/SimPumpLink.h"

using namespace drinkbot;

namespace {

const char kConfig[] = R"json({
  "schemaVersion": 2,
  "futureKey": 7,
  "defaults": { "volumeMl": 250, "recipeId": "shot", "primeSec": 5, "purgeSec": 10 },
  "pumps": [
    { "pumpId": 1, "liquid": "Tequila", "msPerMl": 20, "densityGPerMl": 1.0, "color": [255,160,0], "autoPrimeSec": 1 },
    { "pumpId": 3, "liquid": "Lime", "msPerMl": 20, "densityGPerMl": 1.0, "color": [0,255,0], "autoPrimeSec": 1 }
  ],
  "recipes": [ { "id": "shot", "name": "Shot", "ingredients": [ { "liquid": "Tequila", "parts": 1, "order": 1 } ] } ]
})json";

class FakeClock : public IClock {
 public:
  uint32_t t = 1000;
  uint32_t nowMs() const override { return t; }
};

Config parsed() {
  JsonDocument doc;
  deserializeJson(doc, kConfig);
  Config c;
  parseConfig(doc.as<JsonVariantConst>(), c);
  return c;
}

struct Rig {
  FakeClock clock;
  SimPumpLink sim{clock, SimPumpLink::specsFromConfig(parsed())};
  App app{sim, clock, &sim};
  UiModel model{clock};
  UiController ui{app, model};

  explicit Rig(bool missing3 = false) {
    sim.setMissing(3, missing3);
    app.begin(kConfig, false);
    // A missing pump costs three bridged timeouts before boot completes
    for (int i = 0; i < 20000 && (app.system().booting() || i < 3000); i++) {
      clock.t++;
      app.tick();
    }
    TEST_ASSERT_FALSE_MESSAGE(app.system().booting(), "never finished booting");
  }
};

JsonDocument parse(const std::string& s) {
  JsonDocument doc;
  TEST_ASSERT_TRUE_MESSAGE(deserializeJson(doc, s) == DeserializationError::Ok, s.c_str());
  return doc;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_hello() {
  auto d = parse(helloJson(true, "drinkbot", "Nov  3 2026"));
  TEST_ASSERT_EQUAL_STRING("hello", d["type"]);
  TEST_ASSERT_TRUE(d["simulator"].as<bool>());
  TEST_ASSERT_EQUAL_STRING("drinkbot", d["hostname"]);
  TEST_ASSERT_EQUAL_STRING(kFirmwareVersion, d["fwVersion"]);
}

void test_state_message() {
  Rig rig(true);
  WifiInfo wifi{WifiMode::Station, "HomeWiFi", "192.168.1.40", -52};
  auto d = parse(stateJson(rig.app, rig.ui.leds(), wifi));
  TEST_ASSERT_EQUAL_STRING("ERROR", d["system"]);  // pump 3 missing
  TEST_ASSERT_EQUAL_STRING("E_PUMP_MISSING", d["errors"][0]["code"]);
  TEST_ASSERT_EQUAL(3, d["errors"][0]["pumpId"].as<int>());
  TEST_ASSERT_FALSE(d["errors"][0]["latched"].as<bool>());
  TEST_ASSERT_EQUAL_STRING("shot", d["activeRecipeId"]);
  TEST_ASSERT_EQUAL_FLOAT(250.0f, d["activeVolumeMl"].as<float>());
  TEST_ASSERT_EQUAL_STRING("STA", d["wifi"]["mode"]);
  TEST_ASSERT_EQUAL(-52, d["wifi"]["rssi"].as<int>());
  TEST_ASSERT_EQUAL(9, d["leds"].size());
  TEST_ASSERT_EQUAL_STRING("blink", d["leds"][7]["mode"]);  // status blinks red
  TEST_ASSERT_EQUAL(255, d["leds"][7]["r"].as<int>());
}

void test_pumps_message() {
  Rig rig(true);
  auto d = parse(pumpsJson(rig.app));
  JsonArray pumps = d["pumps"];
  TEST_ASSERT_EQUAL(2, pumps.size());
  TEST_ASSERT_EQUAL(1, pumps[0]["pumpId"].as<int>());
  TEST_ASSERT_TRUE(pumps[0]["present"].as<bool>());
  TEST_ASSERT_TRUE(pumps[0]["isBridge"].as<bool>());
  TEST_ASSERT_EQUAL_STRING("0.4.0-sim", pumps[0]["fwVersion"]);
  TEST_ASSERT_EQUAL_FLOAT(750.0f, pumps[0]["levelMl"].as<float>());
  TEST_ASSERT_EQUAL_STRING("OK", pumps[0]["levelState"]);
  TEST_ASSERT_EQUAL_STRING("IDLE", pumps[0]["activity"]);
  TEST_ASSERT_EQUAL(0, pumps[0]["led"]["brightness"].as<int>());
  TEST_ASSERT_FALSE(pumps[1]["present"].as<bool>());
  TEST_ASSERT_TRUE(pumps[1]["weightG"].isNull());  // never read
}

void test_config_message_keeps_unknown_keys() {
  Rig rig;
  auto d = parse(configMessageJson(rig.app.configJson()));
  TEST_ASSERT_EQUAL_STRING("config", d["type"]);
  TEST_ASSERT_EQUAL(7, d["config"]["futureKey"].as<int>());
  TEST_ASSERT_EQUAL(2, d["config"]["pumps"].size());
}

void test_progress_messages() {
  DispenseProgress p;
  p.recipeId = "shot";
  p.volumeMl = 250;
  p.step = 2;
  p.steps = 3;
  p.liquid = "Lime";
  p.pumpId = 3;
  p.targetMl = 62.4999f;
  p.hasActual = true;
  p.actualMl = 61.96f;
  p.status = DispenseProgress::Status::Done;
  auto d = parse(dispenseProgressJson(p));
  TEST_ASSERT_EQUAL_STRING("DONE", d["status"]);
  TEST_ASSERT_EQUAL_FLOAT(62.5f, d["targetMl"].as<float>());
  TEST_ASSERT_EQUAL_FLOAT(62.0f, d["actualMl"].as<float>());

  BulkProgress b;
  b.op = BulkOp::PurgeAll;
  b.steps = 2;
  b.estimatedTotalMs = 9600;
  b.status = BulkProgress::Status::Starting;
  auto bd = parse(bulkProgressJson(b));
  TEST_ASSERT_EQUAL_STRING("purgeAll", bd["op"]);
  TEST_ASSERT_EQUAL(0, bd["step"].as<int>());
  TEST_ASSERT_FALSE(bd["pumpId"].is<int>());
  TEST_ASSERT_EQUAL(9600, bd["estimatedTotalMs"].as<int>());
}

void test_ack_and_error() {
  CommandResult r;
  r.lines = {"P,MILLI,20", "R,OK"};
  auto a = parse(ackJson("r7", r));
  TEST_ASSERT_EQUAL_STRING("r7", a["reqId"]);
  TEST_ASSERT_EQUAL_STRING("P,MILLI,20", a["data"]["lines"][0]);
  TEST_ASSERT_TRUE(parse(ackJson("r8", CommandResult()))["data"].isNull());

  auto e = parse(errorJson("r9", "CONFIG_INVALID", "1 problem", {{"pumps[0].msPerMl", "must be between 1 and 65535"}}));
  TEST_ASSERT_EQUAL_STRING("CONFIG_INVALID", e["code"]);
  TEST_ASSERT_EQUAL_STRING("pumps[0].msPerMl", e["details"][0]["path"]);

  CommandResult bad;
  bad.ok = false;
  bad.code = "CONFIG_INVALID";
  bad.configErrors = {{"recipes[0].id", "is already used"}};
  auto u = parse(configUploadReplyJson(bad));
  TEST_ASSERT_FALSE(u["ok"].as<bool>());
  TEST_ASSERT_EQUAL_STRING("recipes[0].id", u["errors"][0]["path"]);
}

void test_parse_client_commands() {
  auto m = parseClientMessage(R"({"type":"dispense","reqId":"r1","recipeId":"shot","volumeMl":150})");
  TEST_ASSERT_TRUE(m.kind == ClientMessage::Kind::Command);
  TEST_ASSERT_EQUAL_STRING("r1", m.reqId.c_str());
  TEST_ASSERT_TRUE(m.command.type == Command::Type::Dispense);
  TEST_ASSERT_EQUAL_STRING("shot", m.command.text.c_str());
  TEST_ASSERT_EQUAL_FLOAT(150.0f, m.command.number);

  m = parseClientMessage(R"({"type":"dispense"})");
  TEST_ASSERT_TRUE(std::isnan(m.command.number));  // use the active volume

  m = parseClientMessage(R"({"type":"purge","pumpId":4})");
  TEST_ASSERT_TRUE(m.command.type == Command::Type::Purge);
  TEST_ASSERT_EQUAL(4, m.command.pumpId);

  m = parseClientMessage(R"({"type":"pumpStop","pumpId":4})");
  TEST_ASSERT_TRUE(m.command.type == Command::Type::PumpStop);
  TEST_ASSERT_EQUAL(4, m.command.pumpId);

  m = parseClientMessage(R"({"type":"reboot","force":true})");
  TEST_ASSERT_TRUE(m.command.flag);

  m = parseClientMessage(R"({"type":"saveConfig","config":{"schemaVersion":2}})");
  TEST_ASSERT_TRUE(m.command.type == Command::Type::SaveConfig);
  TEST_ASSERT_EQUAL_STRING("{\"schemaVersion\":2}", m.command.text.c_str());

  m = parseClientMessage(R"({"type":"pumpSetId","pumpId":4,"newId":9})");
  TEST_ASSERT_EQUAL(9, m.command.newId);

  m = parseClientMessage(R"({"type":"pumpRaw","pumpId":2,"command":"gmilli"})");
  TEST_ASSERT_EQUAL_STRING("gmilli", m.command.text.c_str());
}

void test_parse_sim_faults_as_the_ui_sends_them() {
  auto m = parseClientMessage(R"({"type":"simFault","pumpId":3,"fault":"missing","value":true})");
  TEST_ASSERT_EQUAL_STRING("missing", m.command.text.c_str());
  m = parseClientMessage(R"({"type":"simFault","pumpId":3,"fault":"missing","value":false})");
  TEST_ASSERT_EQUAL_STRING("present", m.command.text.c_str());
  m = parseClientMessage(R"({"type":"simFault","pumpId":3,"fault":"overrun"})");
  TEST_ASSERT_EQUAL_STRING("overrun", m.command.text.c_str());
  m = parseClientMessage(R"({"type":"simFault","pumpId":3,"fault":"weight","weightG":120})");
  TEST_ASSERT_EQUAL_FLOAT(120.0f, m.command.number);
}

void test_parse_other_kinds_and_errors() {
  TEST_ASSERT_TRUE(parseClientMessage(R"({"type":"getConfig"})").kind == ClientMessage::Kind::GetConfig);
  auto lvl = parseClientMessage(R"({"type":"setLogLevel","level":"DEBUG"})");
  TEST_ASSERT_TRUE(lvl.kind == ClientMessage::Kind::SetLogLevel);
  TEST_ASSERT_TRUE(lvl.logLevel == LogLevel::Debug);
  TEST_ASSERT_TRUE(parseClientMessage(R"({"type":"setLogLevel","level":"LOUD"})").kind ==
                   ClientMessage::Kind::Invalid);
  TEST_ASSERT_TRUE(parseClientMessage("not json").kind == ClientMessage::Kind::Invalid);
  auto unk = parseClientMessage(R"({"type":"launch","reqId":"r3"})");
  TEST_ASSERT_TRUE(unk.kind == ClientMessage::Kind::Invalid);
  TEST_ASSERT_EQUAL_STRING("r3", unk.reqId.c_str());
  TEST_ASSERT_TRUE(parseClientMessage(R"({"type":"saveConfig","config":5})").kind == ClientMessage::Kind::Invalid);
}

void test_aborted_progress_names_running_step() {
  Rig rig;
  struct Rec : IAppListener {
    std::vector<DispenseProgress> d;
    void onDispenseProgress(const DispenseProgress& p) override { d.push_back(p); }
  } rec;
  rig.app.addListener(&rec);
  Command c;
  c.type = Command::Type::Dispense;
  rig.app.post(c);
  for (int i = 0; i < 1500; i++) {
    rig.clock.t++;
    rig.app.tick();
  }
  Command stop;
  stop.type = Command::Type::Estop;
  rig.app.post(stop);
  for (int i = 0; i < 200; i++) {
    rig.clock.t++;
    rig.app.tick();
  }
  TEST_ASSERT_TRUE(rec.d.back().status == DispenseProgress::Status::Aborted);
  TEST_ASSERT_EQUAL(1, rec.d.back().step);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_hello);
  RUN_TEST(test_state_message);
  RUN_TEST(test_pumps_message);
  RUN_TEST(test_config_message_keeps_unknown_keys);
  RUN_TEST(test_progress_messages);
  RUN_TEST(test_ack_and_error);
  RUN_TEST(test_parse_client_commands);
  RUN_TEST(test_parse_sim_faults_as_the_ui_sends_them);
  RUN_TEST(test_parse_other_kinds_and_errors);
  RUN_TEST(test_aborted_progress_names_running_step);
  return UNITY_END();
}
