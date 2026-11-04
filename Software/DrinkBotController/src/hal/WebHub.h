#pragma once

#include <ESPAsyncWebServer.h>

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <string>

#include "core/App.h"
#include "core/Log.h"
#include "core/UiController.h"
#include "core/WsProtocol.h"
#include "hal/NetManager.h"

namespace drinkbot {

// Web server, OTA and WebSocket hub (SPEC_v3 §13, §14).
//
// Threading: HTTP and WebSocket callbacks run on the AsyncTCP task. They
// only read mutex-guarded snapshots and post() commands; everything that
// touches the App (listener callbacks, tick()) runs on the App task.
class WebHub : public IAppListener, public ILogSink {
 public:
  static constexpr size_t kMaxClients = 4;
  static constexpr uint32_t kStateCheckMs = 100;
  static constexpr uint32_t kLogMsgPerSecond = 50;
  static constexpr size_t kLogQueueMax = 200;
  static constexpr size_t kMaxMessageBytes = 40 * 1024;
  static constexpr uint32_t kOtaRebootDelayMs = 1000;

  WebHub(App& app, UiController& ui, NetManager& net, const std::string& buildDate);

  // Starts the server; call once an IP exists.
  void begin();
  bool started() const { return started_; }
  // App task: state/LED/Wi-Fi changes, log fan-out, OTA reboot.
  void tick();

  // IAppListener (App task)
  void onStateChanged() override;
  void onPumpsChanged() override;
  void onConfigChanged() override;
  void onDispenseProgress(const DispenseProgress& p) override;
  void onBulkProgress(const BulkProgress& p) override;

  // ILogSink (any task)
  void write(const LogEntry& entry) override;

 private:
  void routeStatic();
  void routeApi();
  void routeOta();
  void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client, AwsEventType type, void* arg,
                 uint8_t* data, size_t len);
  void onWsMessage(uint32_t clientId, const std::string& text);
  void sendSnapshot(AsyncWebSocketClient* client);
  void broadcast(const std::string& msg);
  void setSnapshot(std::string& slot, const std::string& value);
  std::string snapshot(const std::string& slot);
  WifiInfo wifiInfo() const;
  void flushLogs();

  App& app_;
  UiController& ui_;
  NetManager& net_;
  std::string buildDate_;
  AsyncWebServer server_{80};
  AsyncWebSocket ws_{"/ws"};
  bool started_ = false;

  std::mutex snapMutex_;
  std::string helloSnap_;
  std::string stateSnap_;
  std::string pumpsSnap_;
  std::string configSnap_;
  WifiInfo wifiSnap_;
  uint32_t nextStateCheck_ = 0;

  // Busy flag for the AsyncTCP side (OTA and config upload are refused)
  std::atomic<bool> busy_{true};

  // WebSocket frame reassembly, per client (AsyncTCP task only)
  std::map<uint32_t, std::string> partial_;

  // Log fan-out
  std::mutex logMutex_;
  std::deque<LogEntry> logQueue_;
  uint32_t logsDropped_ = 0;
  float logTokens_ = kLogMsgPerSecond;
  uint32_t lastLogFlush_ = 0;

  // OTA (AsyncTCP task), reboot picked up by tick()
  bool otaRejected_ = false;
  bool otaFailed_ = false;
  std::string otaError_;
  int otaLastPercent_ = -1;
  std::atomic<bool> otaRebootPending_{false};
  uint32_t otaRebootAt_ = 0;
};

}  // namespace drinkbot
