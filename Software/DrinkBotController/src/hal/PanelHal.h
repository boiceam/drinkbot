#pragma once

#include <Adafruit_NeoPixel.h>
#include <Arduino.h>

#include <array>

#include "core/UiModel.h"

namespace drinkbot {

// XIAO ESP32-S3 pin map (SPEC_v3 §3.2, D-1): wired by D-label.
// D0..D5 = GPIO 1-6 (skipping D2 / GPIO 3, a strapping pin), D8 = GPIO 7,
// D9 = GPIO 8, D10 = GPIO 9.
constexpr std::array<uint8_t, kButtonCount> kButtonPins = {D0, D1, D3, D4, D5, D8, D9};
constexpr uint8_t kLedDataPin = D10;

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

// 9 WS2812 (GRB, 800 kHz) on D10.
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
