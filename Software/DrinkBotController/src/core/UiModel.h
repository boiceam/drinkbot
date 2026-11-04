#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/Clock.h"
#include "core/Config.h"
#include "core/SystemState.h"

namespace drinkbot {

// Controller panel: 7 buttons and 9 NeoPixels (SPEC_v3 §3.3, §10).

constexpr size_t kLedCount = 9;
constexpr size_t kStatusLed = 7;  // LED 8
constexpr size_t kWifiLed = 8;    // LED 9

enum class LedMode : uint8_t { Off, Solid, Blink, Fast };
const char* ledModeName(LedMode mode);

enum class WifiMode : uint8_t { Connecting, Station, AccessPoint };
const char* wifiModeName(WifiMode mode);

// Logical LED state, as mirrored in the web UI. A blinking LED shows
// `color` in its on phase and `altColor` (black unless set) in its off phase.
struct LedCell {
  Rgb color;
  LedMode mode = LedMode::Off;
  Rgb altColor;
};

struct ButtonEvent {
  enum class Kind : uint8_t { Press, LongPress };
  size_t index = 0;  // 0-based (Button 1 = 0)
  Kind kind = Kind::Press;
};

// Everything the LEDs depend on, gathered by the caller each frame.
struct UiInputs {
  SysState state = SysState::Booting;
  bool hasErrors = false;
  WifiMode wifi = WifiMode::Connecting;
  std::string activeRecipeId;
  float activeVolumeMl = 0;
  std::array<ButtonConfig, kButtonCount> buttons{};
};

class UiModel {
 public:
  static constexpr uint32_t kDebounceMs = 30;
  static constexpr uint32_t kLongPressMs = 3000;
  static constexpr uint32_t kPressFlashMs = 250;
  static constexpr uint32_t kBlinkHalfMs = 500;
  static constexpr uint32_t kFastHalfMs = 150;
  static constexpr uint32_t kTripleFlashMs = 6 * kFastHalfMs;

  explicit UiModel(const IClock& clock) : clock_(clock) {}

  // Feeds the raw (already active-low inverted) button levels; appends a
  // Press on each debounced press edge and one LongPress per hold >= 3 s.
  void updateButtons(const std::array<bool, kButtonCount>& pressed, std::vector<ButtonEvent>& events);

  // Feedback overlays on a button LED
  void flashPress(size_t index);    // green 250 ms
  void flashFailure(size_t index);  // red, 3 fast blinks
  void flashConfirm(size_t index);  // green, 3 fast blinks

  // Logical state of all 9 LEDs (§10.2).
  std::array<LedCell, kLedCount> model(const UiInputs& in) const;
  // Colors to show now: blink phase applied and scaled by `brightness`.
  std::array<Rgb, kLedCount> render(const UiInputs& in, uint8_t brightness) const;

 private:
  struct Debounce {
    bool raw = false;
    bool stable = false;
    uint32_t changedAt = 0;
    uint32_t pressedAt = 0;
    bool longFired = false;
  };
  struct Overlay {
    enum class Kind : uint8_t { None, Press, Failure, Confirm };
    Kind kind = Kind::None;
    uint32_t until = 0;
  };

  LedCell buttonCell(size_t index, const UiInputs& in) const;

  const IClock& clock_;
  std::array<Debounce, kButtonCount> buttons_{};
  std::array<Overlay, kButtonCount> overlays_{};
};

}  // namespace drinkbot
