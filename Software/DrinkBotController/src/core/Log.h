#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "core/Clock.h"

namespace drinkbot {

// SPEC_v3 §12
enum class LogLevel : uint8_t { Error = 0, Warn, Info, Debug, Verbose };

const char* logLevelName(LogLevel level);
bool parseLogLevel(const std::string& name, LogLevel& out);

struct LogEntry {
  uint32_t t = 0;
  LogLevel level = LogLevel::Info;
  std::string tag;  // <= 4 chars
  std::string msg;
};

// "[  123456] INFO  DISP - message"
std::string formatLogLine(const LogEntry& entry);

class ILogSink {
 public:
  virtual ~ILogSink() = default;
  virtual void write(const LogEntry& entry) = 0;
};

class Logger {
 public:
  static constexpr size_t kHistorySize = 300;
  static constexpr size_t kMaxMessage = 200;

  void setClock(const IClock* clock) { clock_ = clock; }

  // A sink only receives entries at or above (more severe than) its level.
  void addSink(ILogSink* sink, LogLevel maxLevel);
  void setSinkLevel(ILogSink* sink, LogLevel maxLevel);

  void log(LogLevel level, const char* tag, const char* fmt, ...)
      __attribute__((format(printf, 4, 5)));
  void vlog(LogLevel level, const char* tag, const char* fmt, va_list args);

  // Ring buffer of the most recent entries (INFO and more severe are kept
  // regardless of sink levels; DEBUG/VERBOSE are not stored).
  std::vector<LogEntry> history() const;

 private:
  struct SinkSlot {
    ILogSink* sink;
    LogLevel maxLevel;
  };

  const IClock* clock_ = nullptr;
  std::vector<SinkSlot> sinks_;
  std::deque<LogEntry> history_;
  mutable std::recursive_mutex mutex_;
};

// Process-wide logger used by all modules.
Logger& logger();

}  // namespace drinkbot

#define DB_LOG(level, tag, ...) ::drinkbot::logger().log(level, tag, __VA_ARGS__)
#define LOG_E(tag, ...) DB_LOG(::drinkbot::LogLevel::Error, tag, __VA_ARGS__)
#define LOG_W(tag, ...) DB_LOG(::drinkbot::LogLevel::Warn, tag, __VA_ARGS__)
#define LOG_I(tag, ...) DB_LOG(::drinkbot::LogLevel::Info, tag, __VA_ARGS__)
#define LOG_D(tag, ...) DB_LOG(::drinkbot::LogLevel::Debug, tag, __VA_ARGS__)
#define LOG_V(tag, ...) DB_LOG(::drinkbot::LogLevel::Verbose, tag, __VA_ARGS__)
