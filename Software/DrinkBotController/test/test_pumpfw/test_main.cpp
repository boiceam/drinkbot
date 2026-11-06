// Pump firmware update from the controller (SPEC_v3 §9.10): the stored image
// and its checks, and the update sequence against simulated boards.
#include <ArduinoJson.h>
#include <unity.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "core/App.h"
#include "core/Md5.h"
#include "core/PumpImage.h"
#include "core/WsProtocol.h"
#include "sim/SimPumpLink.h"

using namespace drinkbot;

namespace {

const char kConfig[] = R"json({
  "schemaVersion": 2,
  "dispense": { "verifyTolerancePct": 15, "verifyToleranceMinMl": 5, "settleMs": 1500,
                "minVolumeMl": 10, "maxVolumeMl": 1000 },
  "defaults": { "volumeMl": 250, "recipeId": "", "primeSec": 5, "purgeSec": 10 },
  "pumps": [
    { "pumpId": 1, "liquid": "Tequila", "msPerMl": 100, "densityGPerMl": 1.0, "color": [255,160,0], "autoPrimeSec": 4 },
    { "pumpId": 2, "liquid": "Triple Sec", "msPerMl": 100, "densityGPerMl": 1.0, "color": [255,120,0], "autoPrimeSec": 0 },
    { "pumpId": 4, "liquid": "Lime Juice", "msPerMl": 100, "densityGPerMl": 1.0, "color": [0,255,0], "autoPrimeSec": 2 },
    { "pumpId": 3, "liquid": "Gin", "msPerMl": 100, "densityGPerMl": 1.0, "color": [0,0,255], "autoPrimeSec": 0 },
    { "pumpId": 5, "liquid": "Rum", "msPerMl": 100, "densityGPerMl": 1.0, "color": [0,0,255], "autoPrimeSec": 0 }
  ],
  "recipes": []
})json";

std::vector<uint8_t> makeImage(const std::string& version = "0.8.0", const std::string& bus = "50000",
                               size_t size = 3000) {
  std::vector<uint8_t> image(size, 0xA5);
  std::string tags = std::string("DBP-IMAGE:") + std::string(1, '\0') + "DBP-IMAGE:" + version + std::string(1, '\0') +
                     "DBP-BUS:" + bus + std::string(1, '\0');
  std::copy(tags.begin(), tags.end(), image.begin() + 1000);
  return image;
}

bool scan(const std::vector<uint8_t>& image, PumpImageInfo& info, std::string& why, size_t chunk = 64) {
  PumpImageScanner scanner;
  for (size_t i = 0; i < image.size(); i += chunk) {
    scanner.add(image.data() + i, std::min(chunk, image.size() - i));
  }
  return scanner.finish(info, why);
}

class MemImage : public IPumpImageStore {
 public:
  std::vector<uint8_t> bytes;
  PumpImageInfo image;
  bool uploading = false;
  void set(const std::vector<uint8_t>& data) {
    bytes = data;
    std::string why;
    TEST_ASSERT_TRUE_MESSAGE(scan(bytes, image, why), why.c_str());
  }
  PumpImageInfo info() const override { return image; }
  bool busy() const override { return uploading; }
  bool read(uint32_t offset, uint8_t* out, size_t len) override {
    if (offset + len > bytes.size()) {
      return false;
    }
    std::copy_n(bytes.begin() + offset, len, out);
    return true;
  }
  bool remove() override {
    image = PumpImageInfo();
    bytes.clear();
    return true;
  }
};

class FakeClock : public IClock {
 public:
  uint32_t t = 1000;
  uint32_t nowMs() const override { return t; }
};

Config parsedConfig() {
  JsonDocument doc;
  deserializeJson(doc, kConfig);
  Config c;
  parseConfig(doc.as<JsonVariantConst>(), c);
  return c;
}

struct Rig {
  FakeClock clock;
  SimPumpLink sim;
  App app;
  MemImage store;

  explicit Rig(bool bridgeBoard = true)
      : sim(clock, SimPumpLink::specsFromConfig(parsedConfig(), bridgeBoard)), app(sim, clock, &sim) {
    app.setPumpImageStore(&store);
    app.begin(kConfig, false);
    run(10000);
    TEST_ASSERT_FALSE(app.system().booting());
  }

  void run(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
      clock.t++;
      app.tick();
    }
  }

  CommandResult send(Command::Type type, int pumpId = 0) {
    Command cmd;
    cmd.type = type;
    cmd.pumpId = pumpId;
    auto out = std::make_shared<CommandResult>();
    auto done = std::make_shared<bool>(false);
    cmd.reply = [out, done](const CommandResult& r) {
      *out = r;
      *done = true;
    };
    app.post(std::move(cmd));
    for (int i = 0; i < 5000 && !*done; i++) {
      run(1);
    }
    TEST_ASSERT_TRUE_MESSAGE(*done, "command never answered");
    return *out;
  }

  void runUntilFinished(uint32_t maxMs = 600000) {
    for (uint32_t i = 0; i < maxMs && app.updater().active(); i++) {
      run(1);
    }
    TEST_ASSERT_FALSE_MESSAGE(app.updater().active(), "the update never finished");
    run(20000);  // the rediscovery at the end
  }

  int countPrefix(const std::string& prefix) const {
    int n = 0;
    for (const auto& line : sim.received()) {
      n += line.rfind(prefix, 0) == 0;
    }
    return n;
  }

  // Index of the board's first fwbegin in the traffic (-1 if none)
  int firstBegin(int pumpId) const {
    std::string prefix = pumpId == 0 ? "fwbegin" : "bridge " + std::to_string(pumpId) + " fwbegin";
    const auto& r = sim.received();
    for (size_t i = 0; i < r.size(); i++) {
      if (r[i].rfind(prefix, 0) == 0) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  bool hasError(ErrorCode code) const {
    for (const auto& e : app.system().errors()) {
      if (e.code == code) {
        return true;
      }
    }
    return false;
  }
};

}  // namespace

void setUp() {}
void tearDown() {}

void test_md5_standard_vectors() {
  Md5 a;
  TEST_ASSERT_EQUAL_STRING("d41d8cd98f00b204e9800998ecf8427e", a.finish().c_str());
  Md5 b;
  const char* text = "The quick brown fox jumps over the lazy dog";
  b.add(reinterpret_cast<const uint8_t*>(text), strlen(text));
  TEST_ASSERT_EQUAL_STRING("9e107d9d372bb6826bd81d3542a419d6", b.finish().c_str());
  // In pieces, across the 64-byte block size
  std::string longText(1000, 'x');
  Md5 whole, parts;
  whole.add(reinterpret_cast<const uint8_t*>(longText.data()), longText.size());
  for (char c : longText) {
    parts.add(reinterpret_cast<const uint8_t*>(&c), 1);
  }
  TEST_ASSERT_EQUAL_STRING(whole.finish().c_str(), parts.finish().c_str());
}

void test_base64_round_trip() {
  uint8_t data[120];
  for (size_t i = 0; i < sizeof(data); i++) {
    data[i] = static_cast<uint8_t>(i * 7);
  }
  std::string text = base64Encode(data, sizeof(data));
  TEST_ASSERT_EQUAL(160, text.size());
  TEST_ASSERT_TRUE(text.find_first_of("*$|]=") == std::string::npos);  // safe on the bus, no padding
  uint8_t back[192];
  TEST_ASSERT_EQUAL(120, base64Decode(text, back, sizeof(back)));
  TEST_ASSERT_EQUAL_MEMORY(data, back, sizeof(data));
  TEST_ASSERT_EQUAL_STRING("TWE=", base64Encode(reinterpret_cast<const uint8_t*>("Ma"), 2).c_str());
  TEST_ASSERT_EQUAL(-1, base64Decode("!!!!", back, sizeof(back)));
}

void test_scanner_accepts_a_good_image_in_any_chunking() {
  for (size_t chunk : {size_t(1), size_t(7), size_t(64), size_t(4096)}) {
    PumpImageInfo info;
    std::string why;
    TEST_ASSERT_TRUE_MESSAGE(scan(makeImage(), info, why, chunk), why.c_str());
    TEST_ASSERT_EQUAL_STRING("0.8.0", info.version.c_str());  // skips the bare search string
    TEST_ASSERT_EQUAL(3000, info.size);
    TEST_ASSERT_EQUAL(32, info.md5.size());
  }
}

void test_scanner_refuses_bad_images() {
  PumpImageInfo info;
  std::string why;
  TEST_ASSERT_FALSE(scan({}, info, why));
  TEST_ASSERT_FALSE(scan(std::vector<uint8_t>(3000, 0xA5), info, why));  // junk: no tag
  TEST_ASSERT_TRUE(why.find("DBP-IMAGE") != std::string::npos);

  std::vector<uint8_t> gz = makeImage();
  gz[0] = 0x1f;
  gz[1] = 0x8b;
  TEST_ASSERT_FALSE(scan(gz, info, why));
  TEST_ASSERT_TRUE(why.find("Compressed") != std::string::npos);

  std::vector<uint8_t> uf2 = makeImage();
  std::copy_n("UF2\n", 4, uf2.begin());
  TEST_ASSERT_FALSE(scan(uf2, info, why));
  TEST_ASSERT_TRUE(why.find("UF2") != std::string::npos);

  TEST_ASSERT_FALSE(scan(makeImage("0.5.0"), info, why));  // older than the minimum
  TEST_ASSERT_TRUE(why.find("older") != std::string::npos);

  // Would strand pumps partway through a bus update
  TEST_ASSERT_FALSE(scan(makeImage("0.8.0", "115200"), info, why));
  TEST_ASSERT_TRUE(why.find("115200") != std::string::npos);

  std::vector<uint8_t> noBus(3000, 0xA5);
  std::string tag = "DBP-IMAGE:0.8.0";
  std::copy(tag.begin(), tag.end(), noBus.begin() + 100);
  TEST_ASSERT_FALSE(scan(noBus, info, why));
  TEST_ASSERT_TRUE(why.find("DBP-BUS") != std::string::npos);

  TEST_ASSERT_FALSE(scan(makeImage("0.8.0", "50000", kMaxPumpImageBytes + 1), info, why));
}

// The rule the user asked for: storing an image must never start an update
void test_nothing_is_sent_until_the_button_is_pressed() {
  Rig rig;
  rig.store.set(makeImage());
  rig.send(Command::Type::PumpFwChanged);
  rig.run(120000);
  TEST_ASSERT_EQUAL(0, rig.countPrefix("fwbegin"));
  for (const auto& line : rig.sim.received()) {
    TEST_ASSERT_TRUE_MESSAGE(line.find(" fw") == std::string::npos, line.c_str());
  }
  // ...while it does know what would be updated
  auto plan = rig.app.updater().plan();
  TEST_ASSERT_EQUAL(6, plan.targets.size());  // the five configured pumps and the bridge
  TEST_ASSERT_FALSE(rig.app.updater().active());
}

void test_update_runs_in_id_order_with_the_bridge_last() {
  Rig rig;
  rig.store.set(makeImage());
  rig.sim.clearReceived();
  TEST_ASSERT_TRUE(rig.send(Command::Type::PumpFwUpdate).ok);
  TEST_ASSERT_TRUE(rig.app.updater().active());
  rig.runUntilFinished();

  TEST_ASSERT_EQUAL_STRING("COMPLETE", rig.app.updater().status().result.c_str());
  for (int id : {0, 1, 2, 3, 4, 5}) {
    TEST_ASSERT_EQUAL_STRING("0.8.0", rig.sim.firmwareVersion(id).c_str());
    TEST_ASSERT_EQUAL(1, rig.sim.installCount(id));
  }
  TEST_ASSERT_EQUAL_STRING("0.7.0-sim", rig.sim.firmwareVersion(6).c_str());  // not configured: left alone
  TEST_ASSERT_TRUE(rig.firstBegin(1) < rig.firstBegin(2));
  TEST_ASSERT_TRUE(rig.firstBegin(2) < rig.firstBegin(3));
  TEST_ASSERT_TRUE(rig.firstBegin(4) < rig.firstBegin(5));
  TEST_ASSERT_TRUE(rig.firstBegin(5) < rig.firstBegin(0));  // the bridge after every pump
  TEST_ASSERT_TRUE(rig.app.pumps().find(1)->fwVersion == "0.8.0");
  TEST_ASSERT_EQUAL_STRING("0.8.0", rig.app.pumps().bridgeVersion().c_str());
  TEST_ASSERT_FALSE(rig.app.system().hasErrors());  // including no E_USB_LOST for the bridge
  TEST_ASSERT_TRUE(rig.app.system().state() == SysState::Ready);
  TEST_ASSERT_EQUAL(0, rig.app.updater().plan().targets.size());
  // Nothing left to do now
  TEST_ASSERT_EQUAL_STRING("INVALID", rig.send(Command::Type::PumpFwUpdate).code.c_str());
}

void test_bad_install_on_the_first_pump_stops_the_rollout() {
  Rig rig;
  rig.store.set(makeImage());
  rig.sim.setInstallFault(1, SimPumpLink::InstallFault::NeverReturns);
  TEST_ASSERT_TRUE(rig.send(Command::Type::PumpFwUpdate).ok);
  rig.runUntilFinished();
  TEST_ASSERT_EQUAL_STRING("FAILED", rig.app.updater().status().result.c_str());
  TEST_ASSERT_EQUAL(1, rig.sim.installCount(1));
  TEST_ASSERT_EQUAL(-1, rig.firstBegin(2));  // no other board was touched
  TEST_ASSERT_EQUAL(-1, rig.firstBegin(0));
  TEST_ASSERT_EQUAL_STRING("0.7.0-sim", rig.sim.firmwareVersion(2).c_str());
  TEST_ASSERT_TRUE(rig.hasError(ErrorCode::PumpUpdate));
  TEST_ASSERT_TRUE(rig.hasError(ErrorCode::PumpMissing));  // rediscovery noticed pump 1
  TEST_ASSERT_TRUE(rig.app.system().state() == SysState::Error);
}

void test_install_that_keeps_the_old_version_stops_the_rollout() {
  Rig rig;
  rig.store.set(makeImage());
  rig.sim.setInstallFault(1, SimPumpLink::InstallFault::KeepsOldVersion);
  TEST_ASSERT_TRUE(rig.send(Command::Type::PumpFwUpdate).ok);
  rig.runUntilFinished();
  TEST_ASSERT_EQUAL_STRING("FAILED", rig.app.updater().status().result.c_str());
  TEST_ASSERT_TRUE(rig.app.updater().status().message.find("0.7.0-sim") != std::string::npos);
  TEST_ASSERT_EQUAL(-1, rig.firstBegin(2));
}

void test_refused_image_says_why_and_installs_nothing() {
  Rig rig;
  rig.store.set(makeImage());
  rig.sim.setRefuseImage(1, true);
  TEST_ASSERT_TRUE(rig.send(Command::Type::PumpFwUpdate).ok);
  rig.runUntilFinished();
  TEST_ASSERT_EQUAL_STRING("FAILED", rig.app.updater().status().result.c_str());
  TEST_ASSERT_TRUE(rig.app.updater().status().message.find("USB") != std::string::npos);
  TEST_ASSERT_EQUAL(0, rig.sim.installCount(1));
  TEST_ASSERT_EQUAL(-1, rig.firstBegin(2));
  TEST_ASSERT_EQUAL_STRING("0.7.0-sim", rig.sim.firmwareVersion(1).c_str());
}

void test_lost_chunks_are_retried() {
  Rig rig;
  rig.store.set(makeImage());
  TEST_ASSERT_TRUE(rig.send(Command::Type::PumpFwUpdate).ok);
  // Partway through pump 1's transfer: one reply lost, one command never run
  for (int i = 0; i < 200000 && rig.app.updater().status().bytes < 600; i++) {
    rig.run(1);
  }
  rig.sim.dropNextResponse(1);
  rig.run(50);
  rig.sim.ignoreNextCommand(1);
  rig.runUntilFinished();
  TEST_ASSERT_EQUAL_STRING("COMPLETE", rig.app.updater().status().result.c_str());
  TEST_ASSERT_EQUAL_STRING("0.8.0", rig.sim.firmwareVersion(1).c_str());
}

void test_a_pump_that_stops_answering_fails_the_transfer() {
  Rig rig;
  rig.store.set(makeImage());
  TEST_ASSERT_TRUE(rig.send(Command::Type::PumpFwUpdate).ok);
  for (int i = 0; i < 200000 && rig.app.updater().status().bytes < 600; i++) {
    rig.run(1);
  }
  rig.sim.setMissing(1, true);
  rig.runUntilFinished();
  TEST_ASSERT_EQUAL_STRING("FAILED", rig.app.updater().status().result.c_str());
  TEST_ASSERT_EQUAL(0, rig.sim.installCount(1));
  TEST_ASSERT_EQUAL(-1, rig.firstBegin(2));
}

void test_other_operations_are_busy_during_an_update() {
  Rig rig;
  rig.store.set(makeImage());
  TEST_ASSERT_TRUE(rig.send(Command::Type::PumpFwUpdate).ok);
  rig.run(100);
  TEST_ASSERT_EQUAL_STRING("BUSY", rig.send(Command::Type::Prime, 2).code.c_str());
  TEST_ASSERT_EQUAL_STRING("BUSY", rig.send(Command::Type::PrimeAll).code.c_str());
  TEST_ASSERT_EQUAL_STRING("BUSY", rig.send(Command::Type::PumpFwUpdate).code.c_str());
  TEST_ASSERT_EQUAL_STRING("BUSY", rig.send(Command::Type::PumpFwDelete).code.c_str());
  rig.runUntilFinished();
}

void test_estop_during_transfer_cancels_and_keeps_the_old_firmware() {
  Rig rig;
  rig.store.set(makeImage());
  TEST_ASSERT_TRUE(rig.send(Command::Type::PumpFwUpdate).ok);
  for (int i = 0; i < 200000 && rig.app.updater().status().bytes < 600; i++) {
    rig.run(1);
  }
  rig.send(Command::Type::Estop);
  TEST_ASSERT_FALSE(rig.app.updater().active());
  TEST_ASSERT_EQUAL_STRING("CANCELLED", rig.app.updater().status().result.c_str());
  rig.run(20000);
  TEST_ASSERT_TRUE(rig.countPrefix("bridge 1 fwabort") >= 1);
  TEST_ASSERT_EQUAL(0, rig.sim.installCount(1));
  TEST_ASSERT_EQUAL_STRING("0.7.0-sim", rig.sim.firmwareVersion(1).c_str());
  TEST_ASSERT_FALSE(rig.hasError(ErrorCode::PumpUpdate));
  TEST_ASSERT_TRUE(rig.app.system().state() == SysState::Ready);
}

void test_cancel_stops_during_transfer() {
  Rig rig;
  rig.store.set(makeImage());
  TEST_ASSERT_EQUAL_STRING("INVALID", rig.send(Command::Type::PumpFwCancel).code.c_str());
  TEST_ASSERT_TRUE(rig.send(Command::Type::PumpFwUpdate).ok);
  for (int i = 0; i < 200000 && rig.app.updater().status().bytes < 600; i++) {
    rig.run(1);
  }
  TEST_ASSERT_TRUE(rig.send(Command::Type::PumpFwCancel).ok);
  rig.runUntilFinished();
  TEST_ASSERT_EQUAL_STRING("CANCELLED", rig.app.updater().status().result.c_str());
  TEST_ASSERT_TRUE(rig.countPrefix("bridge 1 fwabort") >= 1);
  TEST_ASSERT_EQUAL(0, rig.sim.installCount(1));
  TEST_ASSERT_FALSE(rig.hasError(ErrorCode::PumpUpdate));
}

void test_cancel_during_install_stops_before_the_next_board() {
  Rig rig;
  rig.store.set(makeImage());
  TEST_ASSERT_TRUE(rig.send(Command::Type::PumpFwUpdate).ok);
  for (int i = 0; i < 200000 && rig.app.updater().status().phase != PumpUpdater::Phase::Install; i++) {
    rig.run(1);
  }
  TEST_ASSERT_TRUE(rig.send(Command::Type::PumpFwCancel).ok);
  rig.runUntilFinished();
  TEST_ASSERT_EQUAL_STRING("CANCELLED", rig.app.updater().status().result.c_str());
  TEST_ASSERT_EQUAL_STRING("0.8.0", rig.sim.firmwareVersion(1).c_str());  // that one finished
  TEST_ASSERT_EQUAL(-1, rig.firstBegin(2));
}

void test_bridge_that_never_returns_reports_the_lost_link() {
  Rig rig;
  rig.store.set(makeImage());
  rig.sim.setInstallFault(0, SimPumpLink::InstallFault::NeverReturns);
  TEST_ASSERT_TRUE(rig.send(Command::Type::PumpFwUpdate).ok);
  rig.runUntilFinished();
  TEST_ASSERT_EQUAL_STRING("FAILED", rig.app.updater().status().result.c_str());
  TEST_ASSERT_EQUAL_STRING("0.8.0", rig.sim.firmwareVersion(5).c_str());  // the pumps finished first
  TEST_ASSERT_TRUE(rig.hasError(ErrorCode::PumpUpdate));
  TEST_ASSERT_TRUE(rig.hasError(ErrorCode::UsbLost));
}

void test_plan_never_downgrades_and_lists_what_it_cannot_update() {
  Rig rig;
  rig.store.set(makeImage());
  rig.sim.setFirmwareVersion(1, "0.9.0");  // newer than the image
  rig.sim.setFirmwareVersion(2, "0.8.0");  // same
  rig.sim.setFirmwareVersion(3, "0.5.0");  // too old for the update commands
  rig.sim.setMissing(4, true);
  rig.send(Command::Type::Rescan);
  rig.run(30000);
  auto plan = rig.app.updater().plan();
  auto has = [](const std::vector<int>& list, int id) { return std::find(list.begin(), list.end(), id) != list.end(); };
  std::vector<int> targets;
  for (const auto& t : plan.targets) {
    targets.push_back(t.pumpId);
  }
  TEST_ASSERT_FALSE(has(targets, 1));
  TEST_ASSERT_FALSE(has(targets, 2));
  TEST_ASSERT_FALSE(has(targets, 3));
  TEST_ASSERT_TRUE(has(plan.needsUsb, 3));
  TEST_ASSERT_TRUE(has(plan.missing, 4));
  TEST_ASSERT_TRUE(has(targets, 5));
  TEST_ASSERT_EQUAL(0, targets.back());  // bridge last
}

void test_requires_an_image_and_an_idle_store() {
  Rig rig;
  TEST_ASSERT_EQUAL_STRING("INVALID", rig.send(Command::Type::PumpFwUpdate).code.c_str());
  rig.store.set(makeImage());
  rig.store.uploading = true;
  TEST_ASSERT_EQUAL_STRING("BUSY", rig.send(Command::Type::PumpFwUpdate).code.c_str());
  rig.store.uploading = false;
  TEST_ASSERT_TRUE(rig.send(Command::Type::PumpFwDelete).ok);
  TEST_ASSERT_FALSE(rig.store.info().present);
  TEST_ASSERT_EQUAL(0, rig.app.updater().plan().targets.size());
}

void test_bench_pump_as_bridge_goes_last_and_direct() {
  Rig rig(false);  // pump 1 is the bridge
  rig.store.set(makeImage());
  rig.sim.clearReceived();
  TEST_ASSERT_TRUE(rig.send(Command::Type::PumpFwUpdate).ok);
  rig.runUntilFinished();
  TEST_ASSERT_EQUAL_STRING("COMPLETE", rig.app.updater().status().result.c_str());
  TEST_ASSERT_EQUAL_STRING("0.8.0", rig.sim.firmwareVersion(1).c_str());
  TEST_ASSERT_TRUE(rig.firstBegin(2) < rig.firstBegin(0));  // firstBegin(0): the direct fwbegin
  TEST_ASSERT_EQUAL(0, rig.countPrefix("bridge 1 fwbegin"));
  TEST_ASSERT_FALSE(rig.app.system().hasErrors());
}

void test_pumpfw_message_reports_image_targets_and_progress() {
  Rig rig;
  JsonDocument doc;
  deserializeJson(doc, pumpFwJson(rig.app));
  TEST_ASSERT_EQUAL_STRING("pumpFw", doc["type"]);
  TEST_ASSERT_TRUE(doc["image"].isNull());
  TEST_ASSERT_EQUAL(0, doc["targets"].size());
  TEST_ASSERT_EQUAL_STRING("0.7.0-sim", doc["bridgeVersion"]);

  rig.store.set(makeImage());
  deserializeJson(doc, pumpFwJson(rig.app));
  TEST_ASSERT_EQUAL_STRING("0.8.0", doc["image"]["version"]);
  TEST_ASSERT_EQUAL(6, doc["targets"].size());
  TEST_ASSERT_EQUAL(0, doc["targets"][5]["pumpId"].as<int>());
  TEST_ASSERT_TRUE(doc["targets"][5]["bridge"].as<bool>());
  TEST_ASSERT_FALSE(doc["update"]["active"].as<bool>());

  rig.store.set(makeImage("0.8.0", "50000", 60000));  // long enough to still be transferring
  rig.send(Command::Type::PumpFwUpdate);
  rig.run(1000);
  deserializeJson(doc, pumpFwJson(rig.app));
  TEST_ASSERT_TRUE(doc["update"]["active"].as<bool>());
  TEST_ASSERT_EQUAL_STRING("TRANSFER", doc["update"]["phase"]);
  TEST_ASSERT_EQUAL(1, doc["update"]["pumpId"].as<int>());
  TEST_ASSERT_EQUAL(1, doc["update"]["index"].as<int>());
  TEST_ASSERT_EQUAL(6, doc["update"]["total"].as<int>());
  TEST_ASSERT_TRUE(doc["update"]["bytes"].as<int>() > 0);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_md5_standard_vectors);
  RUN_TEST(test_base64_round_trip);
  RUN_TEST(test_scanner_accepts_a_good_image_in_any_chunking);
  RUN_TEST(test_scanner_refuses_bad_images);
  RUN_TEST(test_nothing_is_sent_until_the_button_is_pressed);
  RUN_TEST(test_update_runs_in_id_order_with_the_bridge_last);
  RUN_TEST(test_bad_install_on_the_first_pump_stops_the_rollout);
  RUN_TEST(test_install_that_keeps_the_old_version_stops_the_rollout);
  RUN_TEST(test_refused_image_says_why_and_installs_nothing);
  RUN_TEST(test_lost_chunks_are_retried);
  RUN_TEST(test_a_pump_that_stops_answering_fails_the_transfer);
  RUN_TEST(test_other_operations_are_busy_during_an_update);
  RUN_TEST(test_estop_during_transfer_cancels_and_keeps_the_old_firmware);
  RUN_TEST(test_cancel_stops_during_transfer);
  RUN_TEST(test_cancel_during_install_stops_before_the_next_board);
  RUN_TEST(test_bridge_that_never_returns_reports_the_lost_link);
  RUN_TEST(test_plan_never_downgrades_and_lists_what_it_cannot_update);
  RUN_TEST(test_requires_an_image_and_an_idle_store);
  RUN_TEST(test_bench_pump_as_bridge_goes_last_and_direct);
  RUN_TEST(test_pumpfw_message_reports_image_targets_and_progress);
  return UNITY_END();
}
