#include "core/UiModel.h"

#include <cmath>

namespace drinkbot {

namespace {

const Rgb kGreen{0, 255, 0};
const Rgb kRed{255, 0, 0};
const Rgb kYellow{255, 180, 0};
const Rgb kWhite{255, 255, 255};
const Rgb kBlack{0, 0, 0};

LedCell cell(const Rgb& color, LedMode mode, const Rgb& alt = kBlack) {
  return {color, mode, alt};
}

Rgb scale(const Rgb& c, uint8_t brightness) {
  auto s = [brightness](uint8_t v) { return static_cast<uint8_t>((v * brightness + 127) / 255); };
  return {s(c.r), s(c.g), s(c.b)};
}

}  // namespace

const char* ledModeName(LedMode mode) {
  switch (mode) {
    case LedMode::Off: return "off";
    case LedMode::Solid: return "solid";
    case LedMode::Blink: return "blink";
    case LedMode::Fast: return "fast";
  }
  return "off";
}

const char* wifiModeName(WifiMode mode) {
  switch (mode) {
    case WifiMode::Connecting: return "CONNECTING";
    case WifiMode::Station: return "STA";
    case WifiMode::AccessPoint: return "AP";
  }
  return "CONNECTING";
}

void UiModel::updateButtons(const std::array<bool, kButtonCount>& pressed, std::vector<ButtonEvent>& events) {
  uint32_t now = clock_.nowMs();
  for (size_t i = 0; i < kButtonCount; i++) {
    Debounce& b = buttons_[i];
    if (pressed[i] != b.raw) {
      b.raw = pressed[i];
      b.changedAt = now;
    }
    if (b.raw != b.stable && now - b.changedAt >= kDebounceMs) {
      b.stable = b.raw;
      if (b.stable) {
        b.pressedAt = now;
        b.longFired = false;
        events.push_back({i, ButtonEvent::Kind::Press});
      }
    }
    if (b.stable && !b.longFired && now - b.pressedAt >= kLongPressMs) {
      b.longFired = true;
      events.push_back({i, ButtonEvent::Kind::LongPress});
    }
  }
}

void UiModel::flashPress(size_t index) {
  overlays_[index] = {Overlay::Kind::Press, clock_.nowMs() + kPressFlashMs};
}

void UiModel::flashFailure(size_t index) {
  overlays_[index] = {Overlay::Kind::Failure, clock_.nowMs() + kTripleFlashMs};
}

void UiModel::flashConfirm(size_t index) {
  overlays_[index] = {Overlay::Kind::Confirm, clock_.nowMs() + kTripleFlashMs};
}

LedCell UiModel::buttonCell(size_t index, const UiInputs& in) const {
  const Overlay& o = overlays_[index];
  if (o.kind != Overlay::Kind::None && !timeReached(clock_.nowMs(), o.until)) {
    switch (o.kind) {
      case Overlay::Kind::Press: return cell(kGreen, LedMode::Solid);
      case Overlay::Kind::Failure: return cell(kRed, LedMode::Fast);
      case Overlay::Kind::Confirm: return cell(kGreen, LedMode::Fast);
      case Overlay::Kind::None: break;
    }
  }

  const ButtonConfig& b = in.buttons[index];
  switch (b.action) {
    case ButtonAction::SetRecipe:
      return b.recipeId == in.activeRecipeId ? cell(kRed, LedMode::Solid) : cell(kBlack, LedMode::Off);
    case ButtonAction::SetVolume:
      return std::fabs(b.volumeMl - in.activeVolumeMl) < 0.05f ? cell(kRed, LedMode::Solid)
                                                               : cell(kBlack, LedMode::Off);
    case ButtonAction::Dispense:
      return in.state == SysState::Dispensing ? cell(kYellow, LedMode::Blink) : cell(kBlack, LedMode::Off);
    case ButtonAction::Estop:
    case ButtonAction::None:
      return cell(kBlack, LedMode::Off);
  }
  return cell(kBlack, LedMode::Off);
}

std::array<LedCell, kLedCount> UiModel::model(const UiInputs& in) const {
  std::array<LedCell, kLedCount> leds{};
  for (size_t i = 0; i < kButtonCount; i++) {
    leds[i] = buttonCell(i, in);
  }

  switch (in.state) {
    case SysState::Booting:
      leds[kStatusLed] = cell(kWhite, LedMode::Solid);
      break;
    case SysState::Ready:
      leds[kStatusLed] = cell(kGreen, LedMode::Solid);
      break;
    case SysState::Dispensing:
    case SysState::Manual:
      // Busy under a (degraded) error alternates yellow / red
      leds[kStatusLed] = cell(kYellow, LedMode::Blink, in.hasErrors ? kRed : kBlack);
      break;
    case SysState::Error:
      leds[kStatusLed] = cell(kRed, LedMode::Blink);
      break;
  }

  switch (in.wifi) {
    case WifiMode::Connecting:
      leds[kWifiLed] = cell(kGreen, LedMode::Blink);
      break;
    case WifiMode::Station:
      leds[kWifiLed] = cell(kGreen, LedMode::Solid);
      break;
    case WifiMode::AccessPoint:
      leds[kWifiLed] = cell(kYellow, LedMode::Solid);
      break;
  }
  return leds;
}

std::array<Rgb, kLedCount> UiModel::render(const UiInputs& in, uint8_t brightness) const {
  uint32_t now = clock_.nowMs();
  bool blinkOn = (now / kBlinkHalfMs) % 2 == 0;
  bool fastOn = (now / kFastHalfMs) % 2 == 0;
  std::array<Rgb, kLedCount> out{};
  auto leds = model(in);
  for (size_t i = 0; i < kLedCount; i++) {
    const LedCell& c = leds[i];
    Rgb color = kBlack;
    switch (c.mode) {
      case LedMode::Off: break;
      case LedMode::Solid: color = c.color; break;
      case LedMode::Blink: color = blinkOn ? c.color : c.altColor; break;
      case LedMode::Fast: color = fastOn ? c.color : c.altColor; break;
    }
    out[i] = scale(color, brightness);
  }
  return out;
}

}  // namespace drinkbot
