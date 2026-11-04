#pragma once

#include <array>
#include <vector>

#include "core/App.h"
#include "core/UiModel.h"

namespace drinkbot {

// Connects the physical panel to the App (SPEC_v3 §10.1). Runs on the App
// task, so App replies can drive LED feedback directly.
class UiController {
 public:
  UiController(App& app, UiModel& model) : app_(app), model_(model) {}

  void setWifiMode(WifiMode mode) { wifi_ = mode; }
  WifiMode wifiMode() const { return wifi_; }

  void handle(const std::vector<ButtonEvent>& events);

  UiInputs inputs() const;
  std::array<LedCell, kLedCount> leds() const { return model_.model(inputs()); }
  std::array<Rgb, kLedCount> frame() const { return model_.render(inputs(), app_.config().ledBrightness); }

 private:
  void press(size_t index);
  void longPress(size_t index);

  App& app_;
  UiModel& model_;
  WifiMode wifi_ = WifiMode::Connecting;
};

}  // namespace drinkbot
