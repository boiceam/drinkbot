// Config storage: atomic save and load fallback (SPEC_v3 §7.1)

#include <unity.h>

#include <map>
#include <string>

#include "core/Config.h"
#include "core/ConfigStore.h"

using namespace drinkbot;

namespace {

class MemoryStore : public IFileStore {
 public:
  std::map<std::string, std::string> files;
  bool failWrites = false;
  bool failRenames = false;

  bool exists(const std::string& p) override { return files.count(p) > 0; }
  bool read(const std::string& p, std::string& out) override {
    auto it = files.find(p);
    if (it == files.end()) {
      return false;
    }
    out = it->second;
    return true;
  }
  bool write(const std::string& p, const std::string& d) override {
    if (failWrites) {
      return false;
    }
    files[p] = d;
    return true;
  }
  bool remove(const std::string& p) override { return files.erase(p) > 0; }
  bool rename(const std::string& from, const std::string& to) override {
    if (failRenames || !exists(from) || exists(to)) {
      return false;
    }
    files[to] = files[from];
    files.erase(from);
    return true;
  }
};

std::string configWithHostname(const char* hostname) {
  JsonDocument doc;
  makeFactoryDefaultConfig(doc);
  doc["device"]["hostname"] = hostname;
  std::string json;
  serializeJson(doc, json);
  return json;
}

std::string hostnameOf(const std::string& json) {
  JsonDocument doc;
  deserializeJson(doc, json);
  return doc["device"]["hostname"].as<std::string>();
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_empty_flash_uses_factory_default() {
  MemoryStore fs;
  ConfigStore store(fs);
  std::string json;
  TEST_ASSERT_TRUE(store.load(json) == ConfigStore::Source::FactoryDefault);
  TEST_ASSERT_EQUAL_STRING("drinkbot", hostnameOf(json).c_str());
}

void test_save_rotates_backup_and_leaves_no_temp() {
  MemoryStore fs;
  ConfigStore store(fs);
  TEST_ASSERT_TRUE(store.save(configWithHostname("first")));
  TEST_ASSERT_FALSE(fs.exists(ConfigStore::kBackup));
  TEST_ASSERT_TRUE(store.save(configWithHostname("second")));
  TEST_ASSERT_EQUAL_STRING("second", hostnameOf(fs.files[ConfigStore::kPrimary]).c_str());
  TEST_ASSERT_EQUAL_STRING("first", hostnameOf(fs.files[ConfigStore::kBackup]).c_str());
  TEST_ASSERT_FALSE(fs.exists(ConfigStore::kTemp));

  std::string json;
  TEST_ASSERT_TRUE(store.load(json) == ConfigStore::Source::Primary);
  TEST_ASSERT_EQUAL_STRING("second", hostnameOf(json).c_str());
}

void test_corrupt_primary_falls_back_to_backup() {
  MemoryStore fs;
  ConfigStore store(fs);
  store.save(configWithHostname("first"));
  store.save(configWithHostname("second"));
  fs.files[ConfigStore::kPrimary] = "{ not json";
  std::string json;
  TEST_ASSERT_TRUE(store.load(json) == ConfigStore::Source::Backup);
  TEST_ASSERT_EQUAL_STRING("first", hostnameOf(json).c_str());
}

void test_invalid_primary_falls_back_too() {
  MemoryStore fs;
  ConfigStore store(fs);
  store.save(configWithHostname("first"));
  store.save(configWithHostname("Bad Host Name"));  // fails §7.3
  std::string json;
  TEST_ASSERT_TRUE(store.load(json) == ConfigStore::Source::Backup);
  fs.files[ConfigStore::kBackup] = "[]";
  TEST_ASSERT_TRUE(store.load(json) == ConfigStore::Source::FactoryDefault);
}

void test_failed_write_keeps_existing_config() {
  MemoryStore fs;
  ConfigStore store(fs);
  store.save(configWithHostname("first"));
  fs.failWrites = true;
  TEST_ASSERT_FALSE(store.save(configWithHostname("second")));
  TEST_ASSERT_EQUAL_STRING("first", hostnameOf(fs.files[ConfigStore::kPrimary]).c_str());
}

void test_v1_config_on_flash_is_accepted() {
  MemoryStore fs;
  JsonDocument doc;
  makeFactoryDefaultConfig(doc);
  doc["schemaVersion"] = 1;
  JsonObject pump = doc["pumps"].add<JsonObject>();
  pump["pumpId"] = 1;
  pump["liquid"] = "Rum";
  pump["msPerMl"] = 100;
  pump["densityGPerMl"] = 1.0;
  std::string v1;
  serializeJson(doc, v1);
  fs.files[ConfigStore::kPrimary] = v1;
  ConfigStore store(fs);
  std::string json;
  TEST_ASSERT_TRUE(store.load(json) == ConfigStore::Source::Primary);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_empty_flash_uses_factory_default);
  RUN_TEST(test_save_rotates_backup_and_leaves_no_temp);
  RUN_TEST(test_corrupt_primary_falls_back_to_backup);
  RUN_TEST(test_invalid_primary_falls_back_too);
  RUN_TEST(test_failed_write_keeps_existing_config);
  RUN_TEST(test_v1_config_on_flash_is_accepted);
  return UNITY_END();
}
