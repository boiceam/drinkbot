// Buttons and controller LEDs (SPEC_v3 §3.3, §10, D-4), against the simulator.

#include <unity.h>

#include <algorithm>
#include <string>
#include <vector>

#include "core/App.h"
#include "core/UiController.h"
#include "core/UiModel.h"
#include "sim/SimPumpLink.h"

using namespace drinkbot;

namespace {

const char kConfig[] = R"json({
  "schemaVersion": 2,
  "defaults": { "volumeMl": 250, "recipeId": "shot", "primeSec": 5, "purgeSec": 10 },
  "pumps": [
    { "pumpId": 1, "liquid": "Tequila", "msPerMl": 20, "densityGPerMl": 1.0, "color": [255,160,0], "autoPrimeSec": 1 }
  ],
  "recipes": [
    { "id": "shot", "name": "Shot", "ingredients": [ { "liquid": "Tequila", "parts": 1, "order": 1 } ] },
    { "id": "double", "name": "Double", "ingredients": [ { "liquid": "Tequila", "parts": 1, "order": 1 } ] }
  ],
  "buttons": [
    { "action": "setRecipe", "recipeId": "shot" },
    { "action": "setRecipe", "recipeId": "double" },
    { "action": "setVolume", "volumeMl": 150 },
    { "action": "setVolume", "volumeMl": 250 },
    { "action": "none" },
    { "action": "dispense" },
    { "action": "estop" }
  ]
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
  std::array<bool, kButtonCount> held{};

  Rig() {
    app.begin(kConfig, false);
    run(3000);
  }

  void run(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
      clock.t++;
      std::vector<ButtonEvent> events;
      model.updateButtons(held, events);
      ui.handle(events);
      app.tick();
    }
  }

  void tap(size_t index, uint32_t holdMs = 100) {
    held[index] = true;
    run(holdMs);
    held[index] = false;
    run(50);
  }

  LedCell led(size_t i) const { return ui.leds()[i]; }
};

bool isColor(const LedCell& c, uint8_t r, uint8_t g, uint8_t b) {
  return c.color.r == r && c.color.g == g && c.color.b == b;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_debounce_ignores_bounces_and_fires_once() {
  FakeClock clock;
  UiModel m(clock);
  std::array<bool, kButtonCount> raw{};
  std::vector<ButtonEvent> events;
  for (int i = 0; i < 10; i++) {  // 10 ms of chatter
    raw[0] = i % 2 == 0;
    clock.t++;
    m.updateButtons(raw, events);
  }
  TEST_ASSERT_EQUAL(0, events.size());
  raw[0] = true;
  for (int i = 0; i < 29; i++) {
    clock.t++;
    m.updateButtons(raw, events);
  }
  TEST_ASSERT_EQUAL(0, events.size());  // not yet stable for 30 ms
  for (int i = 0; i < 500; i++) {
    clock.t++;
    m.updateButtons(raw, events);
  }
  TEST_ASSERT_EQUAL(1, events.size());  // holding does not repeat
  TEST_ASSERT_TRUE(events[0].kind == ButtonEvent::Kind::Press);
}

void test_long_press_after_three_seconds() {
  FakeClock clock;
  UiModel m(clock);
  std::array<bool, kButtonCount> raw{};
  raw[6] = true;
  std::vector<ButtonEvent> events;
  for (int i = 0; i < 3100; i++) {
    clock.t++;
    m.updateButtons(raw, events);
  }
  TEST_ASSERT_EQUAL(2, events.size());
  TEST_ASSERT_TRUE(events[1].kind == ButtonEvent::Kind::LongPress);
}

void test_status_and_wifi_leds() {
  Rig rig;
  TEST_ASSERT_TRUE(isColor(rig.led(kStatusLed), 0, 255, 0));  // READY solid green
  TEST_ASSERT_TRUE(rig.led(kStatusLed).mode == LedMode::Solid);
  TEST_ASSERT_TRUE(rig.led(kWifiLed).mode == LedMode::Blink);  // searching
  rig.ui.setWifiMode(WifiMode::AccessPoint);
  TEST_ASSERT_TRUE(isColor(rig.led(kWifiLed), 255, 180, 0));
  rig.ui.setWifiMode(WifiMode::Station);
  TEST_ASSERT_TRUE(rig.led(kWifiLed).mode == LedMode::Solid);
  TEST_ASSERT_TRUE(isColor(rig.led(kWifiLed), 0, 255, 0));
}

void test_recipe_button_flashes_green_then_red() {
  Rig rig;
  TEST_ASSERT_TRUE(isColor(rig.led(0), 255, 0, 0));  // default recipe is active
  TEST_ASSERT_TRUE(rig.led(1).mode == LedMode::Off);

  rig.held[1] = true;
  rig.run(100);
  TEST_ASSERT_TRUE(isColor(rig.led(1), 0, 255, 0));  // press feedback
  rig.held[1] = false;
  rig.run(300);
  TEST_ASSERT_EQUAL_STRING("double", rig.app.activeRecipeId().c_str());
  TEST_ASSERT_TRUE(isColor(rig.led(1), 255, 0, 0));
  TEST_ASSERT_TRUE(rig.led(0).mode == LedMode::Off);
}

void test_volume_led_follows_web_setting() {
  Rig rig;
  TEST_ASSERT_TRUE(isColor(rig.led(3), 255, 0, 0));  // 250 ml default
  Command c;
  c.type = Command::Type::SetVolume;
  c.number = 150;
  rig.app.post(c);
  rig.run(10);
  TEST_ASSERT_TRUE(isColor(rig.led(2), 255, 0, 0));
  TEST_ASSERT_TRUE(rig.led(3).mode == LedMode::Off);
  c.number = 300;  // no button for 300 ml: both off
  rig.app.post(c);
  rig.run(10);
  TEST_ASSERT_TRUE(rig.led(2).mode == LedMode::Off && rig.led(3).mode == LedMode::Off);
}

void test_dispense_button_blinks_yellow_and_status_too() {
  Rig rig;
  rig.tap(5);
  rig.run(300);
  TEST_ASSERT_TRUE(rig.app.system().state() == SysState::Dispensing);
  TEST_ASSERT_TRUE(rig.led(5).mode == LedMode::Blink);
  TEST_ASSERT_TRUE(isColor(rig.led(5), 255, 180, 0));
  TEST_ASSERT_TRUE(rig.led(kStatusLed).mode == LedMode::Blink);
  TEST_ASSERT_TRUE(isColor(rig.led(kStatusLed), 255, 180, 0));
}

void test_refused_dispense_flashes_red() {
  Rig rig;
  rig.sim.setWeight(1, 5);  // nearly empty: precheck fails
  rig.tap(5);
  rig.run(100);
  TEST_ASSERT_FALSE(rig.app.system().busy());
  TEST_ASSERT_TRUE(rig.led(5).mode == LedMode::Fast);
  TEST_ASSERT_TRUE(isColor(rig.led(5), 255, 0, 0));
}

void test_estop_hold_clears_latched_errors() {
  Rig rig;
  // A pump that never reports idle overruns and latches E_DISPENSE_TIMEOUT
  rig.sim.setStuck(1, true);
  Command prime;
  prime.type = Command::Type::Prime;
  prime.pumpId = 1;
  prime.number = 1;
  rig.app.post(prime);
  rig.run(8000);
  rig.sim.setStuck(1, false);
  TEST_ASSERT_TRUE(rig.app.system().hasLatchedError());

  rig.tap(6, 200);  // short press: E-Stop only
  TEST_ASSERT_TRUE(rig.app.system().hasLatchedError());

  rig.tap(6, 3200);  // hold 3 s
  TEST_ASSERT_FALSE(rig.app.system().hasLatchedError());
  TEST_ASSERT_TRUE(rig.led(6).mode == LedMode::Fast);  // green confirm
  TEST_ASSERT_TRUE(isColor(rig.led(6), 0, 255, 0));
}

void test_render_applies_blink_phase_and_brightness() {
  FakeClock clock;
  UiModel m(clock);
  UiInputs in;
  in.state = SysState::Error;
  in.wifi = WifiMode::Station;
  clock.t = 0;  // blink on phase
  auto on = m.render(in, 255);
  TEST_ASSERT_EQUAL(255, on[kStatusLed].r);
  clock.t = 600;  // off phase
  auto off = m.render(in, 255);
  TEST_ASSERT_EQUAL(0, off[kStatusLed].r);
  auto dim = m.render(in, 64);
  TEST_ASSERT_EQUAL(64, dim[kWifiLed].g);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_debounce_ignores_bounces_and_fires_once);
  RUN_TEST(test_long_press_after_three_seconds);
  RUN_TEST(test_status_and_wifi_leds);
  RUN_TEST(test_recipe_button_flashes_green_then_red);
  RUN_TEST(test_volume_led_follows_web_setting);
  RUN_TEST(test_dispense_button_blinks_yellow_and_status_too);
  RUN_TEST(test_refused_dispense_flashes_red);
  RUN_TEST(test_estop_hold_clears_latched_errors);
  RUN_TEST(test_render_applies_blink_phase_and_brightness);
  return UNITY_END();
}
