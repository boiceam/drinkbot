#pragma once

#include <string>

namespace drinkbot {

// Line-level link to the directly attached (bridge) pump. Implemented by
// UsbPumpLink on hardware and SimPumpLink in simulator mode and tests.
// All calls are made from the App task.
class IPumpTransport {
 public:
  virtual ~IPumpTransport() = default;

  virtual bool connected() const = 0;
  // Sends one line; the implementation appends '\n'.
  virtual bool writeLine(const std::string& line) = 0;
  // Non-blocking: returns the next received line (without '\n'), if any.
  virtual bool readLine(std::string& line) = 0;
  // Called every App tick (the simulator advances its pumps here).
  virtual void tick() {}
  // Non-empty while a device is attached that can't be used as the bridge
  // (e.g. "in USB boot mode"); connected() is false meanwhile.
  virtual std::string deviceProblem() const { return {}; }
};

}  // namespace drinkbot
