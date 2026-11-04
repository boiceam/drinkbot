#include "hal/NetManager.h"

#include <ESPmDNS.h>

#include <algorithm>

#include "core/Log.h"

namespace drinkbot {

namespace {
const IPAddress kApIp(192, 168, 4, 1);
const IPAddress kApMask(255, 255, 255, 0);
constexpr uint16_t kDnsPort = 53;
}  // namespace

void NetManager::begin(const Config& config) {
  networks_ = config.networks;
  hostname_ = config.hostname;
  apSsid_ = config.apSsid;
  startedAt_ = millis();

  WiFi.persistent(false);  // settings come from config.json, not NVS
  WiFi.setHostname(hostname_.c_str());

  if (networks_.empty()) {
    LOG_I("NET", "No Wi-Fi networks configured");
    startAccessPoint();
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  perNetworkMs_ = std::max<uint32_t>(kConnectTotalMs / networks_.size(), kMinPerNetworkMs);
  tryNetwork(0);
}

void NetManager::tryNetwork(size_t index) {
  networkIndex_ = index;
  const WifiNetwork& n = networks_[index];
  LOG_I("NET", "Connecting to '%s' (%u of %u)", n.ssid.c_str(), static_cast<unsigned>(index + 1),
        static_cast<unsigned>(networks_.size()));
  WiFi.disconnect();
  WiFi.begin(n.ssid.c_str(), n.password.empty() ? nullptr : n.password.c_str());
  networkDeadline_ = millis() + perNetworkMs_;
}

void NetManager::tick() {
  if (apStarted_) {
    dns_.processNextRequest();
    return;
  }

  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && mode_ != WifiMode::Station) {
    onConnected();
    return;
  }
  if (!connected && mode_ == WifiMode::Station) {
    // §9.2: back to blinking while auto-reconnect runs; no AP fallback
    LOG_W("NET", "Wi-Fi connection lost; reconnecting");
    mode_ = WifiMode::Connecting;
    return;
  }
  if (connected || everConnected_) {
    return;
  }

  uint32_t now = millis();
  if (now - startedAt_ >= kConnectTotalMs) {
    LOG_W("NET", "No configured network joined within 20 s");
    startAccessPoint();
    return;
  }
  if (timeReached(now, networkDeadline_) && networkIndex_ + 1 < networks_.size()) {
    tryNetwork(networkIndex_ + 1);
  }
}

void NetManager::onConnected() {
  mode_ = WifiMode::Station;
  everConnected_ = true;
  LOG_I("NET", "Connected to '%s', IP %s, RSSI %d dBm", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
        WiFi.RSSI());
  startMdns();
}

void NetManager::startAccessPoint() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(kApIp, kApIp, kApMask);
  if (!WiFi.softAP(apSsid_.c_str())) {  // no password: open network
    LOG_E("NET", "Could not start access point '%s'", apSsid_.c_str());
  }
  dns_.setErrorReplyCode(DNSReplyCode::NoError);
  dns_.start(kDnsPort, "*", kApIp);  // captive DNS: every name -> 192.168.4.1
  apStarted_ = true;
  mode_ = WifiMode::AccessPoint;
  LOG_I("NET", "Open access point '%s' started at %s", apSsid_.c_str(), kApIp.toString().c_str());
  startMdns();
}

void NetManager::startMdns() {
  if (mdnsStarted_) {
    return;
  }
  if (MDNS.begin(hostname_.c_str())) {
    MDNS.addService("http", "tcp", 80);
    mdnsStarted_ = true;
    LOG_I("NET", "mDNS: http://%s.local", hostname_.c_str());
  } else {
    LOG_W("NET", "mDNS failed to start");
  }
}

std::string NetManager::ssid() const {
  return apStarted_ ? apSsid_ : std::string(WiFi.SSID().c_str());
}

std::string NetManager::ip() const {
  if (apStarted_) {
    return kApIp.toString().c_str();
  }
  return mode_ == WifiMode::Station ? std::string(WiFi.localIP().toString().c_str()) : std::string();
}

int NetManager::rssi() const {
  return mode_ == WifiMode::Station ? WiFi.RSSI() : 0;
}

}  // namespace drinkbot
