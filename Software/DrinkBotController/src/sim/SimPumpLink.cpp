#include "sim/SimPumpLink.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "core/Crc32.h"

namespace drinkbot {

namespace {

std::vector<std::string> tokenize(const std::string& s) {
  std::vector<std::string> tokens;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && s[i] == ' ') {
      i++;
    }
    size_t start = i;
    while (i < s.size() && s[i] != ' ') {
      i++;
    }
    if (i > start) {
      tokens.push_back(s.substr(start, i - start));
    }
  }
  return tokens;
}

}  // namespace

SimPumpLink::SimPumpLink(const IClock& clock, std::vector<PumpSpec> pumps) : clock_(clock) {
  for (const auto& spec : pumps) {
    Pump p;
    p.spec = spec;
    p.id = spec.id;
    p.weightG = kStartLevelMl * spec.densityGPerMl;
    pumps_.push_back(p);
  }
}

std::vector<SimPumpLink::PumpSpec> SimPumpLink::specsFromConfig(const Config& config) {
  std::vector<PumpSpec> specs;
  for (const auto& pc : config.pumps) {
    specs.push_back({pc.pumpId, pc.densityGPerMl, pc.msPerMl});
  }
  for (int id = 1; id <= kPumpCount && specs.size() < static_cast<size_t>(kPumpCount); id++) {
    bool used = std::any_of(specs.begin(), specs.end(), [id](const PumpSpec& s) { return s.id == id; });
    if (!used) {
      specs.push_back({id, 1.0f, 100});
    }
  }
  return specs;
}

std::vector<int> SimPumpLink::pumpIds() const {
  std::vector<int> ids;
  for (const auto& p : pumps_) {
    ids.push_back(p.id);
  }
  return ids;
}

SimPumpLink::Pump* SimPumpLink::find(int id) {
  for (auto& p : pumps_) {
    if (p.id == id) {
      return &p;
    }
  }
  return nullptr;
}

const SimPumpLink::Pump* SimPumpLink::find(int id) const {
  return const_cast<SimPumpLink*>(this)->find(id);
}

void SimPumpLink::schedule(uint32_t delayMs, const std::string& line) {
  outbox_.emplace(clock_.nowMs() + delayMs, line);
}

std::string SimPumpLink::wrapBridged(const std::string& payload) {
  std::string crcData = "$[" + payload + "|";
  char crc[9];
  snprintf(crc, sizeof(crc), "%08X", static_cast<unsigned>(crc32(crcData.data(), crcData.size())));
  return crcData + crc + "]";
}

bool SimPumpLink::isBroadcastCommand(const std::string& command) {
  std::string cmd = command.substr(0, command.find(' '));
  return cmd == "stop" || cmd == "ledcolor" || cmd == "ledmode" || cmd == "ledfade" || cmd == "ledchase" ||
         cmd == "ledtime";
}

bool SimPumpLink::writeLine(const std::string& line) {
  received_.push_back(line);
  uint32_t now = clock_.nowMs();
  for (auto& p : pumps_) {
    advance(p, now);
  }

  Pump* bridge = pumps_.empty() ? nullptr : &pumps_.front();
  if (!bridge) {
    return true;
  }

  std::vector<std::string> tokens = tokenize(line);
  if (tokens.empty()) {
    return true;
  }

  if (tokens[0] != "bridge") {
    // Direct command to the bridge pump itself
    if (bridge->ignoreCommands > 0) {
      bridge->ignoreCommands--;
      return true;
    }
    auto reply = execute(*bridge, line);
    if (bridge->dropResponses > 0) {
      bridge->dropResponses--;
      return true;
    }
    for (const auto& l : reply) {
      schedule(kDirectLatencyMs, l);
    }
    return true;
  }

  // bridge <id> <command...>
  int target = tokens.size() > 1 ? atoi(tokens[1].c_str()) : 0;
  size_t cmdPos = line.find(' ', line.find(' ') + 1);
  std::string inner = cmdPos == std::string::npos ? "" : line.substr(cmdPos + 1);

  if (tokens.size() < 3 || target < 1 || target > 255 || target == bridge->id ||
      (bridgePending_ && !timeReached(now, bridgePendingUntil_))) {
    schedule(kDirectLatencyMs, "R,ERR");
    return true;
  }

  if (target == 255) {
    if (!isBroadcastCommand(inner)) {
      schedule(kDirectLatencyMs, "R,ERR");
      return true;
    }
    std::vector<std::string> local;
    for (auto& p : pumps_) {
      if (p.missing) {
        continue;
      }
      auto reply = execute(p, inner);
      if (&p == bridge) {
        local = reply;
      }
    }
    for (const auto& l : local) {
      schedule(kDirectLatencyMs, l);
    }
    return true;
  }

  bridgePending_ = true;
  bridgePendingUntil_ = now + kBridgeTimeoutMs;
  Pump* p = find(target);
  if (!p || p->missing) {
    schedule(kBridgeTimeoutMs, "R,TIMEOUT");
    return true;
  }
  if (p->ignoreCommands > 0) {
    p->ignoreCommands--;
    schedule(kBridgeTimeoutMs, "R,TIMEOUT");
    return true;
  }
  auto reply = execute(*p, inner);
  if (p->dropResponses > 0) {
    p->dropResponses--;
    schedule(kBridgeTimeoutMs, "R,TIMEOUT");
    return true;
  }
  for (const auto& l : reply) {
    schedule(kBridgedLatencyMs, wrapBridged(l));
  }
  bridgePending_ = false;
  return true;
}

bool SimPumpLink::readLine(std::string& line) {
  if (outbox_.empty() || !timeReached(clock_.nowMs(), outbox_.begin()->first)) {
    return false;
  }
  line = outbox_.begin()->second;
  outbox_.erase(outbox_.begin());
  if (line == "R,TIMEOUT") {
    bridgePending_ = false;
  }
  return true;
}

void SimPumpLink::tick() {
  uint32_t now = clock_.nowMs();
  for (auto& p : pumps_) {
    advance(p, now);
  }
}

void SimPumpLink::advance(Pump& p, uint32_t now) {
  if (!p.active) {
    return;
  }
  uint32_t end = p.startMs + p.durationMs;
  uint32_t until = timeReached(now, end) ? end : now;
  uint32_t dt = until - p.lastUpdateMs;
  if (dt > 0 && p.spec.msPerMl > 0) {
    float ml = static_cast<float>(dt) / static_cast<float>(p.spec.msPerMl);
    float grams = ml * p.spec.densityGPerMl;
    p.weightG = p.reverse ? p.weightG + grams : std::max(0.0f, p.weightG - grams);
  }
  p.lastUpdateMs = until;
  if (until == end) {
    p.active = false;
  }
}

std::vector<std::string> SimPumpLink::execute(Pump& p, const std::string& command) {
  std::vector<std::string> out;
  std::vector<std::string> t = tokenize(command);
  if (t.empty()) {
    return out;
  }
  const std::string& cmd = t[0];
  size_t args = t.size() - 1;
  uint32_t now = clock_.nowMs();
  char buf[64];

  auto startMotion = [&](bool reverse, uint32_t ms) {
    if (p.active || ms == 0 || ms > kMaxMotionMs) {
      return false;
    }
    p.active = true;
    p.reverse = reverse;
    p.startMs = now;
    p.lastUpdateMs = now;
    p.durationMs = ms;
    return true;
  };

  bool ok = true;
  if (cmd == "version") {
    out.push_back("P,VER," + p.version);
  } else if (cmd == "gid") {
    out.push_back("P,ID," + std::to_string(p.id));
  } else if (cmd == "weight") {
    snprintf(buf, sizeof(buf), "P,WEIGHT,%f", p.weightG);
    out.push_back(buf);
  } else if (cmd == "dispense" && args > 0) {
    // Firmware 0.4.0: dispense <ms> [ledMode]
    ok = startMotion(false, strtoul(t[1].c_str(), nullptr, 10));
    if (ok && args > 1) {
      p.led.mode = atoi(t[2].c_str());
    }
  } else if ((cmd == "prime" || cmd == "purge") && args > 0) {
    ok = startMotion(cmd == "purge", strtoul(t[1].c_str(), nullptr, 10));
  } else if (cmd == "stop") {
    advance(p, now);
    p.active = false;
    p.stuck = false;
  } else if (cmd == "status") {
    const char* state = p.stuck ? "FWD" : (!p.active ? "IDLE" : (p.reverse ? "REV" : "FWD"));
    uint32_t remaining = p.stuck ? 1000 : 0;
    if (p.active && !p.stuck) {
      uint32_t elapsed = now - p.startMs;
      remaining = elapsed >= p.durationMs ? 0 : p.durationMs - elapsed;
    }
    snprintf(buf, sizeof(buf), "P,STATUS,%s,%u", state, static_cast<unsigned>(remaining));
    out.push_back(buf);
  } else if (cmd == "ledcolor" && args > 2) {
    p.led.r = static_cast<uint8_t>(atoi(t[1].c_str()));
    p.led.g = static_cast<uint8_t>(atoi(t[2].c_str()));
    p.led.b = static_cast<uint8_t>(atoi(t[3].c_str()));
  } else if (cmd == "ledmode" && args > 1) {
    p.led.mode = atoi(t[1].c_str());
    p.led.brightness = atoi(t[2].c_str());
  } else if ((cmd == "ledfade" && args > 3) || (cmd == "ledchase" && args > 1) || (cmd == "ledtime" && args > 0)) {
    // Timing parameters only affect the animation
  } else if (cmd == "tare") {
    p.weightG = 0;
  } else if (cmd == "cal" && args > 0) {
    // Calibration has no effect on the model
  } else if (cmd == "sid" && args > 0) {
    int id = atoi(t[1].c_str());
    ok = id >= 1 && id <= 254;
    if (ok) {
      p.id = id;
    }
  } else if (cmd == "scfg") {
  } else {
    out.push_back("R,INVLD");
    return out;
  }
  out.push_back(ok ? "R,OK" : "R,ERR");
  return out;
}

void SimPumpLink::setMissing(int id, bool missing) {
  if (Pump* p = find(id)) {
    p->missing = missing;
  }
}

void SimPumpLink::setWeight(int id, float grams) {
  if (Pump* p = find(id)) {
    p->weightG = grams;
  }
}

void SimPumpLink::setFirmwareVersion(int id, const std::string& version) {
  if (Pump* p = find(id)) {
    p->version = version;
  }
}

void SimPumpLink::setStuck(int id, bool stuck) {
  if (Pump* p = find(id)) {
    p->stuck = stuck;
  }
}

void SimPumpLink::dropNextResponse(int id) {
  if (Pump* p = find(id)) {
    p->dropResponses++;
  }
}

void SimPumpLink::ignoreNextCommand(int id) {
  if (Pump* p = find(id)) {
    p->ignoreCommands++;
  }
}

float SimPumpLink::weight(int id) const {
  const Pump* p = find(id);
  return p ? p->weightG : 0;
}

bool SimPumpLink::running(int id) const {
  const Pump* p = find(id);
  return p && p->active && !timeReached(clock_.nowMs(), p->startMs + p->durationMs);
}

bool SimPumpLink::reversing(int id) const {
  const Pump* p = find(id);
  return p && p->active && p->reverse;
}

int SimPumpLink::msPerMl(int id) const {
  const Pump* p = find(id);
  return p ? p->spec.msPerMl : 0;
}

SimPumpLink::LedState SimPumpLink::led(int id) const {
  const Pump* p = find(id);
  return p ? p->led : LedState();
}

}  // namespace drinkbot
