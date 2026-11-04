#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>

#include "core/Clock.h"
#include "core/PumpProtocol.h"
#include "core/PumpTransport.h"

namespace drinkbot {

// Runs pump requests one at a time over the transport, with the timeouts
// and retry policy of SPEC_v3 §5.3-§5.5.
class TransactionEngine {
 public:
  using Callback = std::function<void(const TxnResult&)>;

  // Address used before the bridge pump's own ID is known (discovery).
  static constexpr int kDirect = 0;

  static constexpr uint32_t kDirectTimeoutMs = 500;
  static constexpr uint32_t kBridgeTimeoutMs = 2500;
  // After a timeout or bad CRC, late lines of that exchange are discarded.
  static constexpr uint32_t kQuietAfterFailureMs = 100;

  TransactionEngine(IPumpTransport& transport, const IClock& clock);

  // The bridge pump's ID (from `gid`); requests to it are sent directly.
  void setBridgeId(int id) { bridgeId_ = id; }
  int bridgeId() const { return bridgeId_; }

  // Queues a request. `urgent` requests (E-Stop) jump the queue.
  // The callback always runs exactly once, from tick() or cancel().
  void submit(int pumpId, const std::string& command, Callback done, bool urgent = false);

  // Runs one engine step: reads lines, completes or times out the current
  // request and starts the next one.
  void tick();

  bool idle() const { return !current_ && queue_.empty(); }
  size_t queued() const { return queue_.size(); }

  // Fails every queued request (not the in-flight one) with `status`.
  void cancelQueued(TxnStatus status);
  // E-Stop: fails only queued dispense/prime/purge commands, and stops any
  // earlier motion command from being retried. Polls and LED commands stay.
  void cancelQueuedMotion(TxnStatus status);
  // Fails the in-flight request and everything queued (e.g. USB lost).
  void cancelAll(TxnStatus status);

 private:
  enum class RetryClass : uint8_t { ReadOnly, Led, Motion, Stop, None };

  struct Request {
    int pumpId = 0;
    std::string command;
    Callback done;
    bool urgent = false;
    RetryClass retryClass = RetryClass::None;
    int retriesLeft = 0;
    bool motionRetried = false;
    // Cancel epoch at submit time. A request from an older epoch is never
    // retried, so a motion command can't be resent after an E-Stop.
    uint32_t epoch = 0;
  };

  static RetryClass classify(const std::string& command);
  static int retriesFor(RetryClass cls);

  bool isDirect(int pumpId) const;
  void start(std::shared_ptr<Request> req);
  void complete(const TxnResult& result);
  void finish(const std::shared_ptr<Request>& req, const TxnResult& result);

  IPumpTransport& transport_;
  const IClock& clock_;
  int bridgeId_ = -1;

  std::deque<std::shared_ptr<Request>> queue_;
  std::shared_ptr<Request> current_;
  TransactionParser parser_;
  uint32_t deadline_ = 0;
  uint32_t quietUntil_ = 0;
  bool quiet_ = false;
  uint32_t epoch_ = 0;
};

}  // namespace drinkbot
