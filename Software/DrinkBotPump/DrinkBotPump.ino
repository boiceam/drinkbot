/*
 * DrinkBot Pump Controller
 * 
 * Hardware Target: Raspberry Pi Pico (RP2040)
 * IDE Requirement: Arduino IDE 2.3.10 or greater
 * 
 * Required Libraries:
 *  - HX711 (by Bogdan Necula)
 *  - Adafruit NeoPixel (by Adafruit)
 *  - ArduinoLog (by Thijs Elenbaas)
 * 
 * Debug Building:
 * Debug features (such as blocking wait for USB Serial on boot and extra bridge packet logging) 
 * are controlled via the Arduino IDE's built-in menus rather than manual macros.
 *  - To Enable Debugging: Go to Tools -> Debug Port -> "Serial" (or your preferred port).
 *  - To Disable Debugging (Production): Go to Tools -> Debug Port -> "Disabled". 
 * When a debug port is selected, the IDE defines `DEBUG_RP2040_PORT`, which this code 
 * uses to conditionally compile debug-only features.
 */

#include "HX711.h"
#include <EEPROM.h>
#include <ArduinoLog.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>
#include <LittleFS.h>
#include <Updater.h>

// --- Global Constants ---
constexpr uint32_t kLedBootAnimationTimeMs = 5000;
constexpr uint32_t kCommBusBaudRate = 50000;
constexpr uint32_t kBridgeTimeoutMs = 3000;  // > the ~1.8 s blocking tare/cal at 10 samples/s
constexpr uint8_t  kScaleReadingsPerMeasurement = 1;
constexpr uint32_t kScaleActionWaitTimeMs = 2000;
constexpr uint8_t  kScaleCalibrationReadings = 20;
constexpr uint8_t  kScaleWaitDelayMs = 5;
constexpr uint32_t kMaxMotionMs = 120000;
constexpr uint8_t  kBroadcastAddress = 255;
// ID 0 means "not a pump": a dedicated bridge board, or a pump with no ID yet.
// It relays bus traffic but won't run a motor, and the bus can't address it.
constexpr uint8_t  kBridgeBoardId = 0;

struct DeviceConfig {
  uint8_t id;
  float scaleFactor;
  int32_t scaleOffset;
};

// Constant parameters
constexpr int kMcuUart0TxPin = D0;
constexpr int kMcuUart0RxPin = D1;
constexpr int kPlcDirPin = D2;
constexpr int kPumpIn1Pin = D4;
constexpr int kPumpIn2Pin = D5;
constexpr int kGoLedPin = D11;
constexpr int kScaleDataPin = D15;
constexpr int kScaleClockPin = D16;
constexpr int kScaleRatePin = D17;
constexpr int kMcuClockoutPin = D21;
constexpr int kLedDataPin = D22;
constexpr int kMcuUart1TxPin = D24;
constexpr int kMcuUart1RxPin = D25;
constexpr int kMcuI2c0SdaPin = D28;
constexpr int kMcuI2c0SclPin = D29;

constexpr int kBusTransmit = HIGH;
constexpr int kBusReceive = LOW;

constexpr uint32_t kScaleTaskIntervalMs = 200;
constexpr uint32_t kUiTaskIntervalMs = 2000;
constexpr uint32_t kLedTaskIntervalMs = 20;

constexpr uint16_t kInputBufferSize = 256;  // fits a bridged fwdata chunk
constexpr uint16_t kMaxBridgeCommand = 200;
constexpr uint16_t kOutputBufferSize = 512;

constexpr uint8_t kMaxBytesPerCall = 32;
constexpr uint8_t kMaxTokens = 8;
constexpr uint8_t kMaxCommandLength = 15;

constexpr uint16_t kConfigEepromAddress = 0;
// Identifies a pump image: the update tool reads the version from this tag, and
// fwend refuses an image without one (a valid sketch that isn't pump firmware).
// `version` reads from it too, which also keeps the linker from removing it.
const char kImageTag[] = "DBP-IMAGE:0.7.0";
const char* kFirmwareVersion = kImageTag + 10;

constexpr uint8_t kPumpEnabled = 1;
constexpr uint8_t kPumpDisabled = 0;
constexpr uint8_t kPumpForward = 0;
constexpr uint8_t kPumpReverse = 1;

enum CommandSource {
  SRC_UART,
  SRC_COMMBUS,
  SRC_BROADCAST
};

// Loop Timers
uint32_t nextScaleTaskTime = 0;
uint32_t nextUiTaskTime = 0;
uint32_t nextLedTaskTime = 0;

char uartInputLineBuffer[kInputBufferSize];
uint16_t uartBufferLocation = 0;

char commBusInputLineBuffer[kInputBufferSize];
uint16_t commBusBufferLocation = 0;

char outputBuffer[kOutputBufferSize];

volatile float lastScaleReading;

DeviceConfig config;
HX711 scale;

CommandSource currentCommandSource = SRC_UART;
bool pendingBridgeResponse = false;
uint32_t bridgeTimeoutMs = 0;

// --- Pump State ---
uint32_t pumpStartTime = 0;
uint32_t pumpDurationMs = 0;
bool pumpActive = false;
uint8_t pumpDirection = kPumpForward;

// --- LED Configuration ---
constexpr uint16_t kNumLeds = 20;
Adafruit_NeoPixel strip(kNumLeds, kLedDataPin, NEO_GRB + NEO_KHZ800);

uint8_t ledMode = 1;
uint8_t ledR = 255;
uint8_t ledG = 0;
uint8_t ledB = 0;
uint8_t ledMaxBrightness = 255;

// LED Timer State
bool ledTimerActive = false;
uint32_t ledTimerEndTime = 0;
bool ledStopPending = false;

// Fade Mode (2) State
uint16_t ledFadeInMs = 1000;
uint16_t ledOnHoldMs = 500;
uint16_t ledFadeOutMs = 1000;
uint16_t ledOffHoldMs = 500;
uint8_t fadeState = 0; 
uint32_t fadeStateStartTime = 0;

// Chase Mode (3 & 4) State
uint16_t ledChaseSpeedMs = 75;
uint8_t ledChaseGradient = 4;
int8_t chaseLedIndex = 0;
int8_t chaseDirection = 1;
uint32_t lastChaseStepTime = 0;


// --- Firmware update over USB or the bus (see README "Firmware Update") ---
// The image is staged in LittleFS by the core's Updater; the OTA loader built
// into every image copies it into place on the next boot (power-fail safe).
enum FwState : uint8_t { FW_IDLE, FW_RECEIVING, FW_STAGED };
constexpr uint16_t kFwMaxChunk = 192;  // bytes per fwdata after base64 decoding
// boot2 and the OTA loader. An image must carry the same bytes as this pump's
// flash: that rejects junk at the first chunk, and a different core or flash
// layout, and means an update never rewrites the loader (so it stays power-fail
// safe). A build with a different core or layout must be flashed over USB.
constexpr uint32_t kFwLoaderBytes = 0x3000;
const char kFwTagPrefix[] = "DBP-IMAGE:";
constexpr uint8_t kFwTagLength = sizeof(kFwTagPrefix) - 1;
uint8_t fwTagMatched = 0;
FwState fwState = FW_IDLE;
uint32_t fwSize = 0;
uint32_t fwReceived = 0;
uint16_t fwLastChunk = 0;

int base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

// Decodes standard base64 (with optional '=' padding). Returns the byte
// count, or -1 on bad input or overflow.
int base64Decode(const char* in, uint8_t* out, int maxOut) {
  int len = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (const char* p = in; *p && *p != '='; p++) {
    int v = base64Value(*p);
    if (v < 0) return -1;
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (len >= maxOut) return -1;
      out[len++] = (acc >> bits) & 0xFF;
    }
  }
  return len;
}

// Checks a new chunk at `offset` against the rules above and looks for the tag.
bool fwCheckChunk(uint32_t offset, const uint8_t* chunk, int n) {
  const uint8_t* flash = (const uint8_t*)XIP_BASE;
  for (int i = 0; i < n; i++) {
    if (offset + i < kFwLoaderBytes && chunk[i] != flash[offset + i]) {
      return false;
    }
    if (fwTagMatched < kFwTagLength) {
      if (chunk[i] == (uint8_t)kFwTagPrefix[fwTagMatched]) {
        fwTagMatched++;
      } else {
        fwTagMatched = chunk[i] == (uint8_t)kFwTagPrefix[0] ? 1 : 0;
      }
    }
  }
  return true;
}

void fwReset() {
  if (fwState == FW_RECEIVING) {
    Update.end(false);  // not finished: discards the partial image
  } else if (fwState == FW_STAGED) {
    // Un-stage: without the command file the OTA loader boots the current image
    LittleFS.remove("otacommand.bin");
  }
  fwState = FW_IDLE;
  fwSize = 0;
  fwReceived = 0;
  fwLastChunk = 0;
  fwTagMatched = 0;
}

void setup() {
  Serial.begin(115200);

#if defined(DEBUG_RP2040_PORT)
  while (!DEBUG_RP2040_PORT) {
    yield();
  }
  Log.begin(LOG_LEVEL_VERBOSE, &DEBUG_RP2040_PORT);
#else
  Log.begin(LOG_LEVEL_SILENT, &Serial);
#endif

  Log.noticeln("SYS - Starting DrinkBot Pump");

  EEPROM.begin(128);
  loadConfig();

  initUi();
  initCommBus();
  initPump();
  initScale();
  initLeds();
}

void loop() {
  auto currentMillis = millis();
  
  // Read the scale once a reading is due AND the HX711 has one ready, so the
  // loop never waits on it (waiting stalled the 20 ms LED frames).
  if (currentMillis >= nextScaleTaskTime && scaleTask()) {
    nextScaleTaskTime = currentMillis + kScaleTaskIntervalMs;
  }

  if (currentMillis >= nextUiTaskTime) {
    nextUiTaskTime = currentMillis + kUiTaskIntervalMs;
    uiTask();
  }

  if (currentMillis >= nextLedTaskTime) {
    nextLedTaskTime = currentMillis + kLedTaskIntervalMs;
    ledTask();
  }

  pumpTask();

  if (pendingBridgeResponse) {
    if (currentMillis > bridgeTimeoutMs) {
      pendingBridgeResponse = false;
      Serial.write("R,TIMEOUT\n");
    }
  }

  processUartInput();
  processCommBus();
  yield();
}

// --- CRC32 & RS485 Utilities ---

uint32_t calculateCRC32(const char* data, size_t length) {
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

void sendCommBusResponse(const char* responseText) {
  char packet[256];
  char crcData[256];
  char cleanResp[128];
  
  strncpy(cleanResp, responseText, 127);
  cleanResp[127] = '\0';
  size_t len = strlen(cleanResp);
  
  if (len > 0) {
    if (cleanResp[len - 1] == '\n') {
      cleanResp[len - 1] = '\0';
    }
  }

  sprintf(crcData, "$[%s|", cleanResp);
  uint32_t crc = calculateCRC32(crcData, strlen(crcData));
  
  sprintf(packet, "%s%08lX]", crcData, (unsigned long)crc);

  transmitCommBusPacket(packet);
}

void transmitCommBusPacket(const char* packet) {
  digitalWrite(kPlcDirPin, kBusTransmit);
  Serial1.write(packet, strlen(packet));
  Serial1.flush();
  digitalWrite(kPlcDirPin, kBusReceive);
}

// Only commands that are safe to run on every pump at once, and that need no reply.
bool isBroadcastCommand(const char* cmd) {
  static const char* kAllowed[] = { "stop", "ledcolor", "ledmode", "ledfade", "ledchase", "ledtime" };
  size_t len = strcspn(cmd, " ");
  for (size_t i = 0; i < sizeof(kAllowed) / sizeof(kAllowed[0]); i++) {
    if (strlen(kAllowed[i]) == len && strncmp(kAllowed[i], cmd, len) == 0) {
      return true;
    }
  }
  return false;
}

void parseCommBusPacket(char* packet) {
  if (packet[0] == '$') {
    if (packet[1] == '[') {
      if (pendingBridgeResponse) {
#if defined(DEBUG_RP2040_PORT)
        Log.verboseln("BRIDGE - Received: %s", packet);
#endif
        Serial.write(packet, strlen(packet));
        Serial.write('\n');
        // Keep forwarding until the slave's terminal R,<result> packet arrives
        if (strncmp(packet + 2, "R,", 2) == 0) {
          pendingBridgeResponse = false;
        }
      }
      return;
    }
  }

  if (packet[0] != '*') {
    return;
  }
  if (packet[1] != '[') {
    return;
  }
  
  char* firstPipe = strchr(packet, '|');
  if (firstPipe == NULL) {
    return;
  }
  
  char* secondPipe = strchr(firstPipe + 1, '|');
  if (secondPipe == NULL) {
    return;
  }
  
  size_t hashLen = (secondPipe - packet) + 1;
  uint32_t calculatedCrc = calculateCRC32(packet, hashLen);
  
  char* crcEnd = strchr(secondPipe + 1, ']');
  if (crcEnd == NULL) {
    return;
  }
  
  char crcStr[9];
  size_t crcLen = crcEnd - (secondPipe + 1);
  if (crcLen > 8) {
    return; 
  }
  
  strncpy(crcStr, secondPipe + 1, crcLen);
  crcStr[crcLen] = '\0';
  uint32_t receivedCrc = strtoul(crcStr, NULL, 16);
  
  if (calculatedCrc != receivedCrc) {
    Log.warningln("COMM - CRC mismatch");
    return;
  }
  
  char addrStr[16];
  size_t addrLen = firstPipe - (packet + 2);
  if (addrLen > 15) {
    addrLen = 15;
  }
  strncpy(addrStr, packet + 2, addrLen);
  addrStr[addrLen] = '\0';
  int targetAddr = atoi(addrStr);
  bool isBroadcast = (targetAddr == kBroadcastAddress);

  if (!isBroadcast && targetAddr != config.id) {
    return;
  }

  char cmdStr[kInputBufferSize];
  size_t cmdLen = secondPipe - (firstPipe + 1);
  if (cmdLen > (kInputBufferSize - 1)) {
    cmdLen = kInputBufferSize - 1;
  }
  strncpy(cmdStr, firstPipe + 1, cmdLen);
  cmdStr[cmdLen] = '\0';

  if (isBroadcast) {
    if (!isBroadcastCommand(cmdStr)) {
      Log.warningln("COMM - Ignoring non-broadcast command sent to broadcast address");
      return;
    }
    currentCommandSource = SRC_BROADCAST;
  } else {
    currentCommandSource = SRC_COMMBUS;
  }
  processCommand(cmdStr);
  currentCommandSource = SRC_UART;
}

// --- General Hardware Logic ---

void initLeds() {
  strip.begin();
  strip.show();
  Log.noticeln("LED - Initialized %d NeoPixels", kNumLeds);

  ledR = 255;
  ledG = 0;
  ledB = 0;
  ledMode = 2;
  ledMaxBrightness = 255;
  
  ledFadeInMs = 300;
  ledOnHoldMs = 200;
  ledFadeOutMs = 300;
  ledOffHoldMs = 200;
  
  fadeState = 0;
  fadeStateStartTime = millis();

  ledTimerActive = true;
  ledTimerEndTime = millis() + kLedBootAnimationTimeMs;
  ledStopPending = false;
}

uint16_t fadePhaseMs(uint8_t state) {
  switch (state) {
    case 0: return ledFadeInMs;
    case 1: return ledOnHoldMs;
    case 2: return ledFadeOutMs;
    default: return ledOffHoldMs;
  }
}

void ledTask() {
  uint32_t currentMillis = millis();
  
  if (ledTimerActive) {
    if (currentMillis >= ledTimerEndTime) {
      ledTimerActive = false;
      ledStopPending = true;
    }
  }

  switch(ledMode) {
    case 1: 
    {
      if (ledStopPending) {
        ledStopPending = false;
        ledMaxBrightness = 0;
      }
      
      strip.setBrightness(ledMaxBrightness);
      for(int i = 0; i < kNumLeds; i++) {
        strip.setPixelColor(i, strip.Color(ledR, ledG, ledB));
      }
      strip.show();
      break;
    }

    case 2: 
    {
      // Step past every phase whose time is used up, carrying the overshoot
      // into the next phase, so a boundary frame is always drawn with the new
      // phase's level (no black frame at the peak or bright frame at the end).
      uint32_t elapsed = currentMillis - fadeStateStartTime;
      for (uint8_t guard = 0; guard < 4; guard++) {
        uint16_t phaseMs = fadePhaseMs(fadeState);
        if (elapsed < phaseMs) {
          break;
        }
        if (fadeState == 3 && ledStopPending) {
          ledStopPending = false;
          ledMode = 1;
          ledMaxBrightness = 0;
          return;
        }
        elapsed -= phaseMs;
        fadeStateStartTime += phaseMs;
        fadeState = (fadeState + 1) % 4;
      }

      float level = 0.0;
      uint16_t phaseMs = fadePhaseMs(fadeState);
      if (fadeState == 0) {
        level = (phaseMs > 0) ? (float)elapsed / phaseMs : 1.0;
      } else if (fadeState == 1) {
        level = 1.0;
      } else if (fadeState == 2) {
        level = (phaseMs > 0) ? 1.0 - (float)elapsed / phaseMs : 0.0;
      }
      if (level < 0.0) level = 0.0;
      if (level > 1.0) level = 1.0;

      // Gamma 2.2 so the fade looks even to the eye instead of rushing
      // through the dim end and lingering near full brightness.
      uint8_t currentBrightness = (uint8_t)(ledMaxBrightness * powf(level, 2.2f) + 0.5f);

      strip.setBrightness(currentBrightness);
      for(int i = 0; i < kNumLeds; i++) {
        strip.setPixelColor(i, strip.Color(ledR, ledG, ledB));
      }
      strip.show();
      break;
    }

    case 3: 
    case 4: 
    {
      if (currentMillis - lastChaseStepTime >= ledChaseSpeedMs) {
        lastChaseStepTime = currentMillis;
        chaseLedIndex += chaseDirection;

        if (ledMode == 3) {
          if (chaseLedIndex >= kNumLeds) {
            chaseLedIndex = 0;
            if (ledStopPending) {
              ledStopPending = false;
              ledMode = 1;
              ledMaxBrightness = 0;
              return;
            }
          } else if (chaseLedIndex < 0) {
            chaseLedIndex = kNumLeds - 1;
            if (ledStopPending) {
              ledStopPending = false;
              ledMode = 1;
              ledMaxBrightness = 0;
              return;
            }
          }
        } else {
          if (chaseLedIndex >= kNumLeds - 1) { 
            chaseLedIndex = kNumLeds - 1; 
            chaseDirection = -1; 
          } else if (chaseLedIndex <= 0) { 
            chaseLedIndex = 0; 
            chaseDirection = 1; 
            if (ledStopPending) {
              ledStopPending = false;
              ledMode = 1;
              ledMaxBrightness = 0;
              return;
            }
          }
        }
      }

      strip.clear();
      strip.setBrightness(ledMaxBrightness);
      
      for (int offset = -ledChaseGradient; offset <= ledChaseGradient; offset++) {
        int drawIndex = chaseLedIndex + offset;
        
        if (ledMode == 3) {
          if (drawIndex < 0) {
            drawIndex += kNumLeds;
          }
          if (drawIndex >= kNumLeds) {
            drawIndex -= kNumLeds;
          }
        } else {
          if (drawIndex < 0 || drawIndex >= kNumLeds) {
            continue;
          }
        }

        float intensity = 1.0 - ((float)abs(offset) / (ledChaseGradient + 1));
        strip.setPixelColor(drawIndex, strip.Color(ledR * intensity, ledG * intensity, ledB * intensity));
      }
      strip.show();
      break;
    }
  }
}

void initUi() {
  digitalWrite(kGoLedPin, HIGH);
  pinMode(kGoLedPin, OUTPUT);
}

void initCommBus() {
  digitalWrite(kPlcDirPin, kBusReceive);

  pinMode(kMcuUart0TxPin, OUTPUT);
  pinMode(kMcuUart0RxPin, INPUT);
  pinMode(kPlcDirPin, OUTPUT);

  Serial1.begin(kCommBusBaudRate);
}

void initPump() {
  digitalWrite(kPumpIn1Pin, kPumpDisabled);
  digitalWrite(kPumpIn2Pin, kPumpDisabled);
  pinMode(kPumpIn1Pin, OUTPUT);
  pinMode(kPumpIn2Pin, OUTPUT);
}

bool startPump(uint8_t direction, uint32_t durationMs) {
  if (pumpActive) {
    Log.warningln("PUMP - Busy, rejecting new motion command");
    return false;
  }
  if (fwState != FW_IDLE) {
    Log.warningln("PUMP - Firmware update in progress, rejecting motion command");
    return false;
  }
  if (config.id == kBridgeBoardId) {
    Log.warningln("PUMP - ID 0 is a bridge board, not a pump: rejecting motion command");
    return false;
  }
  if (durationMs == 0 || durationMs > kMaxMotionMs) {
    Log.warningln("PUMP - Invalid motion duration: %l ms", durationMs);
    return false;
  }

  pumpDirection = direction;
  if (direction == kPumpForward) {
    digitalWrite(kPumpIn1Pin, kPumpDisabled);
    digitalWrite(kPumpIn2Pin, kPumpEnabled);
  } else {
    digitalWrite(kPumpIn1Pin, kPumpEnabled);
    digitalWrite(kPumpIn2Pin, kPumpDisabled);
  }
  pumpStartTime = millis();
  pumpDurationMs = durationMs;
  pumpActive = true;
  return true;
}

void stopPump() {
  pumpActive = false;
  digitalWrite(kPumpIn1Pin, kPumpDisabled);
  digitalWrite(kPumpIn2Pin, kPumpDisabled);
}

uint32_t pumpRemainingMs() {
  if (!pumpActive) {
    return 0;
  }
  uint32_t elapsed = millis() - pumpStartTime;
  if (elapsed >= pumpDurationMs) {
    return 0;
  }
  return pumpDurationMs - elapsed;
}

void pumpTask() {
  if (pumpActive) {
    // Subtraction keeps this correct across millis() rollover
    if ((millis() - pumpStartTime) >= pumpDurationMs) {
      stopPump();
    }
  }
}

void initScale() {
  Log.noticeln("SCALE - Intializing scale...");

  digitalWrite(kScaleClockPin, 0);
  digitalWrite(kScaleDataPin, 0);
  digitalWrite(kScaleRatePin, 0);

  pinMode(kScaleClockPin, OUTPUT);
  pinMode(kScaleDataPin, INPUT);
  pinMode(kScaleRatePin, OUTPUT);

  scale.begin(kScaleDataPin, kScaleClockPin, true, true);
  scale.set_offset(config.scaleOffset);
  scale.set_scale(config.scaleFactor);

  Log.verboseln("SCALE - Using scale offset: %d", config.scaleOffset);
  Log.verboseln("SCALE - Using scale factor: %F", config.scaleFactor);

  if (scale.wait_ready_timeout(kScaleActionWaitTimeMs, kScaleWaitDelayMs)) {
    Log.verboseln("SCALE - scale is ready to use");
  } else {
    Log.errorln("SCALE - scale is not ready to use");
  }
}

bool tareScale() {
  if (!scale.wait_ready_timeout(kScaleActionWaitTimeMs, kScaleWaitDelayMs)) {
    Log.errorln("SCALE - scale not ready");
    return false;
  }
  scale.tare(kScaleCalibrationReadings);
  config.scaleOffset = scale.get_offset();
  saveConfig();

  Log.verboseln("SCALE - Measured tare offset: %d", config.scaleOffset);
  return true;
}

bool calibrateScale(float weight) {
  if (!scale.wait_ready_timeout(kScaleActionWaitTimeMs, kScaleWaitDelayMs)) {
    Log.errorln("SCALE - scale not ready");
    return false;
  }
  scale.calibrate_scale(weight, kScaleCalibrationReadings);
  config.scaleFactor = scale.get_scale();
  saveConfig();

  Log.verboseln("SCALE - Calibrated with weight: %F", weight);
  Log.verboseln("SCALE - Measured scale factor: %F", config.scaleFactor);

  return true;
}

void saveConfig() {
  EEPROM.put(kConfigEepromAddress, config);
  EEPROM.commit();
  Log.verboseln("EEPROM - Device config saved.");
}

void loadConfig() {
  EEPROM.get(kConfigEepromAddress, config);
  Log.verboseln("EEPROM - Device config loaded.");

  if (config.id == 255) {
    Log.warningln("EEPROM - Invaild config detected. Loading default config.");
    clearConfig();
    saveConfig();
  }
}

void clearConfig() {
  config.id = 0;
  config.scaleOffset = 0.0;
  config.scaleFactor = 1.0;
  saveConfig();
}

// Returns true if a reading was taken; never waits for the HX711.
bool scaleTask() {
  if (!scale.is_ready()) {
    return false;
  }
  lastScaleReading = scale.get_units(kScaleReadingsPerMeasurement);
  return true;
}

void uiTask() {
  Log.verboseln("UPDATE - Weight: %F", lastScaleReading);
  digitalWrite(kGoLedPin, !digitalRead(kGoLedPin));
}

void processUartInput() {
  for (size_t i = 0; i < kMaxBytesPerCall; i++) {
    if (!Serial.available()) {
      break;
    }
    
    uartInputLineBuffer[uartBufferLocation] = Serial.read();
    uartBufferLocation++;
    uartInputLineBuffer[uartBufferLocation] = '\0';
    
    if (uartBufferLocation == (kInputBufferSize - 1)) {
      uartBufferLocation = 0;
      uartInputLineBuffer[uartBufferLocation] = '\0';
    } else if (uartInputLineBuffer[uartBufferLocation - 1] == '\n') {
      uartInputLineBuffer[uartBufferLocation - 1] = '\0';
      processCommand(uartInputLineBuffer);
      uartBufferLocation = 0;
      uartInputLineBuffer[uartBufferLocation] = '\0';
    }
  }
}

void processCommBus() {
  for (size_t i = 0; i < kMaxBytesPerCall; i++) {
    if (!Serial1.available()) {
      break;
    }
    
    char c = Serial1.read();

    // '*' and '$' only ever start a packet: drop any partial or noisy bytes
    // before them, so line noise can't spoil the next good packet.
    if (c == '*' || c == '$') {
      commBusBufferLocation = 0;
    }
    
    if (commBusBufferLocation < (kInputBufferSize - 1)) {
      commBusInputLineBuffer[commBusBufferLocation] = c;
      commBusBufferLocation++;
      commBusInputLineBuffer[commBusBufferLocation] = '\0';
    } else {
      commBusBufferLocation = 0;
      commBusInputLineBuffer[commBusBufferLocation] = '\0';
    }
    
    if (c == ']') {
      parseCommBusPacket(commBusInputLineBuffer);
      commBusBufferLocation = 0;
      commBusInputLineBuffer[commBusBufferLocation] = '\0';
    }
  }
}

void writeOutput(char* buf, uint16_t count) {
  if (currentCommandSource == SRC_BROADCAST) {
    return;
  } else if (currentCommandSource == SRC_UART) {
    Serial.write(buf, count);
  } else {
    char temp[kOutputBufferSize];
    strncpy(temp, buf, count);
    temp[count] = '\0';
    sendCommBusResponse(temp);
  }
}

void writeOutput(const char* buf) {
  if (currentCommandSource == SRC_BROADCAST) {
    return;
  } else if (currentCommandSource == SRC_UART) {
    Serial.write(buf, strlen(buf));
  } else {
    sendCommBusResponse(buf);
  }
}

void processCommand(char* buf) {
  Log.verboseln("CMD - Got command message: %s", buf);

  char* token[kMaxTokens];
  uint8_t count = 0;

  token[0] = strtok(buf, " \r");
  if (!token[0]) {
    // Blank line: nothing to do, and strncmp on NULL would crash
    return;
  }
  for (count = 1; count < kMaxTokens; count++) {
    token[count] = strtok(NULL, " \r");
    if (!token[count]) {
      break;
    }
  }

  char* command = token[0];
  uint8_t argCount = count - 1;
  bool result = true;

  if (strncmp("sid", command, kMaxCommandLength) == 0 && argCount > 0) {
    int newId = atoi(token[1]);
    // 255 is the broadcast address
    if (newId >= 1 && newId <= 254) {
      config.id = newId;
    } else if (strcmp(token[1], "0") == 0 && currentCommandSource == SRC_UART) {
      // USB only: over the bus this would leave a pump nothing can address
      config.id = kBridgeBoardId;
    } else {
      result = false;
    }
  } else if (strncmp("gid", command, kMaxCommandLength) == 0) {
    char outBuf[32];
    sprintf(outBuf, "P,ID,%d\n", config.id);
    writeOutput(outBuf);
  } else if (strncmp("ledcolor", command, kMaxCommandLength) == 0 && argCount > 2) {
    ledR = atoi(token[1]);
    ledG = atoi(token[2]);
    ledB = atoi(token[3]);
  } else if (strncmp("ledmode", command, kMaxCommandLength) == 0 && argCount > 1) {
    ledMode = atoi(token[1]);
    ledMaxBrightness = atoi(token[2]);
    fadeState = 0; 
    fadeStateStartTime = millis(); 
  } else if (strncmp("ledfade", command, kMaxCommandLength) == 0 && argCount > 3) {
    ledFadeInMs = atoi(token[1]);
    ledOnHoldMs = atoi(token[2]);
    ledFadeOutMs = atoi(token[3]);
    ledOffHoldMs = atoi(token[4]);
  } else if (strncmp("ledchase", command, kMaxCommandLength) == 0 && argCount > 1) {
    ledChaseSpeedMs = atoi(token[1]);
    ledChaseGradient = atoi(token[2]);
  } else if (strncmp("ledtime", command, kMaxCommandLength) == 0 && argCount > 0) {
    uint32_t duration = strtoul(token[1], NULL, 10);
    if (duration > 0) {
      ledTimerActive = true;
      ledTimerEndTime = millis() + duration;
      ledStopPending = false;
    } else {
      ledTimerActive = false;
      ledStopPending = false;
    }
  } else if (strncmp("dispense", command, kMaxCommandLength) == 0 && argCount > 0) {
    // The controller converts volume to run time; the pump only runs for <ms>
    uint32_t duration = strtoul(token[1], NULL, 10);
    uint8_t dispLedMode = 0;
    if (argCount > 1) {
      dispLedMode = atoi(token[2]);
    }

    // startPump rejects 0, anything over kMaxMotionMs, and a busy pump
    result = startPump(kPumpForward, duration);

    if (result && dispLedMode > 0) {
      ledMode = dispLedMode;
      ledTimerActive = true;
      ledTimerEndTime = millis() + duration;
      ledStopPending = false;
      if (ledMaxBrightness == 0) {
         ledMaxBrightness = 255;
      }
    }
  } else if (strncmp("prime", command, kMaxCommandLength) == 0 && argCount > 0) {
    result = startPump(kPumpForward, strtoul(token[1], NULL, 10));
  } else if (strncmp("purge", command, kMaxCommandLength) == 0 && argCount > 0) {
    result = startPump(kPumpReverse, strtoul(token[1], NULL, 10));
  } else if (strncmp("stop", command, kMaxCommandLength) == 0) {
    stopPump();
    ledTimerActive = false;
    ledStopPending = false;
    Log.noticeln("PUMP - Stopped by command");
  } else if (strncmp("status", command, kMaxCommandLength) == 0) {
    const char* state = "IDLE";
    if (pumpActive) {
      state = (pumpDirection == kPumpForward) ? "FWD" : "REV";
    }
    char outBuf[48];
    sprintf(outBuf, "P,STATUS,%s,%lu\n", state, (unsigned long)pumpRemainingMs());
    writeOutput(outBuf);
  } else if (strncmp("tare", command, kMaxCommandLength) == 0) {
    result = tareScale();
  } else if (strncmp("cal", command, kMaxCommandLength) == 0 && argCount > 0) {
    result = calibrateScale(atof(token[1]));
  } else if (strncmp("weight", command, kMaxCommandLength) == 0) {
    char outBuf[32];
    sprintf(outBuf, "P,WEIGHT,%f\n", lastScaleReading);
    writeOutput(outBuf);
  } else if (strncmp("scfg", command, kMaxCommandLength) == 0) {
    saveConfig();
  } else if (strncmp("fwbegin", command, kMaxCommandLength) == 0 && argCount > 1) {
    // fwbegin <size> <md5hex>: start receiving an image (uncompressed .bin only)
    uint32_t size = strtoul(token[1], NULL, 10);
    fwReset();
    result = !pumpActive && size > 0 && strlen(token[2]) == 32 && Update.begin(size) && Update.setMD5(token[2]);
    if (result) {
      fwState = FW_RECEIVING;
      fwSize = size;
    } else {
      fwReset();
    }
  } else if (strncmp("fwdata", command, kMaxCommandLength) == 0 && argCount > 1) {
    // fwdata <offset> <base64>: chunks must arrive in order. A repeat of the
    // last chunk (a retry after a lost reply) is acknowledged, not rewritten.
    uint32_t offset = strtoul(token[1], NULL, 10);
    uint8_t chunk[kFwMaxChunk];
    int n = base64Decode(token[2], chunk, sizeof(chunk));
    if (fwState != FW_RECEIVING || n <= 0) {
      result = false;
    } else if (offset == fwReceived && fwReceived + n <= fwSize) {
      result = fwCheckChunk(offset, chunk, n) && Update.write(chunk, n) == (size_t)n;
      if (result) {
        fwReceived += n;
        fwLastChunk = n;
      } else {
        fwReset();
      }
    } else if (offset + n == fwReceived && n == fwLastChunk) {
      result = true;
    } else {
      result = false;
    }
  } else if (strncmp("fwend", command, kMaxCommandLength) == 0) {
    // Verifies size and MD5, then stages the image for the OTA loader
    // Safe to repeat: a retry after a lost reply must not un-stage the image
    if (fwState != FW_STAGED) {
      result = fwState == FW_RECEIVING && fwReceived == fwSize && fwTagMatched == kFwTagLength && Update.end();
      if (result) {
        fwState = FW_STAGED;
      } else {
        fwReset();
      }
    }
  } else if (strncmp("fwapply", command, kMaxCommandLength) == 0) {
    // Reply first, then reboot into the OTA loader, which installs the image
    if (fwState == FW_STAGED && !pumpActive) {
      writeOutput("R,OK\n");
      Serial.flush();
      delay(100);
      rp2040.reboot();
    }
    result = false;
  } else if (strncmp("fwabort", command, kMaxCommandLength) == 0) {
    fwReset();
  } else if (strncmp("fwstat", command, kMaxCommandLength) == 0) {
    static const char* kNames[] = { "IDLE", "RECEIVING", "STAGED" };
    char outBuf[64];
    sprintf(outBuf, "P,FW,%s,%lu,%lu\n", kNames[fwState], (unsigned long)fwReceived, (unsigned long)fwSize);
    writeOutput(outBuf);
  } else if (strncmp("version", command, kMaxCommandLength) == 0) {
    char version[16];
    sprintf(version, "P,VER,%s\n", kFirmwareVersion);
    writeOutput(version);
  } else if (strncmp("bridge", command, kMaxCommandLength) == 0 && argCount > 1 && currentCommandSource == SRC_UART) {
    int targetAddr = atoi(token[1]);
    bool isBroadcast = (targetAddr == kBroadcastAddress);

    char bridgeCmd[kMaxBridgeCommand + 1];
    bridgeCmd[0] = '\0';

    for (int i = 2; i < count; i++) {
      if (strlen(bridgeCmd) + strlen(token[i]) + 2 > sizeof(bridgeCmd)) {
        Log.warningln("BRIDGE - Command too long");
        writeOutput("R,ERR\n");
        return;
      }
      strcat(bridgeCmd, token[i]);
      if (i < count - 1) {
        strcat(bridgeCmd, " ");
      }
    }

    if (targetAddr < 1 || targetAddr > kBroadcastAddress || targetAddr == config.id) {
      // Out of range, or our own address (nobody on the bus would answer)
      writeOutput("R,ERR\n");
      return;
    }
    if (pendingBridgeResponse) {
      Log.warningln("BRIDGE - Busy, previous request still pending");
      writeOutput("R,ERR\n");
      return;
    }
    if (isBroadcast && !isBroadcastCommand(bridgeCmd)) {
      writeOutput("R,ERR\n");
      return;
    }

    char crcData[kMaxBridgeCommand + 32];
    sprintf(crcData, "*[%d|%s|", targetAddr, bridgeCmd);
    uint32_t crc = calculateCRC32(crcData, strlen(crcData));

    char packet[kMaxBridgeCommand + 48];
    sprintf(packet, "%s%08lX]", crcData, (unsigned long)crc);

#if defined(DEBUG_RP2040_PORT)
    Log.verboseln("BRIDGE - Sent: %s", packet);
#endif

    transmitCommBusPacket(packet);

    if (isBroadcast) {
      // Slaves don't reply to broadcasts. Run it here too; its local
      // R,<result> is the response the host sees.
      processCommand(bridgeCmd);
    } else {
      // No local ack: the forwarded $[...] packets (or R,TIMEOUT) are the response
      pendingBridgeResponse = true;
      bridgeTimeoutMs = millis() + kBridgeTimeoutMs;
    }
    return;
  } else {
    Log.warning("CMD - Invalid Command");
    writeOutput("R,INVLD\n");
    return;
  }

  if (result) {
    writeOutput("R,OK\n");
  } else {
    writeOutput("R,ERR\n");
  }
}