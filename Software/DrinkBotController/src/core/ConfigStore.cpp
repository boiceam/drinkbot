#include "core/ConfigStore.h"

#include "core/Config.h"
#include "core/Log.h"

namespace drinkbot {

const char* ConfigStore::sourceName(Source source) {
  switch (source) {
    case Source::Primary: return "config.json";
    case Source::Backup: return "config.bak";
    case Source::FactoryDefault: return "factory default";
  }
  return "?";
}

bool ConfigStore::readValid(const char* path, std::string& json) {
  std::string text;
  if (!files_.exists(path) || !files_.read(path, text)) {
    return false;
  }
  if (text.size() > kMaxBytes) {
    LOG_E("CFG", "%s is larger than %u bytes", path, static_cast<unsigned>(kMaxBytes));
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, text) != DeserializationError::Ok) {
    LOG_E("CFG", "%s is not valid JSON", path);
    return false;
  }
  migrateConfig(doc);
  Config cfg;
  auto errors = parseConfig(doc.as<JsonVariantConst>(), cfg);
  if (!errors.empty()) {
    LOG_E("CFG", "%s is invalid: %s %s", path, errors[0].path.c_str(), errors[0].message.c_str());
    return false;
  }
  json = text;  // the App migrates (and re-saves) it if needed
  return true;
}

ConfigStore::Source ConfigStore::load(std::string& json) {
  if (readValid(kPrimary, json)) {
    LOG_I("CFG", "Config loaded from %s", kPrimary);
    return Source::Primary;
  }
  if (readValid(kBackup, json)) {
    LOG_E("CFG", "Using backup config %s", kBackup);
    return Source::Backup;
  }
  LOG_E("CFG", "No usable stored config; using the factory default");
  JsonDocument doc;
  makeFactoryDefaultConfig(doc);
  json.clear();
  serializeJson(doc, json);
  return Source::FactoryDefault;
}

bool ConfigStore::save(const std::string& json) {
  if (json.size() > kMaxBytes) {
    LOG_E("CFG", "Config is larger than %u bytes", static_cast<unsigned>(kMaxBytes));
    return false;
  }
  if (files_.exists(kTemp)) {
    files_.remove(kTemp);
  }
  std::string check;
  if (!files_.write(kTemp, json) || !files_.read(kTemp, check) || check != json) {
    LOG_E("CFG", "Writing %s failed", kTemp);
    files_.remove(kTemp);
    return false;
  }
  if (files_.exists(kPrimary)) {
    if (files_.exists(kBackup)) {
      files_.remove(kBackup);
    }
    if (!files_.rename(kPrimary, kBackup)) {
      LOG_E("CFG", "Could not move %s to %s", kPrimary, kBackup);
      return false;
    }
  }
  if (!files_.rename(kTemp, kPrimary)) {
    LOG_E("CFG", "Could not move %s to %s", kTemp, kPrimary);
    return false;
  }
  LOG_D("CFG", "Config written (%u bytes)", static_cast<unsigned>(json.size()));
  return true;
}

}  // namespace drinkbot
