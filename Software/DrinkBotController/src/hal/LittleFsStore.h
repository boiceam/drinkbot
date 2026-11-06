#pragma once

#include <FS.h>
#include <LittleFS.h>

#include "core/ConfigStore.h"
#include "core/Log.h"

namespace drinkbot {

// IFileStore over the LittleFS data partition (config, §4.1; the pump firmware image is in LittleFsPumpImage).
class LittleFsStore : public IFileStore {
 public:
  // Mounts, formatting the partition if it can't be mounted (§9.1 step 3).
  bool begin() {
    if (LittleFS.begin(false)) {
      return true;
    }
    LOG_E("CFG", "LittleFS mount failed; formatting");
    return LittleFS.begin(true);
  }

  bool exists(const std::string& path) override { return LittleFS.exists(path.c_str()); }

  bool read(const std::string& path, std::string& out) override {
    File f = LittleFS.open(path.c_str(), "r");
    if (!f) {
      return false;
    }
    out.clear();
    out.reserve(f.size());
    char buf[256];
    while (f.available()) {
      size_t n = f.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
      if (n == 0) {
        break;
      }
      out.append(buf, n);
    }
    f.close();
    return true;
  }

  bool write(const std::string& path, const std::string& data) override {
    File f = LittleFS.open(path.c_str(), "w");
    if (!f) {
      return false;
    }
    size_t n = f.write(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    f.close();
    return n == data.size();
  }

  bool remove(const std::string& path) override { return LittleFS.remove(path.c_str()); }

  bool rename(const std::string& from, const std::string& to) override {
    return LittleFS.rename(from.c_str(), to.c_str());
  }
};

}  // namespace drinkbot
