// M0 spike: prove the USB host CDC-ACM link to a DrinkBotPump.
// Opens the first CDC device, asserts DTR/RTS, sends `version` and `gid`
// and logs every reply line on UART0 (D6 = TX, 115200 8N1).

#include <Arduino.h>

#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"

namespace {

constexpr uint32_t kUsbDetectTimeoutMs = 3000;
constexpr uint32_t kTxTimeoutMs = 500;

cdc_acm_dev_hdl_t pumpDev = nullptr;
volatile bool pumpDisconnected = false;

char rxLine[257];
size_t rxLen = 0;

void usbHostTask(void*) {
  while (true) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
      usb_host_device_free_all();
    }
  }
}

bool onRx(const uint8_t* data, size_t len, void*) {
  for (size_t i = 0; i < len; i++) {
    char c = static_cast<char>(data[i]);
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      rxLine[rxLen] = '\0';
      Serial.printf("[%8lu] USB  < %s\n", millis(), rxLine);
      rxLen = 0;
    } else if (rxLen < sizeof(rxLine) - 1) {
      rxLine[rxLen++] = c;
    } else {
      Serial.println("USB  - RX line too long, discarded");
      rxLen = 0;
    }
  }
  return true;
}

void onEvent(const cdc_acm_host_dev_event_data_t* event, void*) {
  if (event->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
    pumpDisconnected = true;
  } else if (event->type == CDC_ACM_HOST_ERROR) {
    Serial.printf("USB  - CDC error %d\n", event->data.error);
  }
}

bool openPump() {
  cdc_acm_host_open_config_t cfg = {};
  cfg.vid = CDC_HOST_ANY_VID;
  cfg.pid = CDC_HOST_ANY_PID;
  cfg.interface_idx = 0;
  cfg.dev_addr = CDC_HOST_ANY_DEV_ADDR;
  cfg.connection_timeout_ms = kUsbDetectTimeoutMs;
  cfg.out_buffer_size = 128;
  cfg.in_buffer_size = 0;
  cfg.event_cb = onEvent;
  cfg.data_cb = onRx;
  cfg.user_arg = nullptr;

  if (cdc_acm_host_open(&cfg, &pumpDev) != ESP_OK) {
    pumpDev = nullptr;
    return false;
  }

  cdc_acm_line_coding_t coding = {};
  coding.dwDTERate = 115200;
  coding.bCharFormat = 0;
  coding.bParityType = 0;
  coding.bDataBits = 8;
  cdc_acm_host_line_coding_set(pumpDev, &coding);

  // The RP2040 TinyUSB CDC stack may drop TX until DTR is asserted
  cdc_acm_host_set_control_line_state(pumpDev, true, true);
  return true;
}

void sendLine(const char* line) {
  if (!pumpDev) {
    return;
  }
  char buf[66];
  int n = snprintf(buf, sizeof(buf), "%s\n", line);
  Serial.printf("[%8lu] USB  > %s\n", millis(), line);
  esp_err_t err = cdc_acm_host_data_tx_blocking(pumpDev, reinterpret_cast<const uint8_t*>(buf), n, kTxTimeoutMs);
  if (err != ESP_OK) {
    Serial.printf("USB  - TX failed: %s\n", esp_err_to_name(err));
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("SYS  - DrinkBotController M0 USB spike");

  usb_host_config_t hostCfg = {};
  hostCfg.skip_phy_setup = false;
  hostCfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
  ESP_ERROR_CHECK(usb_host_install(&hostCfg));
  xTaskCreatePinnedToCore(usbHostTask, "usb_host", 4096, nullptr, 10, nullptr, 1);
  ESP_ERROR_CHECK(cdc_acm_host_install(nullptr));

  if (!openPump()) {
    Serial.println("USB  - No CDC device within 3 s: would enter simulator mode");
    return;
  }
  Serial.println("USB  - CDC device opened");

  // Let the pump's USB CDC settle after DTR
  delay(100);
  sendLine("version");
  delay(200);
  sendLine("gid");
}

void loop() {
  if (pumpDisconnected) {
    pumpDisconnected = false;
    Serial.println("USB  - Pump disconnected");
    cdc_acm_host_close(pumpDev);
    pumpDev = nullptr;
  }

  static uint32_t nextPoll = 0;
  if (pumpDev && millis() >= nextPoll) {
    nextPoll = millis() + 5000;
    sendLine("weight");
  }
  delay(10);
}
