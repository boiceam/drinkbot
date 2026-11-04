#pragma once

#include <DNSServer.h>
#include <WiFi.h>

#include <string>
#include <vector>

#include "core/Config.h"
#include "core/UiModel.h"

namespace drinkbot {

// Wi-Fi (SPEC_v3 §9.2): tries the configured networks in order for 20 s,
// then falls back to an open AP with a captive DNS. Non-blocking: call
// tick() from the main loop. Settings are read once at boot (§7.5).
class NetManager {
 public:
  static constexpr uint32_t kConnectTotalMs = 20000;
  static constexpr uint32_t kMinPerNetworkMs = 6000;

  void begin(const Config& config);
  void tick();

  WifiMode mode() const { return mode_; }
  // True once an IP exists (STA connected or AP up): start the web server.
  bool hasIp() const { return apStarted_ || everConnected_; }
  bool apMode() const { return apStarted_; }
  std::string ssid() const;
  std::string ip() const;
  int rssi() const;
  const std::string& hostname() const { return hostname_; }

 private:
  void tryNetwork(size_t index);
  void onConnected();
  void startAccessPoint();
  void startMdns();

  std::vector<WifiNetwork> networks_;
  std::string hostname_;
  std::string apSsid_;
  WifiMode mode_ = WifiMode::Connecting;
  bool apStarted_ = false;
  bool everConnected_ = false;
  bool mdnsStarted_ = false;
  uint32_t startedAt_ = 0;
  uint32_t networkDeadline_ = 0;
  uint32_t perNetworkMs_ = 0;
  size_t networkIndex_ = 0;
  DNSServer dns_;
};

}  // namespace drinkbot
