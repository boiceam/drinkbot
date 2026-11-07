#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <string>

#include "core/LineFramer.h"
#include "core/PumpTransport.h"
#include "usb/cdc_acm_host.h"

namespace drinkbot {

// USB host CDC-ACM link to the bridge pump (SPEC_v3 §5.1). A background
// task keeps trying to open the first CDC device and reopens it after a
// re-plug or a transfer failure; received lines are queued for the App task.
// A device that isn't a serial port (an RP2040 in USB boot mode, say) is held
// open but unused until it is unplugged, and reported by deviceProblem().
class UsbPumpLink : public IPumpTransport {
 public:
  UsbPumpLink();

  // Installs the USB host stack and starts the connection task.
  bool begin();
  // Blocks until a device is attached (usable or not) or `timeoutMs` passes
  // (boot detection: no device at all means simulator mode).
  bool waitForDevice(uint32_t timeoutMs);
  // Increments every time a device is opened, so callers can spot a re-plug.
  uint32_t attachCount() const { return attaches_.load(); }

  bool connected() const override { return connected_.load(); }
  bool writeLine(const std::string& line) override;
  bool readLine(std::string& line) override;
  std::string deviceProblem() const override;

 private:
  // Consecutive TX timeouts before the link is reopened
  static constexpr int kMaxTxTimeouts = 3;

  static void hostTask(void* arg);
  static void connectionTask(void* arg);
  static bool onRx(const uint8_t* data, size_t len, void* arg);
  static void onEvent(const cdc_acm_host_dev_event_data_t* event, void* arg);
  static void onNewDevice(usb_device_handle_t usbDev);

  bool open();
  void close();
  void setProblem(const std::string& problem);
  void requestReopen(const char* why);

  LineFramer framer_;
  std::mutex linesMutex_;
  std::deque<std::string> lines_;
  std::mutex devMutex_;
  cdc_acm_dev_hdl_t dev_ = nullptr;
  std::atomic<bool> connected_{false};
  std::atomic<bool> disconnectPending_{false};
  std::atomic<bool> reopenPending_{false};
  std::atomic<bool> unusableDevice_{false};  // open but not a serial port
  std::atomic<int> txTimeouts_{0};
  std::atomic<uint32_t> attaches_{0};
  std::atomic<uint32_t> lastVidPid_{0};  // of the most recently attached device
  mutable std::mutex problemMutex_;
  std::string problem_;
};

}  // namespace drinkbot
