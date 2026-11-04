// DrinkBotController firmware entry point (SPEC_v3).
// M3 wiring: logging, USB-or-simulator pump link and the App. Buttons/LEDs,
// config storage, networking and the web server are added in M4-M6.

#include <Arduino.h>

#include <memory>

#include "core/App.h"
#include "core/Config.h"
#include "core/Log.h"
#include "hal/UsbPumpLink.h"
#include "sim/SimPumpLink.h"

using namespace drinkbot;

namespace {

constexpr uint32_t kUsbDetectTimeoutMs = 3000;
constexpr uint32_t kAppTickMs = 5;

class MillisClock : public IClock {
 public:
  uint32_t nowMs() const override { return millis(); }
};

// UART0 on D6 (TX) / D7 (RX), 115200 8N1 (§3.2, §12.3)
class UartLogSink : public ILogSink {
 public:
  void write(const LogEntry& entry) override { Serial.println(formatLogLine(entry).c_str()); }
};

MillisClock clock_;
UartLogSink uartSink;
UsbPumpLink usbLink;
std::unique_ptr<SimPumpLink> simLink;
std::unique_ptr<App> app;

}  // namespace

void setup() {
  Serial.begin(115200);
  logger().setClock(&clock_);
  logger().addSink(&uartSink, LogLevel::Debug);
  LOG_I("SYS", "DrinkBotController starting (build %s %s, reset reason %d)", __DATE__, __TIME__,
        static_cast<int>(esp_reset_reason()));

  // Until config storage lands (M5) the factory default is used
  JsonDocument doc;
  makeFactoryDefaultConfig(doc);
  std::string configJson;
  serializeJson(doc, configJson);

  bool simulator = !usbLink.begin() || !usbLink.waitForDevice(kUsbDetectTimeoutMs);
  if (simulator) {
    LOG_W("SIM", "No pump USB device within 3 s: running in SIMULATOR mode");
    Config config;
    parseConfig(doc.as<JsonVariantConst>(), config);
    simLink.reset(new SimPumpLink(clock_, SimPumpLink::specsFromConfig(config)));
    app.reset(new App(*simLink, clock_, simLink.get()));
  } else {
    app.reset(new App(usbLink, clock_));
  }

  app->setHooks({[](const std::string&) {
                   LOG_W("CFG", "Config storage not implemented yet; change kept in RAM only");
                   return true;
                 },
                 [] { ESP.restart(); }});
  app->begin(configJson, false);
}

void loop() {
  app->tick();
  delay(kAppTickMs);
}
