#include "core/PumpProtocol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/Crc32.h"

namespace drinkbot {

namespace {

bool startsWith(const std::string& s, const char* prefix) {
  return s.compare(0, strlen(prefix), prefix) == 0;
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (true) {
    size_t pos = s.find(sep, start);
    parts.push_back(s.substr(start, pos - start));
    if (pos == std::string::npos) {
      break;
    }
    start = pos + 1;
  }
  return parts;
}

// Classifies an already-unwrapped payload.
void classifyPayload(ParsedLine& out) {
  const std::string& p = out.payload;
  if (startsWith(p, "R,")) {
    out.kind = ParsedLine::Kind::Result;
    std::string code = p.substr(2);
    if (code == "OK") {
      out.result = ResultCode::Ok;
    } else if (code == "ERR") {
      out.result = ResultCode::Err;
    } else if (code == "INVLD") {
      out.result = ResultCode::Invalid;
    } else if (code == "TIMEOUT") {
      out.result = ResultCode::Timeout;
    } else {
      out.kind = ParsedLine::Kind::Ignored;
    }
  } else if (startsWith(p, "P,")) {
    out.kind = ParsedLine::Kind::Data;
  } else {
    out.kind = ParsedLine::Kind::Ignored;
  }
}

}  // namespace

ParsedLine parseLine(const std::string& line) {
  ParsedLine out;
  if (startsWith(line, "$[")) {
    out.bridged = true;
    size_t pipe = line.rfind('|');
    if (pipe == std::string::npos || line.size() < pipe + 2 || line.back() != ']') {
      out.kind = ParsedLine::Kind::BadCrc;
      return out;
    }
    std::string crcText = line.substr(pipe + 1, line.size() - pipe - 2);
    if (crcText.empty() || crcText.size() > 8) {
      out.kind = ParsedLine::Kind::BadCrc;
      return out;
    }
    char* end = nullptr;
    uint32_t received = strtoul(crcText.c_str(), &end, 16);
    if (*end != '\0') {
      out.kind = ParsedLine::Kind::BadCrc;
      return out;
    }
    // CRC covers "$[<payload>|"
    if (crc32(line.data(), pipe + 1) != received) {
      out.kind = ParsedLine::Kind::BadCrc;
      return out;
    }
    out.payload = line.substr(2, pipe - 2);
  } else {
    out.payload = line;
  }
  classifyPayload(out);
  return out;
}

const char* txnStatusName(TxnStatus status) {
  switch (status) {
    case TxnStatus::Ok: return "OK";
    case TxnStatus::Err: return "ERR";
    case TxnStatus::Invalid: return "INVLD";
    case TxnStatus::Timeout: return "TIMEOUT";
    case TxnStatus::BadCrc: return "BAD_CRC";
    case TxnStatus::LinkError: return "LINK_ERROR";
  }
  return "?";
}

void TransactionParser::begin(bool bridged) {
  bridged_ = bridged;
  active_ = true;
  result_ = TxnResult();
}

void TransactionParser::finish(TxnStatus status) {
  result_.status = status;
  active_ = false;
}

bool TransactionParser::feed(const std::string& line) {
  if (!active_) {
    return false;
  }
  ParsedLine p = parseLine(line);

  if (p.kind == ParsedLine::Kind::BadCrc) {
    if (bridged_) {
      finish(TxnStatus::BadCrc);
      return true;
    }
    return false;
  }
  if (p.kind == ParsedLine::Kind::Ignored) {
    return false;
  }

  if (p.bridged != bridged_) {
    // A local result during a bridged request is the bridge itself
    // answering: R,TIMEOUT, or R,ERR/R,INVLD when it rejected the request.
    // (A local R,OK is the 0.2.0 bridge ack and is ignored.)
    if (bridged_ && !p.bridged && p.kind == ParsedLine::Kind::Result) {
      switch (p.result) {
        case ResultCode::Timeout: finish(TxnStatus::Timeout); return true;
        case ResultCode::Err: finish(TxnStatus::Err); return true;
        case ResultCode::Invalid: finish(TxnStatus::Invalid); return true;
        case ResultCode::Ok: return false;
      }
    }
    return false;
  }

  if (p.kind == ParsedLine::Kind::Data) {
    result_.data.push_back(p.payload);
    return false;
  }

  switch (p.result) {
    case ResultCode::Ok: finish(TxnStatus::Ok); break;
    case ResultCode::Err: finish(TxnStatus::Err); break;
    case ResultCode::Invalid: finish(TxnStatus::Invalid); break;
    case ResultCode::Timeout: finish(TxnStatus::Timeout); break;
  }
  return true;
}

bool buildWireCommand(bool direct, int pumpId, const std::string& command, std::string& out) {
  out.clear();
  if (command.empty() || command.size() > kMaxPumpCommand) {
    return false;
  }
  if (command.find_first_of("|]\n\r") != std::string::npos) {
    return false;
  }
  std::vector<std::string> tokens;
  for (const auto& t : split(command, ' ')) {
    if (!t.empty()) {
      tokens.push_back(t);
    }
  }
  if (tokens.empty() || tokens[0].size() > 15) {
    return false;
  }
  size_t maxTokens = direct ? 8 : 6;
  if (tokens.size() > maxTokens) {
    return false;
  }
  if (direct) {
    out = command;
    return true;
  }
  if (pumpId < 1 || pumpId > kBroadcastId) {
    return false;
  }
  out = "bridge " + std::to_string(pumpId) + " " + command;
  return true;
}

bool findDataFields(const TxnResult& result, const char* key, std::vector<std::string>& fields) {
  std::string prefix = std::string("P,") + key + ",";
  for (const auto& line : result.data) {
    if (startsWith(line, prefix.c_str())) {
      fields = split(line.substr(prefix.size()), ',');
      return true;
    }
  }
  return false;
}

bool parseWeight(const TxnResult& result, float& grams) {
  std::vector<std::string> f;
  if (!findDataFields(result, "WEIGHT", f) || f.empty()) {
    return false;
  }
  char* end = nullptr;
  grams = strtof(f[0].c_str(), &end);
  return end != f[0].c_str();
}

bool parseVersion(const TxnResult& result, std::string& version) {
  std::vector<std::string> f;
  if (!findDataFields(result, "VER", f) || f.empty() || f[0].empty()) {
    return false;
  }
  version = f[0];
  return true;
}

bool parseId(const TxnResult& result, int& id) {
  std::vector<std::string> f;
  if (!findDataFields(result, "ID", f) || f.empty()) {
    return false;
  }
  char* end = nullptr;
  long v = strtol(f[0].c_str(), &end, 10);
  if (end == f[0].c_str()) {
    return false;
  }
  id = static_cast<int>(v);
  return true;
}

bool parseStatus(const TxnResult& result, PumpMotion& motion, uint32_t& remainingMs) {
  std::vector<std::string> f;
  if (!findDataFields(result, "STATUS", f) || f.size() < 2) {
    return false;
  }
  if (f[0] == "IDLE") {
    motion = PumpMotion::Idle;
  } else if (f[0] == "FWD") {
    motion = PumpMotion::Forward;
  } else if (f[0] == "REV") {
    motion = PumpMotion::Reverse;
  } else {
    return false;
  }
  remainingMs = strtoul(f[1].c_str(), nullptr, 10);
  return true;
}

int compareVersions(const std::string& a, const std::string& b) {
  auto parts = [](const std::string& v) {
    std::vector<long> nums;
    std::string core = v.substr(0, v.find('-'));
    for (const auto& p : split(core, '.')) {
      nums.push_back(strtol(p.c_str(), nullptr, 10));
    }
    while (nums.size() < 3) {
      nums.push_back(0);
    }
    return nums;
  };
  auto pa = parts(a);
  auto pb = parts(b);
  for (size_t i = 0; i < 3; i++) {
    if (pa[i] != pb[i]) {
      return pa[i] < pb[i] ? -1 : 1;
    }
  }
  return 0;
}

}  // namespace drinkbot
