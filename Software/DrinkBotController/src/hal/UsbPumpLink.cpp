#include "hal/UsbPumpLink.h"

#include <Arduino.h>

#include "core/Log.h"
#include "usb/usb_host.h"

namespace drinkbot {

namespace {
constexpr uint32_t kOpenTimeoutMs = 1000;
constexpr uint32_t kTxTimeoutMs = 200;
constexpr size_t kMaxQueuedLines = 64;
constexpr uint32_t kReopenDelayMs = 1000;  // between closing a failed link and reopening it

// Raspberry Pi ROM bootloaders (the RPI-RP2 / RP2350 drive): no serial port
constexpr uint16_t kRaspberryPiVid = 0x2E8A;
constexpr uint16_t kRp2040BootPid = 0x0003;
constexpr uint16_t kRp2350BootPid = 0x000F;

UsbPumpLink* instance = nullptr;  // for onNewDevice, which has no user argument
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
  instance = this;
  usb_host_config_t hostCfg = {};
  hostCfg.skip_phy_setup = false;
  hostCfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
  if (usb_host_install(&hostCfg) != ESP_OK) {
    LOG_E("USB", "USB host install failed");
    return false;
  }
  xTaskCreatePinnedToCore(hostTask, "usb_host", 4096, this, 10, nullptr, 1);
  cdc_acm_host_driver_config_t driverCfg = {};
  driverCfg.driver_task_stack_size = 4096;
  driverCfg.driver_task_priority = 10;
  driverCfg.xCoreID = 0;  // the driver's defaults, plus the new-device callback
  driverCfg.new_dev_cb = onNewDevice;
  if (cdc_acm_host_install(&driverCfg) != ESP_OK) {
    LOG_E("USB", "CDC-ACM host driver install failed");
    return false;
  }
  xTaskCreatePinnedToCore(connectionTask, "usb_conn", 4096, this, 5, nullptr, 1);
  return true;
}

bool UsbPumpLink::waitForDevice(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (!connected() && !unusableDevice_ && millis() - start < timeoutMs) {
    delay(20);
  }
  return connected() || unusableDevice_;
}

std::string UsbPumpLink::deviceProblem() const {
  std::lock_guard<std::mutex> lock(problemMutex_);
  return problem_;
}

void UsbPumpLink::setProblem(const std::string& problem) {
  std::lock_guard<std::mutex> lock(problemMutex_);
  problem_ = problem;
}

void UsbPumpLink::requestReopen(const char* why) {
  if (!unusableDevice_ && connected_ && !reopenPending_.exchange(true)) {
    LOG_W("USB", "Pump USB link failed (%s); reopening it", why);
  }
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
      bool wasUnusable = self->unusableDevice_;
      self->close();
      LOG_W("USB", wasUnusable ? "Unusable USB device removed" : "Pump USB device disconnected");
    }
    if (self->reopenPending_.exchange(false) && !self->unusableDevice_) {
      // The device is still attached but transfers fail (the driver stops
      // reading after an error), so start over with a fresh handle
      self->close();
      vTaskDelay(pdMS_TO_TICKS(kReopenDelayMs));
    }
    if (!self->connected() && !self->unusableDevice_ && self->open()) {
      self->attaches_++;
      LOG_I("USB", "Pump USB device connected");
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void UsbPumpLink::onNewDevice(usb_device_handle_t usbDev) {
  const usb_device_desc_t* desc = nullptr;
  if (instance && usb_host_get_device_descriptor(usbDev, &desc) == ESP_OK && desc) {
    instance->lastVidPid_ = (static_cast<uint32_t>(desc->idVendor) << 16) | desc->idProduct;
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
    if (!self->unusableDevice_) {
      LOG_W("USB", "CDC error %d", event->data.error);
      self->requestReopen("transfer error");
    }
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
  if (cdc_acm_host_line_coding_set(dev, &coding) == ESP_ERR_NOT_SUPPORTED) {
    // No CDC control interface: not a serial port. Keep it open so the
    // disconnect event tells us when it's unplugged, but don't use it.
    uint32_t vidPid = lastVidPid_;
    uint16_t vid = vidPid >> 16;
    uint16_t pid = vidPid & 0xFFFF;
    std::string problem;
    if (vid == kRaspberryPiVid && (pid == kRp2040BootPid || pid == kRp2350BootPid)) {
      problem = "The board on USB is in USB boot mode (BOOTSEL) and has no pump firmware running. "
                "Flash DrinkBotPump over USB, then plug it back in.";
    } else {
      char buf[120];
      snprintf(buf, sizeof(buf), "The USB device %04x:%04x is not a serial port, so it can't be the bridge board",
               vid, pid);
      problem = buf;
    }
    LOG_E("USB", "%s", problem.c_str());
    setProblem(problem);
    dev_ = dev;
    unusableDevice_ = true;
    reopenPending_ = false;
    return false;
  }
  // The RP2040 TinyUSB CDC stack may drop TX until DTR is asserted (§5.1)
  cdc_acm_host_set_control_line_state(dev, true, true);

  framer_.reset();
  {
    std::lock_guard<std::mutex> lines(linesMutex_);
    lines_.clear();
  }
  dev_ = dev;
  txTimeouts_ = 0;
  reopenPending_ = false;
  connected_ = true;
  return true;
}

void UsbPumpLink::close() {
  std::lock_guard<std::mutex> lock(devMutex_);
  connected_ = false;
  unusableDevice_ = false;
  setProblem("");
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
  esp_err_t err;
  {
    std::lock_guard<std::mutex> lock(devMutex_);
    if (!dev_) {
      return false;
    }
    err = cdc_acm_host_data_tx_blocking(dev_, reinterpret_cast<const uint8_t*>(out.data()), out.size(),
                                        kTxTimeoutMs);
  }
  if (err == ESP_OK) {
    txTimeouts_ = 0;
    return true;
  }
  LOG_W("USB", "TX failed: %s", esp_err_to_name(err));
  // A timeout can be a busy device; anything else means the handle is broken
  if (err != ESP_ERR_TIMEOUT || ++txTimeouts_ >= kMaxTxTimeouts) {
    requestReopen(esp_err_to_name(err));
  }
  return false;
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
