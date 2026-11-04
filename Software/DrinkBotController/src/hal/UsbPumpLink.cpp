#include "hal/UsbPumpLink.h"

#include <Arduino.h>

#include "core/Log.h"
#include "usb/usb_host.h"

namespace drinkbot {

namespace {
constexpr uint32_t kOpenTimeoutMs = 1000;
constexpr uint32_t kTxTimeoutMs = 200;
constexpr size_t kMaxQueuedLines = 64;
}  // namespace

UsbPumpLink::UsbPumpLink()
    : framer_(
          [this](const std::string& line) {
            std::lock_guard<std::mutex> lock(linesMutex_);
            if (lines_.size() >= kMaxQueuedLines) {
              lines_.pop_front();
            }
            lines_.push_back(line);
          },
          [] { LOG_W("USB", "RX line longer than 256 bytes discarded"); }) {}

bool UsbPumpLink::begin() {
  usb_host_config_t hostCfg = {};
  hostCfg.skip_phy_setup = false;
  hostCfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
  if (usb_host_install(&hostCfg) != ESP_OK) {
    LOG_E("USB", "USB host install failed");
    return false;
  }
  xTaskCreatePinnedToCore(hostTask, "usb_host", 4096, this, 10, nullptr, 1);
  if (cdc_acm_host_install(nullptr) != ESP_OK) {
    LOG_E("USB", "CDC-ACM host driver install failed");
    return false;
  }
  xTaskCreatePinnedToCore(connectionTask, "usb_conn", 4096, this, 5, nullptr, 1);
  return true;
}

bool UsbPumpLink::waitForDevice(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (!connected() && millis() - start < timeoutMs) {
    delay(20);
  }
  return connected();
}

void UsbPumpLink::hostTask(void*) {
  while (true) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
      usb_host_device_free_all();
    }
  }
}

void UsbPumpLink::connectionTask(void* arg) {
  auto* self = static_cast<UsbPumpLink*>(arg);
  while (true) {
    if (self->disconnectPending_.exchange(false)) {
      self->close();
      LOG_W("USB", "Pump USB device disconnected");
    }
    if (!self->connected() && self->open()) {
      self->attaches_++;
      LOG_I("USB", "Pump USB device connected");
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

bool UsbPumpLink::onRx(const uint8_t* data, size_t len, void* arg) {
  static_cast<UsbPumpLink*>(arg)->framer_.feed(data, len);
  return true;
}

void UsbPumpLink::onEvent(const cdc_acm_host_dev_event_data_t* event, void* arg) {
  auto* self = static_cast<UsbPumpLink*>(arg);
  if (event->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
    self->connected_ = false;
    self->disconnectPending_ = true;
  } else if (event->type == CDC_ACM_HOST_ERROR) {
    LOG_W("USB", "CDC error %d", event->data.error);
  }
}

bool UsbPumpLink::open() {
  cdc_acm_host_open_config_t cfg = {};
  cfg.vid = CDC_HOST_ANY_VID;
  cfg.pid = CDC_HOST_ANY_PID;
  cfg.interface_idx = 0;
  cfg.dev_addr = CDC_HOST_ANY_DEV_ADDR;
  cfg.connection_timeout_ms = kOpenTimeoutMs;
  cfg.out_buffer_size = 128;
  cfg.in_buffer_size = 0;
  cfg.event_cb = onEvent;
  cfg.data_cb = onRx;
  cfg.user_arg = this;

  std::lock_guard<std::mutex> lock(devMutex_);
  cdc_acm_dev_hdl_t dev = nullptr;
  if (cdc_acm_host_open(&cfg, &dev) != ESP_OK) {
    return false;
  }

  cdc_acm_line_coding_t coding = {};
  coding.dwDTERate = 115200;
  coding.bDataBits = 8;
  cdc_acm_host_line_coding_set(dev, &coding);
  // The RP2040 TinyUSB CDC stack may drop TX until DTR is asserted (§5.1)
  cdc_acm_host_set_control_line_state(dev, true, true);

  framer_.reset();
  {
    std::lock_guard<std::mutex> lines(linesMutex_);
    lines_.clear();
  }
  dev_ = dev;
  connected_ = true;
  return true;
}

void UsbPumpLink::close() {
  std::lock_guard<std::mutex> lock(devMutex_);
  connected_ = false;
  if (dev_) {
    cdc_acm_host_close(dev_);
    dev_ = nullptr;
  }
}

bool UsbPumpLink::writeLine(const std::string& line) {
  if (!connected()) {
    return false;
  }
  std::string out = line + "\n";
  std::lock_guard<std::mutex> lock(devMutex_);
  if (!dev_) {
    return false;
  }
  esp_err_t err =
      cdc_acm_host_data_tx_blocking(dev_, reinterpret_cast<const uint8_t*>(out.data()), out.size(), kTxTimeoutMs);
  if (err != ESP_OK) {
    LOG_W("USB", "TX failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

bool UsbPumpLink::readLine(std::string& line) {
  std::lock_guard<std::mutex> lock(linesMutex_);
  if (lines_.empty()) {
    return false;
  }
  line = std::move(lines_.front());
  lines_.pop_front();
  return true;
}

}  // namespace drinkbot
