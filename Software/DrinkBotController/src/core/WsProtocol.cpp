#include "core/WsProtocol.h"

#include <cmath>

namespace drinkbot {

namespace {

std::string serialize(const JsonDocument& doc) {
  std::string out;
  serializeJson(doc, out);
  return out;
}

void putColor(JsonObject o, const Rgb& c) {
  o["r"] = c.r;
  o["g"] = c.g;
  o["b"] = c.b;
}

// Floats rounded to 0.1 so the UI doesn't show 62.4999
double tenth(float v) {
  return std::round(static_cast<double>(v) * 10.0) / 10.0;
}

void putErrors(JsonArray arr, const std::vector<ConfigError>& errors) {
  for (const auto& e : errors) {
    JsonObject o = arr.add<JsonObject>();
    o["path"] = e.path;
    o["message"] = e.message;
  }
}

void putLogEntry(JsonObject o, const LogEntry& e) {
  o["t"] = e.t;
  o["level"] = logLevelName(e.level);
  o["tag"] = e.tag;
  o["msg"] = e.msg;
}

}  // namespace

std::string helloJson(bool simulator, const std::string& hostname, const std::string& buildDate) {
  JsonDocument doc;
  doc["type"] = "hello";
  doc["fwVersion"] = kFirmwareVersion;
  doc["buildDate"] = buildDate;
  doc["simulator"] = simulator;
  doc["hostname"] = hostname;
  return serialize(doc);
}

std::string stateJson(const App& app, const std::array<LedCell, kLedCount>& leds, const WifiInfo& wifi) {
  JsonDocument doc;
  doc["type"] = "state";
  doc["system"] = sysStateName(app.system().state());
  JsonArray errors = doc["errors"].to<JsonArray>();
  for (const auto& e : app.system().errors()) {
    JsonObject o = errors.add<JsonObject>();
    o["code"] = errorCodeName(e.code);
    if (e.pumpId != 0) {
      o["pumpId"] = e.pumpId;
    }
    o["message"] = e.message;
    o["latched"] = e.latched();
  }
  doc["activeRecipeId"] = app.activeRecipeId();
  doc["activeVolumeMl"] = tenth(app.activeVolumeMl());
  JsonObject w = doc["wifi"].to<JsonObject>();
  w["mode"] = wifiModeName(wifi.mode);
  w["ssid"] = wifi.ssid;
  w["ip"] = wifi.ip;
  w["rssi"] = wifi.rssi;
  JsonArray ledArr = doc["leds"].to<JsonArray>();
  for (const auto& l : leds) {
    JsonObject o = ledArr.add<JsonObject>();
    putColor(o, l.mode == LedMode::Off ? Rgb{} : l.color);
    o["mode"] = ledModeName(l.mode);
  }
  return serialize(doc);
}

std::string pumpsJson(const App& app) {
  JsonDocument doc;
  doc["type"] = "pumps";
  JsonArray arr = doc["pumps"].to<JsonArray>();
  for (const auto& p : app.pumps().pumps()) {
    JsonObject o = arr.add<JsonObject>();
    o["pumpId"] = p.pumpId;
    o["liquid"] = p.liquid;
    o["present"] = p.present;
    o["fwVersion"] = p.fwVersion;
    o["isBridge"] = p.isBridge;
    if (p.hasWeight) {
      o["weightG"] = tenth(p.weightG);
      o["levelMl"] = tenth(p.levelMl);
    } else {
      o["weightG"] = nullptr;
      o["levelMl"] = nullptr;
    }
    o["levelState"] = levelStateName(p.levelState);
    o["activity"] = pumpActivityName(p.activity);
    JsonObject led = o["led"].to<JsonObject>();
    putColor(led, p.led.color);
    led["mode"] = p.led.mode;
    led["brightness"] = p.led.brightness;
  }
  return serialize(doc);
}

std::string configMessageJson(const std::string& configJson) {
  // The stored JSON is embedded as-is so unknown keys reach the editor too
  return std::string("{\"type\":\"config\",\"config\":") + configJson + "}";
}

std::string logJson(const LogEntry& entry) {
  JsonDocument doc;
  doc["type"] = "log";
  putLogEntry(doc.to<JsonObject>(), entry);
  doc["type"] = "log";
  return serialize(doc);
}

std::string logHistoryJson(const std::vector<LogEntry>& entries) {
  JsonDocument doc;
  doc["type"] = "logHistory";
  JsonArray lines = doc["lines"].to<JsonArray>();
  for (const auto& e : entries) {
    putLogEntry(lines.add<JsonObject>(), e);
  }
  return serialize(doc);
}

std::string dispenseProgressJson(const DispenseProgress& p) {
  JsonDocument doc;
  doc["type"] = "dispenseProgress";
  doc["recipeId"] = p.recipeId;
  doc["volumeMl"] = tenth(p.volumeMl);
  doc["step"] = p.step;
  doc["steps"] = p.steps;
  if (!p.liquid.empty()) {
    doc["liquid"] = p.liquid;
    doc["pumpId"] = p.pumpId;
    doc["targetMl"] = tenth(p.targetMl);
  }
  if (p.hasActual) {
    doc["actualMl"] = tenth(p.actualMl);
  }
  doc["status"] = dispenseStatusName(p.status);
  if (!p.reason.empty()) {
    doc["reason"] = p.reason;
  }
  return serialize(doc);
}

std::string bulkProgressJson(const BulkProgress& p) {
  JsonDocument doc;
  doc["type"] = "bulkProgress";
  doc["op"] = bulkOpName(p.op);
  doc["step"] = p.step;
  doc["steps"] = p.steps;
  if (p.pumpId != 0) {
    doc["pumpId"] = p.pumpId;
    doc["liquid"] = p.liquid;
    doc["durationMs"] = p.durationMs;
  }
  doc["estimatedTotalMs"] = p.estimatedTotalMs;
  doc["status"] = bulkStatusName(p.status);
  if (!p.reason.empty()) {
    doc["reason"] = p.reason;
  }
  return serialize(doc);
}

std::string otaJson(int percent, const char* status, const std::string& message) {
  JsonDocument doc;
  doc["type"] = "ota";
  doc["percent"] = percent;
  doc["status"] = status;
  if (!message.empty()) {
    doc["message"] = message;
  }
  return serialize(doc);
}

std::string ackJson(const std::string& reqId, const CommandResult& result) {
  JsonDocument doc;
  doc["type"] = "ack";
  doc["reqId"] = reqId;
  if (!result.lines.empty() || !result.message.empty()) {
    JsonObject data = doc["data"].to<JsonObject>();
    if (!result.lines.empty()) {
      JsonArray lines = data["lines"].to<JsonArray>();
      for (const auto& l : result.lines) {
        lines.add(l);
      }
    }
    if (!result.message.empty()) {
      data["message"] = result.message;
    }
  }
  return serialize(doc);
}

std::string errorJson(const std::string& reqId, const char* code, const std::string& message,
                      const std::vector<ConfigError>& details) {
  JsonDocument doc;
  doc["type"] = "error";
  if (!reqId.empty()) {
    doc["reqId"] = reqId;
  }
  doc["code"] = code;
  doc["message"] = message;
  if (!details.empty()) {
    putErrors(doc["details"].to<JsonArray>(), details);
  }
  return serialize(doc);
}

std::string infoJson(bool simulator, const std::string& buildDate, uint32_t uptimeMs, uint32_t freeHeap,
                     const WifiInfo& wifi) {
  JsonDocument doc;
  doc["fwVersion"] = kFirmwareVersion;
  doc["buildDate"] = buildDate;
  doc["uptimeMs"] = uptimeMs;
  doc["freeHeap"] = freeHeap;
  doc["simulator"] = simulator;
  doc["ip"] = wifi.ip;
  doc["wifiMode"] = wifiModeName(wifi.mode);
  return serialize(doc);
}

std::string configUploadReplyJson(const CommandResult& result) {
  JsonDocument doc;
  doc["ok"] = result.ok;
  if (!result.ok) {
    doc["code"] = result.code;
    doc["message"] = result.message;
    putErrors(doc["errors"].to<JsonArray>(), result.configErrors);
  }
  return serialize(doc);
}

ClientMessage parseClientMessage(const std::string& text) {
  ClientMessage m;
  JsonDocument doc;
  if (deserializeJson(doc, text) != DeserializationError::Ok || !doc.is<JsonObject>()) {
    m.error = "Message is not a JSON object";
    return m;
  }
  m.reqId = doc["reqId"] | "";
  std::string type = doc["type"] | "";

  auto number = [&](const char* key) {
    JsonVariantConst v = doc[key];
    return v.is<double>() ? static_cast<float>(v.as<double>()) : NAN;
  };
  auto pumpId = [&]() { return doc["pumpId"].is<int>() ? doc["pumpId"].as<int>() : 0; };

  using T = Command::Type;
  Command& c = m.command;
  m.kind = ClientMessage::Kind::Command;

  if (type == "getConfig") {
    m.kind = ClientMessage::Kind::GetConfig;
  } else if (type == "setLogLevel") {
    m.kind = ClientMessage::Kind::SetLogLevel;
    if (!parseLogLevel(doc["level"] | "", m.logLevel)) {
      m.kind = ClientMessage::Kind::Invalid;
      m.error = "Unknown log level";
    }
  } else if (type == "setRecipe") {
    c.type = T::SetRecipe;
    c.text = doc["recipeId"] | "";
  } else if (type == "setVolume") {
    c.type = T::SetVolume;
    c.number = number("volumeMl");
  } else if (type == "dispense") {
    c.type = T::Dispense;
    c.text = doc["recipeId"] | "";
    c.number = number("volumeMl");
  } else if (type == "prime" || type == "purge") {
    c.type = type == "prime" ? T::Prime : T::Purge;
    c.pumpId = pumpId();
  } else if (type == "pumpStop") {
    c.type = T::PumpStop;
    c.pumpId = pumpId();
  } else if (type == "primeAll") {
    c.type = T::PrimeAll;
  } else if (type == "purgeAll") {
    c.type = T::PurgeAll;
  } else if (type == "estop") {
    c.type = T::Estop;
  } else if (type == "clearErrors") {
    c.type = T::ClearErrors;
  } else if (type == "rescan") {
    c.type = T::Rescan;
  } else if (type == "reboot") {
    c.type = T::Reboot;
    c.flag = doc["force"] | false;
  } else if (type == "saveConfig") {
    c.type = T::SaveConfig;
    if (!doc["config"].is<JsonObjectConst>()) {
      m.kind = ClientMessage::Kind::Invalid;
      m.error = "saveConfig needs a config object";
    } else {
      serializeJson(doc["config"], c.text);
    }
  } else if (type == "pumpTare") {
    c.type = T::PumpTare;
    c.pumpId = pumpId();
  } else if (type == "pumpCalibrate") {
    c.type = T::PumpCalibrate;
    c.pumpId = pumpId();
    c.number = number("grams");
  } else if (type == "pumpSetId") {
    c.type = T::PumpSetId;
    c.pumpId = pumpId();
    c.newId = doc["newId"] | 0;
  } else if (type == "pumpLedTest") {
    c.type = T::PumpLedTest;
    c.pumpId = pumpId();
  } else if (type == "pumpRaw") {
    c.type = T::PumpRaw;
    c.pumpId = pumpId();
    c.text = doc["command"] | "";
  } else if (type == "simFault") {
    c.type = T::SimFault;
    c.pumpId = pumpId();
    std::string fault = doc["fault"] | "";
    if (fault == "missing") {
      c.text = (doc["value"] | true) ? "missing" : "present";
    } else {
      c.text = fault;
    }
    c.number = number("weightG");
  } else {
    m.kind = ClientMessage::Kind::Invalid;
    m.error = "Unknown message type '" + type + "'";
  }
  return m;
}

}  // namespace drinkbot
