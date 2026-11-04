#include "hal/WebHub.h"

#include <Arduino.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <freertos/semphr.h>

#include <memory>

#include "core/ConfigStore.h"
#include "generated/web_assets.h"

namespace drinkbot {

namespace {

constexpr uint16_t kChipIdEsp32S3 = 9;  // esp_image_header_t::chip_id
constexpr uint32_t kConfigReplyWaitMs = 3000;

// Firmware image header check before anything is written (§13)
bool looksLikeEsp32S3Image(const uint8_t* data, size_t len, std::string& why) {
  if (len < 24) {
    why = "File is too small to be a firmware image";
    return false;
  }
  if (data[0] != 0xE9) {
    why = "Not an ESP32 firmware image (.bin)";
    return false;
  }
  uint16_t chip = static_cast<uint16_t>(data[12] | (data[13] << 8));
  if (chip != kChipIdEsp32S3) {
    why = "Firmware is for a different chip (id " + std::to_string(chip) + "), not the ESP32-S3";
    return false;
  }
  return true;
}

}  // namespace

WebHub::WebHub(App& app, UiController& ui, NetManager& net, const std::string& buildDate)
    : app_(app), ui_(ui), net_(net), buildDate_(buildDate) {}

void WebHub::begin() {
  if (started_) {
    return;
  }
  setSnapshot(helloSnap_, helloJson(app_.simulator(), app_.config().hostname, buildDate_));
  setSnapshot(configSnap_, configMessageJson(app_.configJson()));
  setSnapshot(pumpsSnap_, pumpsJson(app_));
  {
    std::lock_guard<std::mutex> lock(snapMutex_);
    wifiSnap_ = wifiInfo();
  }
  setSnapshot(stateSnap_, stateJson(app_, ui_.leds(), wifiInfo()));

  ws_.onEvent([this](AsyncWebSocket* s, AsyncWebSocketClient* c, AwsEventType t, void* arg, uint8_t* d, size_t l) {
    onWsEvent(s, c, t, arg, d, l);
  });
  server_.addHandler(&ws_);
  routeStatic();
  routeApi();
  routeOta();

  server_.onNotFound([this](AsyncWebServerRequest* req) {
    if (net_.apMode()) {
      req->redirect("http://192.168.4.1/");  // captive portal
    } else {
      req->send(404, "text/plain", "Not found");
    }
  });

  server_.begin();
  started_ = true;
  LOG_I("WEB", "Web server started on port 80 (%u embedded files)", static_cast<unsigned>(kWebAssetCount));

  // The new image has proved it can serve the UI: keep it (§13 rollback)
  esp_ota_mark_app_valid_cancel_rollback();
}

void WebHub::routeStatic() {
  for (size_t i = 0; i < kWebAssetCount; i++) {
    const WebAsset* asset = &kWebAssets[i];
    auto handler = [asset](AsyncWebServerRequest* req) {
      if (req->hasHeader("If-None-Match") && req->header("If-None-Match") == kWebAssetsEtag) {
        req->send(304);
        return;
      }
      AsyncWebServerResponse* res = req->beginResponse(200, asset->mime, asset->data, asset->length);
      res->addHeader("Content-Encoding", "gzip");
      res->addHeader("Cache-Control", "no-cache");
      res->addHeader("ETag", kWebAssetsEtag);
      req->send(res);
    };
    std::string path = std::string("/") + asset->path;
    server_.on(path.c_str(), HTTP_GET, handler);
    if (path == "/index.html") {
      server_.on("/", HTTP_GET, handler);
    }
  }
}

void WebHub::routeApi() {
  server_.on("/api/config", HTTP_GET, [this](AsyncWebServerRequest* req) {
    std::string msg = snapshot(configSnap_);
    // Unwrap {"type":"config","config":<json>}
    const std::string prefix = "{\"type\":\"config\",\"config\":";
    std::string json = msg.substr(prefix.size(), msg.size() - prefix.size() - 1);
    AsyncWebServerResponse* res = req->beginResponse(200, "application/json", json.c_str());
    res->addHeader("Content-Disposition", "attachment; filename=drinkbot-config.json");
    req->send(res);
  });

  server_.on("/api/info", HTTP_GET, [this](AsyncWebServerRequest* req) {
    WifiInfo wifi;
    {
      std::lock_guard<std::mutex> lock(snapMutex_);
      wifi = wifiSnap_;
    }
    req->send(200, "application/json",
              infoJson(app_.simulator(), buildDate_, millis(), ESP.getFreeHeap(), wifi).c_str());
  });

  // POST /api/config: import a whole config (body <= 32 KB)
  server_.on(
      "/api/config", HTTP_POST,
      [this](AsyncWebServerRequest* req) {
        // The body handler collected it in a malloc() buffer (freed by the library too)
        std::string text;
        if (req->_tempObject) {
          text.assign(static_cast<const char*>(req->_tempObject), req->contentLength());
          free(req->_tempObject);
          req->_tempObject = nullptr;
        } else if (req->contentLength() > ConfigStore::kMaxBytes) {
          req->send(413, "application/json",
                    "{\"ok\":false,\"code\":\"INVALID\",\"message\":\"Config is larger than 32 KB\"}");
          return;
        }
        if (busy_) {
          req->send(409, "application/json",
                    "{\"ok\":false,\"code\":\"BUSY\",\"message\":\"The controller is busy\"}");
          return;
        }

        // The App answers on its own task; wait here for its reply
        struct Wait {
          SemaphoreHandle_t done = xSemaphoreCreateBinary();
          CommandResult result;
          ~Wait() { vSemaphoreDelete(done); }
        };
        auto wait = std::make_shared<Wait>();
        Command cmd;
        cmd.type = Command::Type::SaveConfig;
        cmd.text = text;
        cmd.source = std::string("import ") + req->client()->remoteIP().toString().c_str();
        cmd.reply = [wait](const CommandResult& r) {
          wait->result = r;
          xSemaphoreGive(wait->done);
        };
        app_.post(std::move(cmd));
        if (xSemaphoreTake(wait->done, pdMS_TO_TICKS(kConfigReplyWaitMs)) != pdTRUE) {
          req->send(503, "application/json", "{\"ok\":false,\"code\":\"INTERNAL\",\"message\":\"No reply\"}");
          return;
        }
        int status = wait->result.ok ? 200 : (wait->result.code == ErrCode::kBusy ? 409 : 400);
        req->send(status, "application/json", configUploadReplyJson(wait->result).c_str());
      },
      nullptr,
      [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
        if (index == 0 && total <= ConfigStore::kMaxBytes && !req->_tempObject) {
          req->_tempObject = malloc(total + 1);
        }
        if (req->_tempObject && index + len <= total) {
          memcpy(static_cast<char*>(req->_tempObject) + index, data, len);
        }
      });
}

void WebHub::routeOta() {
  server_.on(
      "/api/ota", HTTP_POST,
      [this](AsyncWebServerRequest* req) {
        if (otaRejected_ || otaFailed_ || Update.hasError()) {
          std::string why = otaError_.empty() ? Update.errorString() : otaError_;
          int status = otaRejected_ && busy_ ? 409 : 400;
          broadcast(otaJson(otaLastPercent_ < 0 ? 0 : otaLastPercent_, "FAILED", why));
          LOG_E("OTA", "Update failed: %s", why.c_str());
          JsonDocument doc;
          doc["ok"] = false;
          doc["message"] = why;
          std::string body;
          serializeJson(doc, body);
          req->send(status, "application/json", body.c_str());
          return;
        }
        LOG_W("OTA", "Update written; rebooting");
        broadcast(otaJson(100, "DONE", "Rebooting into the new firmware"));
        req->send(200, "application/json", "{\"ok\":true}");
        otaRebootPending_ = true;
      },
      [this](AsyncWebServerRequest* req, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
        if (index == 0) {
          otaRejected_ = false;
          otaFailed_ = false;
          otaError_.clear();
          otaLastPercent_ = -1;
          if (busy_) {
            otaRejected_ = true;
            otaError_ = "The controller is busy; update when it is idle";
            return;
          }
          std::string why;
          if (!looksLikeEsp32S3Image(data, len, why)) {
            otaRejected_ = true;
            otaError_ = why;
            return;
          }
          LOG_I("OTA", "Receiving %s from %s", filename.c_str(), req->client()->remoteIP().toString().c_str());
          if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            otaFailed_ = true;
            otaError_ = Update.errorString();
            return;
          }
        }
        if (otaRejected_ || otaFailed_) {
          return;
        }
        if (Update.write(data, len) != len) {
          otaFailed_ = true;
          otaError_ = Update.errorString();
          Update.abort();
          return;
        }
        size_t total = req->contentLength();
        int percent = total ? static_cast<int>((index + len) * 100 / total) : 0;
        if (percent >= otaLastPercent_ + 5 || final) {
          otaLastPercent_ = percent;
          broadcast(otaJson(final ? 100 : percent, "WRITING"));
        }
        if (final && !Update.end(true)) {
          otaFailed_ = true;
          otaError_ = Update.errorString();
        }
      });
}

void WebHub::onWsEvent(AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type, void* arg,
                       uint8_t* data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      if (ws_.count() > kMaxClients) {
        LOG_W("WEB", "Refusing WebSocket client %s: %u already connected",
              client->remoteIP().toString().c_str(), static_cast<unsigned>(kMaxClients));
        client->close(1013, "Too many clients");
        return;
      }
      LOG_I("WEB", "Browser connected from %s", client->remoteIP().toString().c_str());
      sendSnapshot(client);
      return;
    case WS_EVT_DISCONNECT:
      partial_.erase(client->id());
      return;
    case WS_EVT_DATA: {
      auto* info = static_cast<AwsFrameInfo*>(arg);
      std::string& buf = partial_[client->id()];
      if (info->index == 0 && info->num == 0) {
        buf.clear();
      }
      if (buf.size() + len > kMaxMessageBytes) {
        buf.clear();
        client->text(errorJson("", ErrCode::kInvalid, "Message too large").c_str());
        return;
      }
      buf.append(reinterpret_cast<const char*>(data), len);
      if (info->final && info->index + len == info->len) {
        std::string text = std::move(buf);
        partial_.erase(client->id());
        onWsMessage(client->id(), text);
      }
      return;
    }
    default:
      return;
  }
}

void WebHub::onWsMessage(uint32_t clientId, const std::string& text) {
  ClientMessage m = parseClientMessage(text);
  AsyncWebSocket* ws = &ws_;
  switch (m.kind) {
    case ClientMessage::Kind::Invalid:
      ws->text(clientId, errorJson(m.reqId, ErrCode::kInvalid, m.error).c_str());
      return;
    case ClientMessage::Kind::GetConfig:
      ws->text(clientId, snapshot(configSnap_).c_str());
      ws->text(clientId, ackJson(m.reqId, CommandResult()).c_str());
      return;
    case ClientMessage::Kind::SetLogLevel:
      logger().setSinkLevel(this, m.logLevel);
      LOG_I("WEB", "Web log level set to %s", logLevelName(m.logLevel));
      ws->text(clientId, ackJson(m.reqId, CommandResult()).c_str());
      return;
    case ClientMessage::Kind::Command: {
      std::string reqId = m.reqId;
      AsyncWebSocketClient* c = ws->client(clientId);
      m.command.source = std::string("web ") + (c ? c->remoteIP().toString().c_str() : "?");
      m.command.reply = [ws, clientId, reqId](const CommandResult& r) {
        std::string out = r.ok ? ackJson(reqId, r) : errorJson(reqId, r.code.c_str(), r.message, r.configErrors);
        if (ws->client(clientId)) {
          ws->text(clientId, out.c_str());
        }
      };
      app_.post(std::move(m.command));
      return;
    }
  }
}

void WebHub::sendSnapshot(AsyncWebSocketClient* client) {
  client->text(snapshot(helloSnap_).c_str());
  client->text(snapshot(stateSnap_).c_str());
  client->text(snapshot(pumpsSnap_).c_str());
  client->text(snapshot(configSnap_).c_str());
  client->text(logHistoryJson(logger().history()).c_str());
}

void WebHub::broadcast(const std::string& msg) {
  if (started_ && ws_.count() > 0) {
    ws_.textAll(msg.c_str());
  }
}

void WebHub::setSnapshot(std::string& slot, const std::string& value) {
  std::lock_guard<std::mutex> lock(snapMutex_);
  slot = value;
}

std::string WebHub::snapshot(const std::string& slot) {
  std::lock_guard<std::mutex> lock(snapMutex_);
  return slot;
}

WifiInfo WebHub::wifiInfo() const {
  WifiInfo w;
  w.mode = net_.mode();
  w.ssid = net_.ssid();
  w.ip = net_.ip();
  w.rssi = net_.rssi();
  return w;
}

// --- App task ---------------------------------------------------------

void WebHub::tick() {
  SysState st = app_.system().state();
  busy_ = st != SysState::Ready && st != SysState::Error;
  if (!started_) {
    return;
  }
  uint32_t now = millis();

  // State covers LEDs and Wi-Fi too, which change without an App event
  if (timeReached(now, nextStateCheck_)) {
    nextStateCheck_ = now + kStateCheckMs;
    WifiInfo wifi = wifiInfo();
    {
      std::lock_guard<std::mutex> lock(snapMutex_);
      wifiSnap_ = wifi;
    }
    std::string s = stateJson(app_, ui_.leds(), wifi);
    if (s != snapshot(stateSnap_)) {
      setSnapshot(stateSnap_, s);
      broadcast(s);
    }
  }

  flushLogs();
  ws_.cleanupClients(kMaxClients);

  if (otaRebootPending_ && otaRebootAt_ == 0) {
    otaRebootAt_ = now + kOtaRebootDelayMs;
  }
  if (otaRebootAt_ != 0 && timeReached(now, otaRebootAt_)) {
    ESP.restart();
  }
}

void WebHub::onStateChanged() {
  nextStateCheck_ = millis();  // send on the next tick
}

void WebHub::onPumpsChanged() {
  std::string s = pumpsJson(app_);
  setSnapshot(pumpsSnap_, s);
  broadcast(s);
}

void WebHub::onConfigChanged() {
  std::string s = configMessageJson(app_.configJson());
  setSnapshot(configSnap_, s);
  broadcast(s);
}

void WebHub::onDispenseProgress(const DispenseProgress& p) {
  broadcast(dispenseProgressJson(p));
}

void WebHub::onBulkProgress(const BulkProgress& p) {
  broadcast(bulkProgressJson(p));
}

void WebHub::write(const LogEntry& entry) {
  std::lock_guard<std::mutex> lock(logMutex_);
  if (logQueue_.size() >= kLogQueueMax) {
    logQueue_.pop_front();
    logsDropped_++;
  }
  logQueue_.push_back(entry);
}

void WebHub::flushLogs() {
  uint32_t now = millis();
  logTokens_ += (now - lastLogFlush_) * kLogMsgPerSecond / 1000.0f;
  if (logTokens_ > kLogMsgPerSecond) {
    logTokens_ = kLogMsgPerSecond;
  }
  lastLogFlush_ = now;

  std::deque<LogEntry> batch;
  uint32_t dropped = 0;
  {
    std::lock_guard<std::mutex> lock(logMutex_);
    while (!logQueue_.empty() && logTokens_ >= 1) {
      batch.push_back(std::move(logQueue_.front()));
      logQueue_.pop_front();
      logTokens_ -= 1;
    }
    // Throttled: collapse what can't be sent this second (§12.3)
    if (!logQueue_.empty() && logTokens_ < 1) {
      dropped = logsDropped_ + logQueue_.size();
      logQueue_.clear();
      logsDropped_ = 0;
    } else if (logsDropped_ > 0) {
      dropped = logsDropped_;
      logsDropped_ = 0;
    }
  }
  if (ws_.count() == 0) {
    return;  // new clients get the history instead
  }
  for (const auto& e : batch) {
    ws_.textAll(logJson(e).c_str());
  }
  if (dropped > 0) {
    LogEntry notice;
    notice.t = now;
    notice.level = LogLevel::Warn;
    notice.tag = "WEB";
    notice.msg = "... " + std::to_string(dropped) + " lines dropped";
    ws_.textAll(logJson(notice).c_str());
  }
}

}  // namespace drinkbot
