#include "core/Config.h"

#include <cctype>
#include <cmath>
#include <cstdio>

namespace drinkbot {

namespace {

const char kFactoryDefaultJson[] = R"json({
  "schemaVersion": 2,
  "device": { "hostname": "drinkbot", "apSsid": "DrinkBot" },
  "wifi": { "networks": [] },
  "levels": { "lowMl": 150, "emptyMl": 50, "actualEmptyMl": 20 },
  "dispense": {
    "verifyTolerancePct": 15, "verifyToleranceMinMl": 5, "settleMs": 1500,
    "minVolumeMl": 10, "maxVolumeMl": 1000
  },
  "defaults": { "volumeMl": 250, "recipeId": "", "primeSec": 5, "purgeSec": 10 },
  "ui": { "ledBrightness": 64 },
  "pumps": [],
  "recipes": [],
  "buttons": [
    { "action": "none" }, { "action": "none" }, { "action": "none" }, { "action": "none" },
    { "action": "none" }, { "action": "none" }, { "action": "none" }
  ]
})json";

std::string trim(const std::string& s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && isspace(static_cast<unsigned char>(s[b]))) {
    b++;
  }
  while (e > b && isspace(static_cast<unsigned char>(s[e - 1]))) {
    e--;
  }
  return s.substr(b, e - b);
}

std::string fmt(const char* format, double a, double b) {
  char buf[64];
  snprintf(buf, sizeof(buf), format, a, b);
  return buf;
}

// Reads typed values out of the JSON tree and records every problem.
class Reader {
 public:
  std::vector<ConfigError> errors;

  void error(const std::string& path, const std::string& message) {
    errors.push_back({path, message});
  }

  // A missing value keeps `out` (its default) unless `required`.
  bool number(JsonVariantConst v, const std::string& path, double min, double max, double& out,
              bool required = false) {
    if (v.isNull()) {
      if (required) {
        error(path, "is required");
      }
      return !required;
    }
    if (!v.is<double>()) {
      error(path, "must be a number");
      return false;
    }
    double d = v.as<double>();
    if (!(d >= min && d <= max)) {
      error(path, fmt("must be between %g and %g", min, max));
      return false;
    }
    out = d;
    return true;
  }

  bool number(JsonVariantConst v, const std::string& path, double min, double max, float& out,
              bool required = false) {
    double d = out;
    bool ok = number(v, path, min, max, d, required);
    out = static_cast<float>(d);
    return ok;
  }

  bool integer(JsonVariantConst v, const std::string& path, long min, long max, long& out,
               bool required = false) {
    if (v.isNull()) {
      if (required) {
        error(path, "is required");
      }
      return !required;
    }
    if (!v.is<long>()) {
      error(path, "must be a whole number");
      return false;
    }
    long n = v.as<long>();
    if (n < min || n > max) {
      error(path, fmt("must be between %g and %g", static_cast<double>(min), static_cast<double>(max)));
      return false;
    }
    out = n;
    return true;
  }

  template <typename T>
  bool integer(JsonVariantConst v, const std::string& path, long min, long max, T& out,
               bool required = false) {
    long n = static_cast<long>(out);
    bool ok = integer(v, path, min, max, n, required);
    out = static_cast<T>(n);
    return ok;
  }

  // Strings are trimmed before the length check (§7.3).
  bool string(JsonVariantConst v, const std::string& path, size_t minLen, size_t maxLen,
              std::string& out, bool required = false) {
    if (v.isNull()) {
      if (required) {
        error(path, "is required");
      }
      return !required;
    }
    if (!v.is<const char*>()) {
      error(path, "must be a string");
      return false;
    }
    std::string s = trim(v.as<const char*>());
    if (s.size() < minLen || s.size() > maxLen) {
      if (minLen == 0) {
        error(path, fmt("must be at most %g characters", static_cast<double>(maxLen), 0));
      } else {
        error(path, fmt("must be %g to %g characters", static_cast<double>(minLen),
                        static_cast<double>(maxLen)));
      }
      return false;
    }
    out = s;
    return true;
  }

  // Missing arrays are treated as empty.
  bool array(JsonVariantConst v, const std::string& path, size_t maxLen, JsonArrayConst& out) {
    if (v.isNull()) {
      out = JsonArrayConst();
      return true;
    }
    if (!v.is<JsonArrayConst>()) {
      error(path, "must be a list");
      return false;
    }
    out = v.as<JsonArrayConst>();
    if (out.size() > maxLen) {
      error(path, fmt("may have at most %g entries", static_cast<double>(maxLen), 0));
      return false;
    }
    return true;
  }

  bool object(JsonVariantConst v, const std::string& path) {
    if (v.isNull() || v.is<JsonObjectConst>()) {
      return true;
    }
    error(path, "must be an object");
    return false;
  }
};

std::string idx(const std::string& base, size_t i) {
  return base + "[" + std::to_string(i) + "]";
}

bool isSlug(const std::string& s, size_t maxLen) {
  if (s.empty() || s.size() > maxLen) {
    return false;
  }
  for (char c : s) {
    if (!(islower(static_cast<unsigned char>(c)) || isdigit(static_cast<unsigned char>(c)) || c == '-')) {
      return false;
    }
  }
  return true;
}

bool hasTenthResolution(double v) {
  double scaled = v * 10.0;
  return std::fabs(scaled - std::round(scaled)) < 1e-3;
}

void readColor(Reader& r, JsonVariantConst v, const std::string& path, Rgb& out) {
  if (v.isNull()) {
    return;
  }
  if (!v.is<JsonArrayConst>() || v.as<JsonArrayConst>().size() != 3) {
    r.error(path, "must be a list of 3 numbers 0-255");
    return;
  }
  JsonArrayConst a = v.as<JsonArrayConst>();
  uint8_t c[3] = {0, 0, 0};
  for (size_t i = 0; i < 3; i++) {
    r.integer(a[i], idx(path, i), 0, 255, c[i], true);
  }
  out = {c[0], c[1], c[2]};
}

bool parseButtonAction(const std::string& s, ButtonAction& out) {
  for (ButtonAction a : {ButtonAction::None, ButtonAction::SetRecipe, ButtonAction::SetVolume,
                         ButtonAction::Dispense, ButtonAction::Estop}) {
    if (s == buttonActionName(a)) {
      out = a;
      return true;
    }
  }
  return false;
}

}  // namespace

const char* buttonActionName(ButtonAction action) {
  switch (action) {
    case ButtonAction::None: return "none";
    case ButtonAction::SetRecipe: return "setRecipe";
    case ButtonAction::SetVolume: return "setVolume";
    case ButtonAction::Dispense: return "dispense";
    case ButtonAction::Estop: return "estop";
  }
  return "none";
}

bool equalsIgnoreCase(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); i++) {
    if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

const PumpConfig* Config::findPumpById(int pumpId) const {
  for (const auto& p : pumps) {
    if (p.pumpId == pumpId) {
      return &p;
    }
  }
  return nullptr;
}

const PumpConfig* Config::findPumpByLiquid(const std::string& liquid) const {
  if (liquid.empty()) {
    return nullptr;
  }
  for (const auto& p : pumps) {
    if (!p.liquid.empty() && equalsIgnoreCase(p.liquid, liquid)) {
      return &p;
    }
  }
  return nullptr;
}

const Recipe* Config::findRecipe(const std::string& id) const {
  for (const auto& r : recipes) {
    if (r.id == id) {
      return &r;
    }
  }
  return nullptr;
}

void makeFactoryDefaultConfig(JsonDocument& doc) {
  doc.clear();
  deserializeJson(doc, kFactoryDefaultJson);
}

bool migrateConfig(JsonDocument& doc) {
  int version = doc["schemaVersion"] | 1;
  bool changed = false;
  if (version < 2) {
    float primeSec = doc["defaults"]["primeSec"] | 5.0f;
    for (JsonObject pump : doc["pumps"].as<JsonArray>()) {
      if (pump["autoPrimeSec"].isNull()) {
        pump["autoPrimeSec"] = primeSec;
      }
    }
    doc["schemaVersion"] = 2;
    changed = true;
  }
  return changed;
}

std::vector<ConfigError> parseConfig(JsonVariantConst root, Config& out) {
  Reader r;
  out = Config();

  if (!root.is<JsonObjectConst>()) {
    r.error("", "config must be a JSON object");
    return r.errors;
  }

  long schema = kConfigSchemaVersion;
  r.integer(root["schemaVersion"], "schemaVersion", kConfigSchemaVersion, kConfigSchemaVersion, schema, true);

  // device
  JsonVariantConst device = root["device"];
  if (r.object(device, "device")) {
    if (r.string(device["hostname"], "device.hostname", 1, kMaxNameLength, out.hostname) &&
        !isSlug(out.hostname, kMaxNameLength)) {
      r.error("device.hostname", "may only contain a-z, 0-9 and '-'");
    }
    r.string(device["apSsid"], "device.apSsid", 1, 32, out.apSsid);
  }

  // wifi
  JsonVariantConst wifi = root["wifi"];
  if (r.object(wifi, "wifi")) {
    JsonArrayConst nets;
    if (r.array(wifi["networks"], "wifi.networks", kMaxNetworks, nets)) {
      for (size_t i = 0; i < nets.size(); i++) {
        std::string path = idx("wifi.networks", i);
        WifiNetwork n;
        r.string(nets[i]["ssid"], path + ".ssid", 1, 32, n.ssid, true);
        // Passwords are not trimmed: spaces are legal in WPA passphrases
        JsonVariantConst pw = nets[i]["password"];
        if (!pw.isNull()) {
          if (!pw.is<const char*>()) {
            r.error(path + ".password", "must be a string");
          } else {
            n.password = pw.as<const char*>();
            if (!n.password.empty() && (n.password.size() < 8 || n.password.size() > 63)) {
              r.error(path + ".password", "must be empty (open network) or 8 to 63 characters");
            }
          }
        }
        out.networks.push_back(n);
      }
    }
  }

  // levels
  JsonVariantConst levels = root["levels"];
  if (r.object(levels, "levels")) {
    bool ok = r.number(levels["lowMl"], "levels.lowMl", 0, 100000, out.levels.lowMl);
    ok &= r.number(levels["emptyMl"], "levels.emptyMl", 0, 100000, out.levels.emptyMl);
    ok &= r.number(levels["actualEmptyMl"], "levels.actualEmptyMl", 0, 100000, out.levels.actualEmptyMl);
    if (ok && !(out.levels.actualEmptyMl < out.levels.emptyMl && out.levels.emptyMl < out.levels.lowMl)) {
      r.error("levels", "must satisfy actualEmptyMl < emptyMl < lowMl");
    }
  }

  // dispense
  JsonVariantConst disp = root["dispense"];
  if (r.object(disp, "dispense")) {
    r.number(disp["verifyTolerancePct"], "dispense.verifyTolerancePct", 0, 100, out.dispense.verifyTolerancePct);
    r.number(disp["verifyToleranceMinMl"], "dispense.verifyToleranceMinMl", 0, 1000,
             out.dispense.verifyToleranceMinMl);
    r.integer(disp["settleMs"], "dispense.settleMs", 0, 10000, out.dispense.settleMs);
    bool ok = r.number(disp["minVolumeMl"], "dispense.minVolumeMl", 1, kMaxDrinkVolumeMl, out.dispense.minVolumeMl);
    ok &= r.number(disp["maxVolumeMl"], "dispense.maxVolumeMl", 1, kMaxDrinkVolumeMl, out.dispense.maxVolumeMl);
    if (ok && out.dispense.minVolumeMl >= out.dispense.maxVolumeMl) {
      r.error("dispense", "minVolumeMl must be less than maxVolumeMl");
    }
  }

  // ui
  JsonVariantConst ui = root["ui"];
  if (r.object(ui, "ui")) {
    r.integer(ui["ledBrightness"], "ui.ledBrightness", 0, 255, out.ledBrightness);
  }

  // pumps
  JsonArrayConst pumps;
  if (r.array(root["pumps"], "pumps", kMaxPumps, pumps)) {
    for (size_t i = 0; i < pumps.size(); i++) {
      std::string path = idx("pumps", i);
      JsonVariantConst p = pumps[i];
      if (!p.is<JsonObjectConst>()) {
        r.error(path, "must be an object");
        continue;
      }
      PumpConfig pc;
      if (r.integer(p["pumpId"], path + ".pumpId", 1, 254, pc.pumpId, true)) {
        for (const auto& other : out.pumps) {
          if (other.pumpId == pc.pumpId) {
            r.error(path + ".pumpId", "is already used by another pump");
          }
        }
      }
      if (r.string(p["liquid"], path + ".liquid", 0, kMaxNameLength, pc.liquid) && !pc.liquid.empty()) {
        if (out.findPumpByLiquid(pc.liquid)) {
          r.error(path + ".liquid", "is already loaded on another pump");
        }
      }
      r.integer(p["msPerMl"], path + ".msPerMl", 1, 65535, pc.msPerMl, true);
      r.number(p["densityGPerMl"], path + ".densityGPerMl", 0.5, 2.0, pc.densityGPerMl, true);
      readColor(r, p["color"], path + ".color", pc.color);
      if (r.number(p["autoPrimeSec"], path + ".autoPrimeSec", 0, 60, pc.autoPrimeSec) &&
          !hasTenthResolution(pc.autoPrimeSec)) {
        r.error(path + ".autoPrimeSec", "must be in steps of 0.1 s");
      }
      out.pumps.push_back(pc);
    }
  }

  // recipes
  JsonArrayConst recipes;
  if (r.array(root["recipes"], "recipes", kMaxRecipes, recipes)) {
    for (size_t i = 0; i < recipes.size(); i++) {
      std::string path = idx("recipes", i);
      JsonVariantConst rv = recipes[i];
      if (!rv.is<JsonObjectConst>()) {
        r.error(path, "must be an object");
        continue;
      }
      Recipe rec;
      if (r.string(rv["id"], path + ".id", 1, kMaxNameLength, rec.id, true)) {
        if (!isSlug(rec.id, kMaxNameLength)) {
          r.error(path + ".id", "may only contain a-z, 0-9 and '-'");
        } else if (out.findRecipe(rec.id)) {
          r.error(path + ".id", "is already used by another recipe");
        }
      }
      r.string(rv["name"], path + ".name", 1, kMaxNameLength, rec.name, true);

      JsonArrayConst ings;
      std::string ipath = path + ".ingredients";
      if (rv["ingredients"].isNull()) {
        r.error(ipath, "is required");
      } else if (r.array(rv["ingredients"], ipath, kMaxIngredients, ings)) {
        if (ings.size() == 0) {
          r.error(ipath, "must have at least 1 ingredient");
        }
        for (size_t j = 0; j < ings.size(); j++) {
          std::string jpath = idx(ipath, j);
          Ingredient ing;
          r.string(ings[j]["liquid"], jpath + ".liquid", 1, kMaxNameLength, ing.liquid, true);
          if (r.number(ings[j]["parts"], jpath + ".parts", 0, 1e6, ing.parts, true) && ing.parts <= 0) {
            r.error(jpath + ".parts", "must be greater than 0");
          }
          r.integer(ings[j]["order"], jpath + ".order", 0, 1000000, ing.order, true);
          rec.ingredients.push_back(ing);
        }
      }
      out.recipes.push_back(rec);
    }
  }

  // defaults (after recipes so recipeId can be checked)
  JsonVariantConst defs = root["defaults"];
  if (r.object(defs, "defaults")) {
    if (r.number(defs["volumeMl"], "defaults.volumeMl", 0, kMaxDrinkVolumeMl, out.defaults.volumeMl) &&
        (out.defaults.volumeMl < out.dispense.minVolumeMl || out.defaults.volumeMl > out.dispense.maxVolumeMl)) {
      r.error("defaults.volumeMl", "must be within dispense.minVolumeMl and dispense.maxVolumeMl");
    }
    if (r.string(defs["recipeId"], "defaults.recipeId", 0, kMaxNameLength, out.defaults.recipeId) &&
        !out.defaults.recipeId.empty() && !out.findRecipe(out.defaults.recipeId)) {
      r.error("defaults.recipeId", "does not match any recipe");
    }
    r.number(defs["primeSec"], "defaults.primeSec", 1, 60, out.defaults.primeSec);
    r.number(defs["purgeSec"], "defaults.purgeSec", 1, 60, out.defaults.purgeSec);
  }

  // buttons
  JsonVariantConst buttons = root["buttons"];
  if (!buttons.isNull()) {
    if (!buttons.is<JsonArrayConst>() || buttons.as<JsonArrayConst>().size() != kButtonCount) {
      r.error("buttons", "must have exactly 7 entries");
    } else {
      JsonArrayConst b = buttons.as<JsonArrayConst>();
      for (size_t i = 0; i < kButtonCount; i++) {
        std::string path = idx("buttons", i);
        ButtonConfig bc;
        std::string action;
        if (!r.string(b[i]["action"], path + ".action", 1, 16, action, true)) {
          continue;
        }
        if (!parseButtonAction(action, bc.action)) {
          r.error(path + ".action", "must be none, setRecipe, setVolume, dispense or estop");
          continue;
        }
        if (bc.action == ButtonAction::SetRecipe) {
          if (r.string(b[i]["recipeId"], path + ".recipeId", 1, kMaxNameLength, bc.recipeId, true) &&
              !out.findRecipe(bc.recipeId)) {
            r.error(path + ".recipeId", "does not match any recipe");
          }
        } else if (bc.action == ButtonAction::SetVolume) {
          if (r.number(b[i]["volumeMl"], path + ".volumeMl", 0, kMaxDrinkVolumeMl, bc.volumeMl, true) &&
              (bc.volumeMl < out.dispense.minVolumeMl || bc.volumeMl > out.dispense.maxVolumeMl)) {
            r.error(path + ".volumeMl", "must be within dispense.minVolumeMl and dispense.maxVolumeMl");
          }
        }
        out.buttons[i] = bc;
      }
    }
  }

  return r.errors;
}

}  // namespace drinkbot
