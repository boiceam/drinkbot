#pragma once

#include <FS.h>
#include <LittleFS.h>

#include <atomic>
#include <mutex>
#include <string>

#include "core/Log.h"
#include "core/PumpImage.h"

namespace drinkbot {

// The stored pump firmware image, kept in LittleFS across reboots and controller
// updates (SPEC_v3 §9.10). An upload goes to a temporary file and replaces the
// current image only once it has passed every check.
// Uploads run on the AsyncTCP task and reads on the App task.
class LittleFsPumpImage : public IPumpImageStore {
 public:
  static constexpr const char* kPath = "/pumpfw.bin";
  static constexpr const char* kTemp = "/pumpfw.tmp";

  // Call once LittleFS is mounted. Scans the stored image, if any.
  void begin() {
    LittleFS.remove(kTemp);
    File f = LittleFS.open(kPath, "r");
    if (!f) {
      return;
    }
    PumpImageScanner scanner;
    uint8_t buf[512];
    size_t n;
    while ((n = f.read(buf, sizeof(buf))) > 0) {
      scanner.add(buf, n);
    }
    f.close();
    PumpImageInfo found;
    std::string why;
    if (scanner.finish(found, why)) {
      std::lock_guard<std::mutex> lock(mutex_);
      info_ = found;
      LOG_I("PUMP", "Stored pump firmware: %s, %u bytes", found.version.c_str(), static_cast<unsigned>(found.size));
    } else {
      LOG_E("PUMP", "Removing the stored pump firmware: %s", why.c_str());
      LittleFS.remove(kPath);
    }
  }

  PumpImageInfo info() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return info_;
  }

  bool busy() const override { return uploading_.load(); }

  bool read(uint32_t offset, uint8_t* out, size_t len) override {
    File f = LittleFS.open(kPath, "r");
    if (!f || !f.seek(offset)) {
      return false;
    }
    size_t n = f.read(out, len);
    f.close();
    return n == len;
  }

  bool remove() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      info_ = PumpImageInfo();
    }
    return !LittleFS.exists(kPath) || LittleFS.remove(kPath);
  }

  // --- Upload (AsyncTCP task) ---
  bool beginUpload(size_t declaredSize, std::string& why) {
    size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    if (declaredSize > kMaxPumpImageBytes + 2048 || declaredSize > freeBytes) {
      why = "The file is too large to store";
      return false;
    }
    file_ = LittleFS.open(kTemp, "w");
    if (!file_) {
      why = "Couldn't create the file";
      return false;
    }
    scanner_ = PumpImageScanner();
    uploading_ = true;
    return true;
  }

  bool writeUpload(const uint8_t* data, size_t len, std::string& why) {
    scanner_.add(data, len);
    if (file_.write(data, len) != len) {
      why = "Couldn't write the file; storage may be full";
      return false;
    }
    return true;
  }

  bool finishUpload(std::string& why) {
    file_.close();
    uploading_ = false;
    PumpImageInfo found;
    if (!scanner_.finish(found, why)) {
      LittleFS.remove(kTemp);
      return false;
    }
    LittleFS.remove(kPath);
    if (!LittleFS.rename(kTemp, kPath)) {
      why = "Couldn't store the file";
      std::lock_guard<std::mutex> lock(mutex_);
      info_ = PumpImageInfo();
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    info_ = found;
    return true;
  }

  void abortUpload() {
    if (file_) {
      file_.close();
    }
    LittleFS.remove(kTemp);
    uploading_ = false;
  }

 private:
  mutable std::mutex mutex_;
  PumpImageInfo info_;
  std::atomic<bool> uploading_{false};
  File file_;
  PumpImageScanner scanner_;
};

}  // namespace drinkbot
