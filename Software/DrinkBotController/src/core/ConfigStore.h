#pragma once

#include <string>

namespace drinkbot {

// Minimal file access, so the save/load rules can be tested without flash.
class IFileStore {
 public:
  virtual ~IFileStore() = default;
  virtual bool exists(const std::string& path) = 0;
  virtual bool read(const std::string& path, std::string& out) = 0;
  virtual bool write(const std::string& path, const std::string& data) = 0;
  virtual bool remove(const std::string& path) = 0;
  // Fails if `to` exists; callers remove it first.
  virtual bool rename(const std::string& from, const std::string& to) = 0;
};

// config.json persistence (SPEC_v3 §7.1).
class ConfigStore {
 public:
  static constexpr const char* kPrimary = "/config.json";
  static constexpr const char* kBackup = "/config.bak";
  static constexpr const char* kTemp = "/config.tmp";
  static constexpr size_t kMaxBytes = 32 * 1024;

  enum class Source { Primary, Backup, FactoryDefault };

  explicit ConfigStore(IFileStore& files) : files_(files) {}

  // Returns the first valid config of config.json, config.bak and the
  // factory default. "Valid" means it parses, migrates and passes §7.3.
  Source load(std::string& json);

  // Atomic save: write config.tmp, check it reads back identically, move
  // config.json to config.bak, then config.tmp to config.json.
  bool save(const std::string& json);

  static const char* sourceName(Source source);

 private:
  bool readValid(const char* path, std::string& json);

  IFileStore& files_;
};

}  // namespace drinkbot
