// DrinkBotController firmware entry point (SPEC_v3).
// M5 wiring: logging, config storage, Wi-Fi, USB-or-simulator pump link, the
// App and the button / LED panel. The web server is added in M6.

#include <Arduino.h>

#include <memory>

#include "core/App.h"
#include "core/Config.h"
#include "core/ConfigStore.h"
#include "core/Log.h"
#include "core/UiController.h"
#include "core/UiModel.h"
#include "hal/LittleFsStore.h"
#include "hal/NetManager.h"
#include "hal/PanelHal.h"
#include "hal/UsbPumpLink.h"
#include "sim/SimPumpLink.h"

using namespace drinkbot;

namespace {

constexpr uint32_t kUsbDetectTimeoutMs = 3000;
constexpr uint32_t kAppTickMs = 5;  // buttons are sampled every tick (>= 100 Hz)
constexpr uint32_t kLedRefreshMs = 20;

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
LittleFsStore files;
ConfigStore configStore(files);
NetManager net;
UsbPumpLink usbLink;
std::unique_ptr<SimPumpLink> simLink;
std::unique_ptr<App> app;
Buttons buttons;
LedStrip leds;
UiModel uiModel(clock_);
std::unique_ptr<UiController> ui;
uint32_t nextLedRefresh = 0;

}  // namespace

void setup() {
  Serial.begin(115200);
  buttons.begin();
  leds.begin();
  logger().setClock(&clock_);
  logger().addSink(&uartSink, LogLevel::Debug);
  LOG_I("SYS", "DrinkBotController starting (build %s %s, reset reason %d)", __DATE__, __TIME__,
        static_cast<int>(esp_reset_reason()));

  // §9.1 step 2: status LED white before the 3 s USB detection
  std::array<Rgb, kLedCount> bootFrame{};
  bootFrame[kStatusLed] = {64, 64, 64};
  leds.show(bootFrame);

  // §9.1 step 3: mount LittleFS and load config.json -> config.bak -> default
  if (!files.begin()) {
    LOG_E("CFG", "LittleFS unavailable; config changes will not be saved");
  }
  std::string configJson;
  ConfigStore::Source source = configStore.load(configJson);

  // Typed view for Wi-Fi and the simulator; the App re-validates it in begin()
  Config config;
  {
    JsonDocument doc;
    deserializeJson(doc, configJson);
    migrateConfig(doc);
    parseConfig(doc.as<JsonVariantConst>(), config);
  }

  // §9.1 step 5: Wi-Fi runs in the background while the pumps are found
  net.begin(config);

  bool simulator = !usbLink.begin() || !usbLink.waitForDevice(kUsbDetectTimeoutMs);
  if (simulator) {
    LOG_W("SIM", "No pump USB device within 3 s: running in SIMULATOR mode");
    simLink.reset(new SimPumpLink(clock_, SimPumpLink::specsFromConfig(config)));
    app.reset(new App(*simLink, clock_, simLink.get()));
  } else {
    app.reset(new App(usbLink, clock_));
  }

  app->setHooks({[](const std::string& json) { return configStore.save(json); }, [] { ESP.restart(); }});
  ui.reset(new UiController(*app, uiModel));
  leds.show(ui->frame());  // status LED white while booting
  app->begin(configJson, source != ConfigStore::Source::Primary);
}

void loop() {
  std::vector<ButtonEvent> events;
  uiModel.updateButtons(buttons.read(), events);
  ui->handle(events);
  app->tick();
  net.tick();
  ui->setWifiMode(net.mode());

  uint32_t now = millis();
  if (timeReached(now, nextLedRefresh)) {
    nextLedRefresh = now + kLedRefreshMs;
    leds.show(ui->frame());
  }
  delay(kAppTickMs);
}
