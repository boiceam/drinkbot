// Config parsing, validation (SPEC_v3 §7.3), migration (§7.6), recipe math (§9.5) and logging (§12)

#include <unity.h>

#include <string>
#include <vector>

#include "core/Config.h"
#include "core/Log.h"
#include "core/RecipeMath.h"

using namespace drinkbot;

namespace {

// The example from SPEC_v3 §7.2
const char kExampleConfig[] = R"json({
  "schemaVersion": 2,
  "device": { "hostname": "drinkbot", "apSsid": "DrinkBot" },
  "wifi": { "networks": [ { "ssid": "HomeWiFi", "password": "secretpass" } ] },
  "levels": { "lowMl": 150, "emptyMl": 50, "actualEmptyMl": 20 },
  "dispense": { "verifyTolerancePct": 15, "verifyToleranceMinMl": 5, "settleMs": 1500,
                "minVolumeMl": 10, "maxVolumeMl": 1000 },
  "defaults": { "volumeMl": 250, "recipeId": "margarita", "primeSec": 5, "purgeSec": 10 },
  "ui": { "ledBrightness": 64 },
  "pumps": [
    { "pumpId": 1, "liquid": "Tequila", "msPerMl": 95, "densityGPerMl": 0.95,
      "color": [255, 160, 0], "autoPrimeSec": 4.0 },
    { "pumpId": 2, "liquid": "Triple Sec", "msPerMl": 100, "densityGPerMl": 1.05,
      "color": [255, 120, 0], "autoPrimeSec": 0 },
    { "pumpId": 4, "liquid": "Lime Juice", "msPerMl": 110, "densityGPerMl": 1.03,
      "color": [0, 255, 0], "autoPrimeSec": 2.5 }
  ],
  "recipes": [
    { "id": "margarita", "name": "Margarita", "ingredients": [
        { "liquid": "Tequila", "parts": 2, "order": 1 },
        { "liquid": "lime juice", "parts": 1, "order": 3 },
        { "liquid": "Triple Sec", "parts": 1, "order": 2 } ] },
    { "id": "paloma", "name": "Paloma", "ingredients": [
        { "liquid": "Tequila", "parts": 1, "order": 1 },
        { "liquid": "Grapefruit Soda", "parts": 3, "order": 2 } ] }
  ],
  "buttons": [
    { "action": "setRecipe", "recipeId": "margarita" },
    { "action": "setRecipe", "recipeId": "paloma" },
    { "action": "setVolume", "volumeMl": 150 },
    { "action": "setVolume", "volumeMl": 250 },
    { "action": "setVolume", "volumeMl": 350 },
    { "action": "dispense" },
    { "action": "estop" }
  ]
})json";

void loadExample(JsonDocument& doc) {
  TEST_ASSERT_TRUE(deserializeJson(doc, kExampleConfig) == DeserializationError::Ok);
}

bool hasErrorAt(const std::vector<ConfigError>& errors, const std::string& path) {
  for (const auto& e : errors) {
    if (e.path == path) {
      return true;
    }
  }
  return false;
}

// Applies `mutate` to the example and expects an error at `path`.
template <typename F>
void expectError(const char* path, F mutate) {
  JsonDocument doc;
  loadExample(doc);
  mutate(doc);
  Config cfg;
  auto errors = parseConfig(doc.as<JsonVariantConst>(), cfg);
  if (!hasErrorAt(errors, path)) {
    std::string msg = std::string("expected error at ") + path + ", got:";
    for (const auto& e : errors) {
      msg += " [" + e.path + ": " + e.message + "]";
    }
    TEST_FAIL_MESSAGE(msg.c_str());
  }
}

class CaptureSink : public ILogSink {
 public:
  std::vector<LogEntry> entries;
  void write(const LogEntry& e) override { entries.push_back(e); }
};

class FakeClock : public IClock {
 public:
  uint32_t t = 0;
  uint32_t nowMs() const override { return t; }
};

}  // namespace

void setUp() {}
void tearDown() {}

void test_example_config_is_valid() {
  JsonDocument doc;
  loadExample(doc);
  Config cfg;
  auto errors = parseConfig(doc.as<JsonVariantConst>(), cfg);
  TEST_ASSERT_EQUAL(0, errors.size());
  TEST_ASSERT_EQUAL(3, cfg.pumps.size());
  TEST_ASSERT_EQUAL_FLOAT(4.0f, cfg.pumps[0].autoPrimeSec);
  TEST_ASSERT_EQUAL(160, cfg.pumps[0].color.g);
  TEST_ASSERT_TRUE(cfg.buttons[6].action == ButtonAction::Estop);
  TEST_ASSERT_EQUAL_FLOAT(350.0f, cfg.buttons[4].volumeMl);
  TEST_ASSERT_NOT_NULL(cfg.findPumpByLiquid("TEQUILA"));
}

void test_factory_default_is_valid() {
  JsonDocument doc;
  makeFactoryDefaultConfig(doc);
  Config cfg;
  auto errors = parseConfig(doc.as<JsonVariantConst>(), cfg);
  TEST_ASSERT_EQUAL(0, errors.size());
  TEST_ASSERT_EQUAL(0, cfg.pumps.size());
  TEST_ASSERT_TRUE(cfg.buttons[0].action == ButtonAction::None);
  TEST_ASSERT_EQUAL_STRING("DrinkBot", cfg.apSsid.c_str());
}

void test_validation_rules() {
  expectError("pumps[1].pumpId", [](JsonDocument& d) { d["pumps"][1]["pumpId"] = 1; });
  expectError("pumps[0].pumpId", [](JsonDocument& d) { d["pumps"][0]["pumpId"] = 255; });
  expectError("pumps[1].liquid", [](JsonDocument& d) { d["pumps"][1]["liquid"] = "tequila"; });
  expectError("pumps[0].msPerMl", [](JsonDocument& d) { d["pumps"][0]["msPerMl"] = 0; });
  expectError("pumps[0].msPerMl", [](JsonDocument& d) { d["pumps"][0]["msPerMl"] = 9.5; });
  expectError("pumps[0].densityGPerMl", [](JsonDocument& d) { d["pumps"][0]["densityGPerMl"] = 2.5; });
  expectError("pumps[0].color[1]", [](JsonDocument& d) { d["pumps"][0]["color"][1] = 300; });
  expectError("pumps[0].color", [](JsonDocument& d) { d["pumps"][0]["color"].as<JsonArray>().remove(2); });
  expectError("pumps[0].autoPrimeSec", [](JsonDocument& d) { d["pumps"][0]["autoPrimeSec"] = 61; });
  expectError("pumps[0].autoPrimeSec", [](JsonDocument& d) { d["pumps"][0]["autoPrimeSec"] = 1.25; });
  expectError("levels", [](JsonDocument& d) { d["levels"]["emptyMl"] = 200; });
  expectError("recipes[1].id", [](JsonDocument& d) { d["recipes"][1]["id"] = "margarita"; });
  expectError("recipes[0].id", [](JsonDocument& d) { d["recipes"][0]["id"] = "Bad Id"; });
  expectError("recipes[0].ingredients", [](JsonDocument& d) { d["recipes"][0]["ingredients"].to<JsonArray>(); });
  expectError("recipes[0].ingredients[0].parts", [](JsonDocument& d) { d["recipes"][0]["ingredients"][0]["parts"] = 0; });
  expectError("recipes[0].ingredients[0].order", [](JsonDocument& d) { d["recipes"][0]["ingredients"][0]["order"] = -1; });
  expectError("recipes[0].name", [](JsonDocument& d) { d["recipes"][0]["name"] = "   "; });
  expectError("buttons", [](JsonDocument& d) { d["buttons"].as<JsonArray>().remove(0); });
  expectError("buttons[0].recipeId", [](JsonDocument& d) { d["buttons"][0]["recipeId"] = "nope"; });
  expectError("buttons[2].volumeMl", [](JsonDocument& d) { d["buttons"][2]["volumeMl"] = 5; });
  expectError("buttons[5].action", [](JsonDocument& d) { d["buttons"][5]["action"] = "explode"; });
  expectError("defaults.recipeId", [](JsonDocument& d) { d["defaults"]["recipeId"] = "nope"; });
  expectError("defaults.primeSec", [](JsonDocument& d) { d["defaults"]["primeSec"] = 0; });
  expectError("wifi.networks[0].password", [](JsonDocument& d) { d["wifi"]["networks"][0]["password"] = "short"; });
  expectError("wifi.networks[0].ssid", [](JsonDocument& d) { d["wifi"]["networks"][0]["ssid"] = ""; });
  expectError("device.hostname", [](JsonDocument& d) { d["device"]["hostname"] = "Drink Bot"; });
  expectError("dispense", [](JsonDocument& d) { d["dispense"]["minVolumeMl"] = 1000; });
  expectError("schemaVersion", [](JsonDocument& d) { d["schemaVersion"] = 1; });
}

void test_open_network_password_allowed() {
  JsonDocument doc;
  loadExample(doc);
  doc["wifi"]["networks"][0]["password"] = "";
  Config cfg;
  TEST_ASSERT_EQUAL(0, parseConfig(doc.as<JsonVariantConst>(), cfg).size());
}

void test_unknown_keys_are_kept_in_document() {
  JsonDocument doc;
  loadExample(doc);
  doc["futureFeature"]["enabled"] = true;
  Config cfg;
  TEST_ASSERT_EQUAL(0, parseConfig(doc.as<JsonVariantConst>(), cfg).size());
  TEST_ASSERT_TRUE(doc["futureFeature"]["enabled"].as<bool>());
}

void test_migrate_v1_adds_auto_prime() {
  JsonDocument doc;
  loadExample(doc);
  doc["schemaVersion"] = 1;
  doc["defaults"]["primeSec"] = 7;
  for (JsonObject p : doc["pumps"].as<JsonArray>()) {
    p.remove("autoPrimeSec");
  }
  TEST_ASSERT_TRUE(migrateConfig(doc));
  TEST_ASSERT_EQUAL(2, doc["schemaVersion"].as<int>());
  Config cfg;
  TEST_ASSERT_EQUAL(0, parseConfig(doc.as<JsonVariantConst>(), cfg).size());
  for (const auto& p : cfg.pumps) {
    TEST_ASSERT_EQUAL_FLOAT(7.0f, p.autoPrimeSec);
  }
  TEST_ASSERT_FALSE(migrateConfig(doc));  // already current
}

void test_pours_split_by_parts_in_order() {
  JsonDocument doc;
  loadExample(doc);
  Config cfg;
  parseConfig(doc.as<JsonVariantConst>(), cfg);
  auto steps = computePours(cfg, *cfg.findRecipe("margarita"), 250);
  TEST_ASSERT_EQUAL(3, steps.size());
  TEST_ASSERT_EQUAL_STRING("Tequila", steps[0].liquid.c_str());
  TEST_ASSERT_EQUAL_FLOAT(125.0f, steps[0].ml);
  TEST_ASSERT_EQUAL_STRING("Triple Sec", steps[1].liquid.c_str());
  TEST_ASSERT_EQUAL_FLOAT(62.5f, steps[1].ml);
  TEST_ASSERT_EQUAL_STRING("lime juice", steps[2].liquid.c_str());
  TEST_ASSERT_EQUAL_FLOAT(62.5f, steps[2].ml);
  TEST_ASSERT_EQUAL(4, steps[2].pumpId);  // case-insensitive liquid match
}

void test_pours_round_and_keep_tie_order() {
  Config cfg;
  cfg.pumps = {{1, "A", 100, 1.0f, {}, 5}, {2, "B", 100, 1.0f, {}, 5}};
  Recipe r{"x", "X", {{"B", 1, 1}, {"A", 2, 1}, {"A", 1, 0}}};
  auto steps = computePours(cfg, r, 100);
  TEST_ASSERT_EQUAL_STRING("A", steps[0].liquid.c_str());  // order 0 first
  TEST_ASSERT_EQUAL_FLOAT(25.0f, steps[0].ml);
  TEST_ASSERT_EQUAL_STRING("B", steps[1].liquid.c_str());  // tie keeps list order
  TEST_ASSERT_EQUAL_FLOAT(25.0f, steps[1].ml);
  TEST_ASSERT_EQUAL_FLOAT(50.0f, steps[2].ml);

  auto totals = totalsByPump(steps);
  TEST_ASSERT_EQUAL(2, totals.size());
  TEST_ASSERT_EQUAL_FLOAT(75.0f, totals[0].ml);  // A listed twice is summed

  Recipe thirds{"t", "T", {{"A", 1, 0}, {"B", 2, 1}}};
  auto t = computePours(cfg, thirds, 100);
  TEST_ASSERT_EQUAL_FLOAT(33.3f, t[0].ml);
  TEST_ASSERT_EQUAL_FLOAT(66.7f, t[1].ml);
}

void test_missing_liquids() {
  JsonDocument doc;
  loadExample(doc);
  Config cfg;
  parseConfig(doc.as<JsonVariantConst>(), cfg);
  auto missing = missingLiquids(cfg, *cfg.findRecipe("paloma"));
  TEST_ASSERT_EQUAL(1, missing.size());
  TEST_ASSERT_EQUAL_STRING("Grapefruit Soda", missing[0].c_str());
  TEST_ASSERT_EQUAL(0, missingLiquids(cfg, *cfg.findRecipe("margarita")).size());
}

void test_log_format_levels_and_history() {
  FakeClock clock;
  clock.t = 123456;
  Logger log;
  log.setClock(&clock);
  CaptureSink uart;
  CaptureSink web;
  log.addSink(&uart, LogLevel::Debug);
  log.addSink(&web, LogLevel::Info);

  log.log(LogLevel::Info, "DISP", "Step %d/%d: %s", 2, 3, "Triple Sec");
  log.log(LogLevel::Debug, "PUMP", "debug detail");

  TEST_ASSERT_EQUAL(2, uart.entries.size());
  TEST_ASSERT_EQUAL(1, web.entries.size());
  TEST_ASSERT_EQUAL_STRING("[  123456] INFO  DISP - Step 2/3: Triple Sec",
                           formatLogLine(uart.entries[0]).c_str());
  TEST_ASSERT_EQUAL(1, log.history().size());  // DEBUG is not kept in history

  for (int i = 0; i < 310; i++) {
    log.log(LogLevel::Warn, "SYS", "line %d", i);
  }
  auto h = log.history();
  TEST_ASSERT_EQUAL(Logger::kHistorySize, h.size());
  TEST_ASSERT_EQUAL_STRING("line 309", h.back().msg.c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_example_config_is_valid);
  RUN_TEST(test_factory_default_is_valid);
  RUN_TEST(test_validation_rules);
  RUN_TEST(test_open_network_password_allowed);
  RUN_TEST(test_unknown_keys_are_kept_in_document);
  RUN_TEST(test_migrate_v1_adds_auto_prime);
  RUN_TEST(test_pours_split_by_parts_in_order);
  RUN_TEST(test_pours_round_and_keep_tie_order);
  RUN_TEST(test_missing_liquids);
  RUN_TEST(test_log_format_levels_and_history);
  return UNITY_END();
}
