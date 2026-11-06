// CRC, line framing and pump protocol parsing (SPEC_v3 §5)

#include <unity.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/Crc32.h"
#include "core/LineFramer.h"
#include "core/PumpProtocol.h"

using namespace drinkbot;

namespace {

// Verbatim copy of calculateCRC32() from Software/DrinkBotPump/DrinkBotPump.ino,
// so the controller is checked against the pump's own algorithm.
uint32_t pumpCalculateCRC32(const char* data, size_t length) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < length; i++) {
    char ch = data[i];
    for (size_t j = 0; j < 8; j++) {
      uint32_t b = (ch ^ crc) & 1;
      crc >>= 1;
      if (b) {
        crc = crc ^ 0xEDB88320;
      }
      ch >>= 1;
    }
  }
  return ~crc;
}

// Builds a response packet exactly like the pump's sendCommBusResponse().
std::string pumpResponse(const char* payload) {
  char crcData[256];
  snprintf(crcData, sizeof(crcData), "$[%s|", payload);
  char packet[300];
  snprintf(packet, sizeof(packet), "%s%08lX]", crcData,
           static_cast<unsigned long>(pumpCalculateCRC32(crcData, strlen(crcData))));
  return packet;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_crc_standard_check_value() {
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926, crc32("123456789", 9));
}

void test_crc_matches_pump_algorithm() {
  const char* samples[] = {"$[R,OK|", "$[P,WEIGHT,123.456000|", "*[3|dispense 50|", "*[255|stop|", ""};
  for (const char* s : samples) {
    TEST_ASSERT_EQUAL_HEX32(pumpCalculateCRC32(s, strlen(s)), crc32(s, strlen(s)));
  }
}

void test_framer_splits_and_strips_cr() {
  std::vector<std::string> lines;
  LineFramer f([&](const std::string& l) { lines.push_back(l); });
  const char* data = "R,OK\r\nP,VER,0.3.0\nR,";
  f.feed(reinterpret_cast<const uint8_t*>(data), strlen(data));
  TEST_ASSERT_EQUAL(2, lines.size());
  TEST_ASSERT_EQUAL_STRING("R,OK", lines[0].c_str());
  TEST_ASSERT_EQUAL_STRING("P,VER,0.3.0", lines[1].c_str());
  f.feed(reinterpret_cast<const uint8_t*>("ERR\n"), 4);
  TEST_ASSERT_EQUAL(3, lines.size());
  TEST_ASSERT_EQUAL_STRING("R,ERR", lines[2].c_str());
}

void test_framer_discards_overlong_line() {
  std::vector<std::string> lines;
  int overflows = 0;
  LineFramer f([&](const std::string& l) { lines.push_back(l); }, [&] { overflows++; });
  std::string big(LineFramer::kMaxLine + 10, 'x');
  big += "\nR,OK\n";
  f.feed(reinterpret_cast<const uint8_t*>(big.data()), big.size());
  TEST_ASSERT_EQUAL(1, overflows);
  TEST_ASSERT_EQUAL(1, lines.size());
  TEST_ASSERT_EQUAL_STRING("R,OK", lines[0].c_str());
}

void test_parse_bridged_packet_and_bad_crc() {
  ParsedLine p = parseLine(pumpResponse("P,WEIGHT,12.5"));
  TEST_ASSERT_TRUE(p.kind == ParsedLine::Kind::Data);
  TEST_ASSERT_TRUE(p.bridged);
  TEST_ASSERT_EQUAL_STRING("P,WEIGHT,12.5", p.payload.c_str());

  std::string bad = pumpResponse("R,OK");
  bad[3] = 'K';  // corrupt payload, CRC no longer matches
  TEST_ASSERT_TRUE(parseLine(bad).kind == ParsedLine::Kind::BadCrc);

  TEST_ASSERT_TRUE(parseLine("SCALE - debug noise").kind == ParsedLine::Kind::Ignored);
}

void test_direct_transaction_collects_data() {
  TransactionParser t;
  t.begin(false);
  TEST_ASSERT_FALSE(t.feed("P,WEIGHT,100.250000"));
  TEST_ASSERT_FALSE(t.feed("some debug line"));
  TEST_ASSERT_TRUE(t.feed("R,OK"));
  float g = 0;
  TEST_ASSERT_TRUE(t.result().ok());
  TEST_ASSERT_TRUE(parseWeight(t.result(), g));
  TEST_ASSERT_EQUAL_FLOAT(100.25f, g);
}

void test_bridged_transaction_forwards_all_packets() {
  TransactionParser t;
  t.begin(true);
  TEST_ASSERT_FALSE(t.feed(pumpResponse("P,STATUS,FWD,1200")));
  TEST_ASSERT_TRUE(t.feed(pumpResponse("R,OK")));
  PumpMotion m;
  uint32_t remaining = 0;
  TEST_ASSERT_TRUE(parseStatus(t.result(), m, remaining));
  TEST_ASSERT_TRUE(m == PumpMotion::Forward);
  TEST_ASSERT_EQUAL_UINT32(1200, remaining);
}

void test_bridged_transaction_timeout_and_reject() {
  TransactionParser t;
  t.begin(true);
  TEST_ASSERT_FALSE(t.feed("R,OK"));  // legacy 0.2.0 local ack is ignored
  TEST_ASSERT_TRUE(t.feed("R,TIMEOUT"));
  TEST_ASSERT_TRUE(t.result().status == TxnStatus::Timeout);

  t.begin(true);
  TEST_ASSERT_TRUE(t.feed("R,ERR"));  // bridge rejected the request
  TEST_ASSERT_TRUE(t.result().status == TxnStatus::Err);

  t.begin(true);
  std::string bad = pumpResponse("R,OK");
  bad[bad.size() - 2] = bad[bad.size() - 2] == '0' ? '1' : '0';
  TEST_ASSERT_TRUE(t.feed(bad));
  TEST_ASSERT_TRUE(t.result().status == TxnStatus::BadCrc);
}

void test_build_wire_command_limits() {
  std::string out;
  TEST_ASSERT_TRUE(buildWireCommand(true, 1, "weight", out));
  TEST_ASSERT_EQUAL_STRING("weight", out.c_str());
  TEST_ASSERT_TRUE(buildWireCommand(false, 3, "dispense 62.5 3", out));
  TEST_ASSERT_EQUAL_STRING("bridge 3 dispense 62.5 3", out.c_str());
  TEST_ASSERT_TRUE(buildWireCommand(false, 255, "stop", out));
  TEST_ASSERT_TRUE(buildWireCommand(false, 2, "ledfade 1500 500 1500 500", out));  // 5 tokens

  TEST_ASSERT_FALSE(buildWireCommand(false, 2, "a b c d e f g", out));  // 7 > 6 inside bridge
  TEST_ASSERT_TRUE(buildWireCommand(true, 2, "a b c d e f g", out));
  TEST_ASSERT_FALSE(buildWireCommand(true, 1, std::string(64, 'a'), out));  // first token > 15
  std::string longData = "fwdata 118200 " + std::string(160, 'A');  // the firmware update chunk
  TEST_ASSERT_TRUE(buildWireCommand(false, 2, longData, out));
  TEST_ASSERT_FALSE(buildWireCommand(false, 2, "fwdata 0 " + std::string(200, 'A'), out));
  TEST_ASSERT_FALSE(buildWireCommand(true, 1, "averyveryverylongcommand", out));
  TEST_ASSERT_FALSE(buildWireCommand(true, 1, "bad|cmd", out));
  TEST_ASSERT_FALSE(buildWireCommand(false, 0, "weight", out));
}

void test_compare_versions() {
  TEST_ASSERT_EQUAL(0, compareVersions("0.3.0", "0.3.0-sim"));
  TEST_ASSERT_TRUE(compareVersions("0.2.0", kMinPumpFirmware) < 0);
  TEST_ASSERT_TRUE(compareVersions("0.10.0", "0.3.0") > 0);
  TEST_ASSERT_TRUE(compareVersions("1.0", "0.9.9") > 0);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_crc_standard_check_value);
  RUN_TEST(test_crc_matches_pump_algorithm);
  RUN_TEST(test_framer_splits_and_strips_cr);
  RUN_TEST(test_framer_discards_overlong_line);
  RUN_TEST(test_parse_bridged_packet_and_bad_crc);
  RUN_TEST(test_direct_transaction_collects_data);
  RUN_TEST(test_bridged_transaction_forwards_all_packets);
  RUN_TEST(test_bridged_transaction_timeout_and_reject);
  RUN_TEST(test_build_wire_command_limits);
  RUN_TEST(test_compare_versions);
  return UNITY_END();
}
