#include "core/TransactionEngine.h"

#include "core/Log.h"

namespace drinkbot {

namespace {

std::string firstToken(const std::string& command) {
  return command.substr(0, command.find(' '));
}

bool isTransientFailure(TxnStatus status) {
  return status == TxnStatus::Timeout || status == TxnStatus::BadCrc;
}

}  // namespace

TransactionEngine::TransactionEngine(IPumpTransport& transport, const IClock& clock)
    : transport_(transport), clock_(clock) {}

TransactionEngine::RetryClass TransactionEngine::classify(const std::string& command) {
  std::string cmd = firstToken(command);
  if (cmd == "version" || cmd == "gid" || cmd == "weight" || cmd == "status") {
    return RetryClass::ReadOnly;
  }
  if (cmd.rfind("led", 0) == 0) {
    return RetryClass::Led;
  }
  if (cmd == "dispense" || cmd == "prime" || cmd == "purge") {
    return RetryClass::Motion;
  }
  if (cmd == "stop") {
    return RetryClass::Stop;
  }
  // tare, cal, sid, scfg, firmware update and raw maintenance commands: report, don't retry
  return RetryClass::None;
}

int TransactionEngine::retriesFor(RetryClass cls) {
  switch (cls) {
    case RetryClass::ReadOnly: return 2;
    case RetryClass::Led: return 1;
    case RetryClass::Stop: return 3;
    case RetryClass::Motion:  // handled by the status check instead
    case RetryClass::None: return 0;
  }
  return 0;
}

bool TransactionEngine::isDirect(int pumpId) const {
  return pumpId == kDirect || pumpId == bridgeId_;
}

void TransactionEngine::submit(int pumpId, const std::string& command, Callback done, bool urgent) {
  auto req = std::make_shared<Request>();
  req->pumpId = pumpId;
  req->command = command;
  req->done = std::move(done);
  req->urgent = urgent;
  req->retryClass = classify(command);
  req->retriesLeft = retriesFor(req->retryClass);
  req->epoch = epoch_;
  if (urgent) {
    // Behind any urgent request already waiting, ahead of everything else
    auto it = queue_.begin();
    while (it != queue_.end() && (*it)->urgent) {
      ++it;
    }
    queue_.insert(it, std::move(req));
  } else {
    queue_.push_back(std::move(req));
  }
}

void TransactionEngine::tick() {
  uint32_t now = clock_.nowMs();
  if (quiet_ && timeReached(now, quietUntil_)) {
    quiet_ = false;
  }

  std::string line;
  while (transport_.readLine(line)) {
    LOG_V("USB", "< %s", line.c_str());
    if (quiet_) {
      continue;
    }
    if (current_ && parser_.feed(line)) {
      complete(parser_.result());
    }
  }

  if (current_ && timeReached(clock_.nowMs(), deadline_)) {
    TxnResult timeout;
    timeout.status = TxnStatus::Timeout;
    complete(timeout);
  }

  if (!current_ && !quiet_ && !queue_.empty()) {
    auto next = queue_.front();
    queue_.pop_front();
    start(std::move(next));
  }
}

void TransactionEngine::start(std::shared_ptr<Request> req) {
  bool direct = isDirect(req->pumpId);
  std::string wire;
  if (!buildWireCommand(direct, req->pumpId, req->command, wire)) {
    LOG_E("PUMP", "Rejected malformed command for pump %d: %s", req->pumpId, req->command.c_str());
    TxnResult r;
    r.status = TxnStatus::LinkError;
    finish(req, r);
    return;
  }
  if (!transport_.connected() || !transport_.writeLine(wire)) {
    TxnResult r;
    r.status = TxnStatus::LinkError;
    finish(req, r);
    return;
  }
  LOG_V("USB", "> %s", wire.c_str());

  // A broadcast is answered by the bridge itself, like a direct command
  bool bridgedReply = !direct && req->pumpId != kBroadcastId;
  parser_.begin(bridgedReply);
  std::string cmd = firstToken(req->command);
  // Flash work blocks the board: averaging the scale, or an image write
  bool slow = cmd == "tare" || cmd == "cal" || cmd.rfind("fw", 0) == 0;
  uint32_t timeoutMs = bridgedReply ? kBridgeTimeoutMs : (slow ? kSlowDirectTimeoutMs : kDirectTimeoutMs);
  deadline_ = clock_.nowMs() + timeoutMs;
  current_ = std::move(req);
}

void TransactionEngine::complete(const TxnResult& result) {
  std::shared_ptr<Request> req = std::move(current_);
  current_.reset();

  if (isTransientFailure(result.status)) {
    quiet_ = true;
    quietUntil_ = clock_.nowMs() + kQuietAfterFailureMs;
  }

  if (!result.ok() && isTransientFailure(result.status) && req->epoch == epoch_) {
    if (req->retryClass == RetryClass::Motion && !req->motionRetried) {
      // The command may have run with its reply lost. Ask the pump before
      // retrying, so a lost reply never turns into a double pour.
      LOG_W("PUMP", "Pump %d: no reply to '%s', checking status", req->pumpId, req->command.c_str());
      TxnResult original = result;
      submit(
          req->pumpId, "status",
          [this, req, original](const TxnResult& st) {
            PumpMotion motion;
            uint32_t remaining = 0;
            if (st.ok() && parseStatus(st, motion, remaining)) {
              if (motion != PumpMotion::Idle) {
                TxnResult ok;
                ok.status = TxnStatus::Ok;
                finish(req, ok);
              } else if (req->epoch != epoch_) {
                finish(req, original);  // cancelled (E-Stop) while checking: never resend
              } else {
                req->motionRetried = true;
                queue_.push_front(req);
              }
            } else {
              finish(req, original);
            }
          },
          true);
      return;
    }
    if (req->retriesLeft > 0) {
      req->retriesLeft--;
      LOG_D("PUMP", "Pump %d: retrying '%s' (%s)", req->pumpId, req->command.c_str(),
            txnStatusName(result.status));
      queue_.push_front(req);
      return;
    }
  }
  finish(req, result);
}

void TransactionEngine::finish(const std::shared_ptr<Request>& req, const TxnResult& result) {
  if (!result.ok()) {
    LOG_W("PUMP", "Pump %d: '%s' failed (%s)", req->pumpId, req->command.c_str(),
          txnStatusName(result.status));
  }
  if (req->done) {
    req->done(result);
  }
}

void TransactionEngine::cancelQueued(TxnStatus status) {
  epoch_++;
  std::deque<std::shared_ptr<Request>> pending;
  pending.swap(queue_);
  TxnResult r;
  r.status = status;
  for (auto& req : pending) {
    if (req->done) {
      req->done(r);
    }
  }
}

void TransactionEngine::cancelQueuedMotion(TxnStatus status) {
  epoch_++;
  std::vector<std::shared_ptr<Request>> cancelled;
  for (auto it = queue_.begin(); it != queue_.end();) {
    if ((*it)->retryClass == RetryClass::Motion) {
      cancelled.push_back(*it);
      it = queue_.erase(it);
    } else {
      ++it;
    }
  }
  TxnResult r;
  r.status = status;
  for (auto& req : cancelled) {
    if (req->done) {
      req->done(r);
    }
  }
}

void TransactionEngine::cancelAll(TxnStatus status) {
  epoch_++;
  if (current_) {
    std::shared_ptr<Request> req = std::move(current_);
    current_.reset();
    TxnResult r;
    r.status = status;
    if (req->done) {
      req->done(r);
    }
  }
  cancelQueued(status);
}

}  // namespace drinkbot
