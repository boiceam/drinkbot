#pragma once

#include <ArduinoJson.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace drinkbot {

// Typed view of config.json (SPEC_v3 §7). The JSON document stays the
// stored form so unknown keys survive a save; this struct is what the
// rest of the firmware reads.

constexpr int kConfigSchemaVersion = 2;
constexpr size_t kButtonCount = 7;
constexpr size_t kMaxPumps = 16;
constexpr size_t kMaxRecipes = 64;
constexpr size_t kMaxIngredients = 16;
constexpr size_t kMaxNetworks = 8;
constexpr size_t kMaxNameLength = 32;
constexpr float kMaxDrinkVolumeMl = 1000.0f;  // pump firmware dispense cap (PF-3)

struct Rgb {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;

  bool operator==(const Rgb& o) const { return r == o.r && g == o.g && b == o.b; }
  bool operator!=(const Rgb& o) const { return !(*this == o); }
};

struct WifiNetwork {
  std::string ssid;
  std::string password;
};

struct LevelThresholds {
  float lowMl = 150;
  float emptyMl = 50;
  float actualEmptyMl = 20;
};

struct DispenseSettings {
  float verifyTolerancePct = 15;
  float verifyToleranceMinMl = 5;
  uint32_t settleMs = 1500;
  float minVolumeMl = 10;
  float maxVolumeMl = 1000;
};

struct Defaults {
  float volumeMl = 250;
  std::string recipeId;
  float primeSec = 5;
  float purgeSec = 10;
};

struct PumpConfig {
  int pumpId = 0;
  std::string liquid;      // "" = unloaded
  int msPerMl = 100;
  float densityGPerMl = 1.0f;
  Rgb color;
  float autoPrimeSec = 5;  // 0 = left out of Prime All / Purge All
};

struct Ingredient {
  std::string liquid;
  float parts = 1;
  int order = 0;
};

struct Recipe {
  std::string id;
  std::string name;
  std::vector<Ingredient> ingredients;
};

enum class ButtonAction : uint8_t { None, SetRecipe, SetVolume, Dispense, Estop };

const char* buttonActionName(ButtonAction action);

struct ButtonConfig {
  ButtonAction action = ButtonAction::None;
  std::string recipeId;  // SetRecipe
  float volumeMl = 0;    // SetVolume
};

struct Config {
  std::string hostname = "drinkbot";
  std::string apSsid = "DrinkBot";
  std::vector<WifiNetwork> networks;
  LevelThresholds levels;
  DispenseSettings dispense;
  Defaults defaults;
  uint8_t ledBrightness = 64;
  std::vector<PumpConfig> pumps;
  std::vector<Recipe> recipes;
  std::array<ButtonConfig, kButtonCount> buttons{};

  const PumpConfig* findPumpById(int pumpId) const;
  // Case-insensitive match on the loaded liquid name; nullptr if not loaded.
  const PumpConfig* findPumpByLiquid(const std::string& liquid) const;
  const Recipe* findRecipe(const std::string& id) const;
};

struct ConfigError {
  std::string path;     // e.g. "pumps[2].msPerMl"
  std::string message;
};

// Fills `doc` with the factory default config (§7.4).
void makeFactoryDefaultConfig(JsonDocument& doc);

// Upgrades an older schema in place. Returns true if anything changed.
// v1 -> v2: adds pumps[].autoPrimeSec = defaults.primeSec (§7.6).
bool migrateConfig(JsonDocument& doc);

// Parses and validates per §7.3. Missing sections and optional fields take
// their defaults; anything present but wrong is reported. `out` is only
// meaningful when the returned list is empty.
std::vector<ConfigError> parseConfig(JsonVariantConst root, Config& out);

bool equalsIgnoreCase(const std::string& a, const std::string& b);

}  // namespace drinkbot
