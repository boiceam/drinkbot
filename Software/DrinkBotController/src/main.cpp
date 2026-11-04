// DrinkBotController firmware entry point (SPEC_v3).
// M2 wiring: logging, USB-or-simulator pump link, discovery and polling.
// The App task, UI, network and web server are added in later milestones.

#include <Arduino.h>

#include <memory>

#include "core/Config.h"
#include "core/Log.h"
#include "core/PumpManager.h"
#include "core/TransactionEngine.h"
#include "hal/UsbPumpLink.h"
#include "sim/SimPumpLink.h"

using namespace drinkbot;

namespace {

constexpr uint32_t kUsbDetectTimeoutMs = 3000;

class MillisClock : public IClock {
 public:
  uint32_t nowMs() const override { return millis(); }
};

// UART0 on D6 (TX) / D7 (RX), 115200 8N1 (§3.2, §12.3)
class UartLogSink : public ILogSink {
 public:
  void write(const LogEntry& entry) override { Serial.println(formatLogLine(entry).c_str()); }
};

// Logs raised/cleared errors until SystemState takes this over in M3.
class LoggingErrorReporter : public IErrorReporter {
 public:
  void raiseError(ErrorCode code, int pumpId, const std::string& message) override {
    LOG_E("SYS", "%s(%d): %s", errorCodeName(code), pumpId, message.c_str());
  }
  void clearError(ErrorCode code, int pumpId) override {
    LOG_D("SYS", "%s(%d) cleared", errorCodeName(code), pumpId);
  }
};

MillisClock clock_;
UartLogSink uartSink;
LoggingErrorReporter errorReporter;
UsbPumpLink usbLink;
std::unique_ptr<SimPumpLink> simLink;
IPumpTransport* transport = nullptr;
std::unique_ptr<TransactionEngine> engine;
std::unique_ptr<PumpManager> pumpManager;

}  // namespace

void setup() {
  Serial.begin(115200);
  logger().setClock(&clock_);
  logger().addSink(&uartSink, LogLevel::Debug);
  LOG_I("SYS", "DrinkBotController starting (build %s %s)", __DATE__, __TIME__);

  JsonDocument doc;
  makeFactoryDefaultConfig(doc);
  Config config;
  parseConfig(doc.as<JsonVariantConst>(), config);

  bool simulator = !usbLink.begin() || !usbLink.waitForDevice(kUsbDetectTimeoutMs);
  if (simulator) {
    LOG_W("SIM", "No pump USB device within 3 s: running in SIMULATOR mode");
    simLink.reset(new SimPumpLink(clock_, SimPumpLink::specsFromConfig(config)));
    transport = simLink.get();
  } else {
    transport = &usbLink;
  }

  engine.reset(new TransactionEngine(*transport, clock_));
  pumpManager.reset(new PumpManager(*engine, clock_, errorReporter));
  pumpManager->applyConfig(config);
  pumpManager->setPollingEnabled(true);
}

void loop() {
  transport->tick();
  engine->tick();
  pumpManager->tick();
  delay(5);
}
