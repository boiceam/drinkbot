#pragma once

#include <array>
#include <string>
#include <vector>

#include "core/App.h"
#include "core/Log.h"
#include "core/UiModel.h"

namespace drinkbot {

// JSON messages of the web app WebSocket (SPEC_v3 §14). Pure functions,
// so the exact wire format is unit-tested.

constexpr const char* kFirmwareVersion = "0.1.0";

struct WifiInfo {
  WifiMode mode = WifiMode::Connecting;
  std::string ssid;
  std::string ip;
  int rssi = 0;
};

// --- Server -> client ---------------------------------------------------
std::string helloJson(bool simulator, const std::string& hostname, const std::string& buildDate);
std::string stateJson(const App& app, const std::array<LedCell, kLedCount>& leds, const WifiInfo& wifi);
std::string pumpsJson(const App& app);
std::string configMessageJson(const std::string& configJson);
std::string logJson(const LogEntry& entry);
std::string logHistoryJson(const std::vector<LogEntry>& entries);
std::string dispenseProgressJson(const DispenseProgress& p);
std::string bulkProgressJson(const BulkProgress& p);
std::string otaJson(int percent, const char* status, const std::string& message = "");
std::string ackJson(const std::string& reqId, const CommandResult& result);
std::string errorJson(const std::string& reqId, const char* code, const std::string& message,
                      const std::vector<ConfigError>& details = {});
std::string infoJson(bool simulator, const std::string& buildDate, uint32_t uptimeMs, uint32_t freeHeap,
                     const WifiInfo& wifi);
// Body of POST /api/config replies: {"ok":true} or {"ok":false,"message":..,"errors":[..]}
std::string configUploadReplyJson(const CommandResult& result);

// --- Client -> server ---------------------------------------------------
struct ClientMessage {
  enum class Kind : uint8_t { Command, GetConfig, SetLogLevel, Invalid };
  Kind kind = Kind::Invalid;
  std::string reqId;
  Command command;     // Kind::Command
  LogLevel logLevel = LogLevel::Info;  // Kind::SetLogLevel
  std::string error;   // Kind::Invalid
};

ClientMessage parseClientMessage(const std::string& text);

}  // namespace drinkbot
