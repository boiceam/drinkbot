#pragma once

#include <Adafruit_NeoPixel.h>
#include <Arduino.h>

#include <array>

#include "core/UiModel.h"

namespace drinkbot {

// Panel pin map by ESP32-S3 GPIO number (SPEC_v3 §3.2, D-1). The same GPIOs
// are used on the YD-ESP32-S3 and the earlier XIAO ESP32-S3 (XIAO D0, D1,
// D3, D4, D5, D8, D9 and D10). GPIO 3 is a strapping pin and is skipped.
constexpr std::array<uint8_t, kButtonCount> kButtonPins = {1, 2, 4, 5, 6, 7, 8};
constexpr uint8_t kLedDataPin = 9;

// 7 active-low buttons with internal pull-ups.
class Buttons {
 public:
  void begin() {
    for (uint8_t pin : kButtonPins) {
      pinMode(pin, INPUT_PULLUP);
    }
  }
  std::array<bool, kButtonCount> read() const {
    std::array<bool, kButtonCount> pressed{};
    for (size_t i = 0; i < kButtonCount; i++) {
      pressed[i] = digitalRead(kButtonPins[i]) == LOW;
    }
    return pressed;
  }
};

// 9 WS2812 (GRB, 800 kHz) on GPIO 9.
class LedStrip {
 public:
  LedStrip() : strip_(kLedCount, kLedDataPin, NEO_GRB + NEO_KHZ800) {}
  void begin() {
    strip_.begin();
    strip_.clear();
    strip_.show();
  }
  // Only writes to the strip when a pixel changed.
  void show(const std::array<Rgb, kLedCount>& frame) {
    if (frame_ == frame && shown_) {
      return;
    }
    for (size_t i = 0; i < kLedCount; i++) {
      strip_.setPixelColor(i, strip_.Color(frame[i].r, frame[i].g, frame[i].b));
    }
    strip_.show();
    frame_ = frame;
    shown_ = true;
  }

 private:
  Adafruit_NeoPixel strip_;
  std::array<Rgb, kLedCount> frame_{};
  bool shown_ = false;
};

}  // namespace drinkbot
