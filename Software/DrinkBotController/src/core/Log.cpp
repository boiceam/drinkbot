#include "core/Log.h"

#include <cstdio>

namespace drinkbot {

namespace {
const char* const kLevelNames[] = {"ERROR", "WARN", "INFO", "DEBUG", "VERBOSE"};
}

const char* logLevelName(LogLevel level) {
  return kLevelNames[static_cast<uint8_t>(level)];
}

bool parseLogLevel(const std::string& name, LogLevel& out) {
  for (uint8_t i = 0; i < 5; i++) {
    if (name == kLevelNames[i]) {
      out = static_cast<LogLevel>(i);
      return true;
    }
  }
  return false;
}

std::string formatLogLine(const LogEntry& entry) {
  char buf[Logger::kMaxMessage + 40];
  // Level is padded to 5 so the tags line up ("VERBOSE" is the one exception)
  snprintf(buf, sizeof(buf), "[%8lu] %-5s %s - %s", static_cast<unsigned long>(entry.t),
           logLevelName(entry.level), entry.tag.c_str(), entry.msg.c_str());
  return buf;
}

void Logger::addSink(ILogSink* sink, LogLevel maxLevel) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  sinks_.push_back({sink, maxLevel});
}

void Logger::setSinkLevel(ILogSink* sink, LogLevel maxLevel) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (auto& slot : sinks_) {
    if (slot.sink == sink) {
      slot.maxLevel = maxLevel;
    }
  }
}

void Logger::log(LogLevel level, const char* tag, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vlog(level, tag, fmt, args);
  va_end(args);
}

void Logger::vlog(LogLevel level, const char* tag, const char* fmt, va_list args) {
  LogEntry entry;
  entry.t = clock_ ? clock_->nowMs() : 0;
  entry.level = level;
  entry.tag = tag;
  char msg[kMaxMessage + 1];
  vsnprintf(msg, sizeof(msg), fmt, args);
  entry.msg = msg;

  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (level <= LogLevel::Info) {
    history_.push_back(entry);
    if (history_.size() > kHistorySize) {
      history_.pop_front();
    }
  }
  for (const auto& slot : sinks_) {
    if (level <= slot.maxLevel) {
      slot.sink->write(entry);
    }
  }
}

std::vector<LogEntry> Logger::history() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return {history_.begin(), history_.end()};
}

Logger& logger() {
  static Logger instance;
  return instance;
}

}  // namespace drinkbot
