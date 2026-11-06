#include "core/App.h"

#include <cstdio>

#include "core/Log.h"
#include "sim/SimPumpLink.h"

namespace drinkbot {

namespace {

CommandResult fromOp(const OpResult& op) {
  CommandResult r;
  r.ok = op.ok;
  r.code = op.code;
  r.message = op.message;
  return r;
}

CommandResult failure(const char* code, const std::string& message) {
  CommandResult r;
  r.ok = false;
  r.code = code;
  r.message = message;
  return r;
}

const char* failureCode(TxnStatus status) {
  return status == TxnStatus::Timeout || status == TxnStatus::LinkError ? ErrCode::kPumpTimeout : ErrCode::kPumpError;
}

}  // namespace

App::App(IPumpTransport& transport, const IClock& clock, SimPumpLink* simulator)
    : transport_(transport),
      clock_(clock),
      sim_(simulator),
      engine_(transport, clock),
      pumps_(engine_, clock, system_),
      dispenser_(engine_, pumps_, system_, clock),
      bulk_(engine_, pumps_, system_, clock) {
  system_.setChangeHandler([this] { stateDirty_ = true; });
  pumps_.setChangeHandler([this] { pumpsDirty_ = true; });
  dispenser_.setProgressHandler([this](const DispenseProgress& p) {
    for (auto* l : listeners_) {
      l->onDispenseProgress(p);
    }
  });
  bulk_.setProgressHandler([this](const BulkProgress& p) {
    for (auto* l : listeners_) {
      l->onBulkProgress(p);
    }
  });
}

void App::begin(const std::string& configJson, bool usedFallback) {
  JsonDocument doc;
  Config cfg;
  bool valid = false;
  bool migrated = false;
  if (deserializeJson(doc, configJson) == DeserializationError::Ok) {
    migrated = migrateConfig(doc);
    auto errors = parseConfig(doc.as<JsonVariantConst>(), cfg);
    valid = errors.empty();
    if (!valid) {
      LOG_E("CFG", "Stored config is invalid (%s: %s); using factory default", errors[0].path.c_str(),
            errors[0].message.c_str());
    }
  } else {
    LOG_E("CFG", "Stored config is not valid JSON; using factory default");
  }
  if (!valid) {
    makeFactoryDefaultConfig(doc);
    parseConfig(doc.as<JsonVariantConst>(), cfg);
    usedFallback = true;
    migrated = false;
  }

  std::string json;
  serializeJson(doc, json);
  if (migrated) {
    LOG_I("CFG", "Config migrated to schema %d", kConfigSchemaVersion);
    if (hooks_.saveConfig) {
      hooks_.saveConfig(json);
    }
  }
  if (usedFallback) {
    system_.raiseError(ErrorCode::Config, 0, "Stored config could not be used; running on a backup or default");
  }

  config_ = cfg;
  configJson_ = json;
  activeRecipeId_ = cfg.defaults.recipeId;
  activeVolumeMl_ = cfg.defaults.volumeMl;
  system_.setBooting(true);
  wasConnected_ = transport_.connected();
  started_ = true;
  pumps_.applyConfig(config_);
}

void App::post(Command command) {
  std::lock_guard<std::mutex> lock(queueMutex_);
  if (command.type == Command::Type::Estop) {
    queue_.push_front(std::move(command));
  } else {
    queue_.push_back(std::move(command));
  }
}

void App::tick() {
  std::deque<Command> pending;
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    pending.swap(queue_);
  }
  for (auto& cmd : pending) {
    handle(cmd);
  }

  transport_.tick();
  checkUsb();
  engine_.tick();

  if (system_.booting() && !pumps_.discovering()) {
    system_.setBooting(false);
    LOG_I("SYS", "Startup complete: %s", sysStateName(system_.state()));
  }
  pumps_.setPollingEnabled(!system_.booting() && !system_.busy() && !maintenanceBusy_);
  pumps_.tick();
  dispenser_.tick();
  bulk_.tick();

  uint32_t now = clock_.nowMs();
  while (!ledTest_.empty() && timeReached(now, ledTest_.front().atMs)) {
    std::string c = ledTest_.front().command;
    ledTest_.pop_front();
    if (c.empty()) {
      pumps_.restoreIdleLed(ledTestPump_);
      maintenanceBusy_ = false;
      system_.setActivity(Activity::Idle);
    } else {
      engine_.submit(ledTestPump_, c, nullptr);
    }
  }

  if (rebootPending_ && timeReached(now, rebootAt_)) {
    rebootPending_ = false;
    LOG_W("SYS", "Rebooting");
    if (hooks_.reboot) {
      hooks_.reboot();
    }
  }

  emitEvents();
}

void App::emitEvents() {
  uint32_t now = clock_.nowMs();
  if (stateDirty_ && now - lastStateEmit_ >= kStateMinIntervalMs) {
    stateDirty_ = false;
    lastStateEmit_ = now;
    for (auto* l : listeners_) {
      l->onStateChanged();
    }
  }
  if ((pumpsDirty_ && now - lastPumpsEmit_ >= kPumpsMinIntervalMs) || now - lastPumpsEmit_ >= kPumpsPeriodicMs) {
    pumpsDirty_ = false;
    lastPumpsEmit_ = now;
    for (auto* l : listeners_) {
      l->onPumpsChanged();
    }
  }
}

void App::checkUsb() {
  bool connected = transport_.connected();
  if (wasConnected_ && !connected) {
    LOG_E("USB", "Pump link lost");
    system_.raiseError(ErrorCode::UsbLost, 0, "The USB link to the bridge was lost");
    dispenser_.abort("USB link lost", false);
    bulk_.abort("USB link lost", false);
    ledTest_.clear();
    engine_.cancelAll(TxnStatus::LinkError);
  } else if (!wasConnected_ && connected && started_) {
    LOG_I("USB", "Pump link restored; rediscovering pumps");
    system_.clearError(ErrorCode::UsbLost, 0);
    pumps_.startDiscovery();
  }
  wasConnected_ = connected;
}

void App::reply(Command& cmd, const CommandResult& result) {
  if (cmd.reply) {
    cmd.reply(result);
  }
}

void App::replyOp(Command& cmd, const OpResult& result) {
  reply(cmd, fromOp(result));
}

bool App::guardIdle(Command& cmd) {
  if (system_.booting()) {
    reply(cmd, failure(ErrCode::kBusy, "Still starting up"));
    return false;
  }
  if (system_.busy() || maintenanceBusy_) {
    reply(cmd, failure(ErrCode::kBusy, std::string("Busy: ") + sysStateName(system_.state())));
    return false;
  }
  return true;
}

std::string App::recipeUnavailableReason(const Recipe& recipe) const {
  for (const auto& ing : recipe.ingredients) {
    const PumpConfig* pc = config_.findPumpByLiquid(ing.liquid);
    if (!pc) {
      return ing.liquid + " is not loaded";
    }
    const PumpState* ps = pumps_.find(pc->pumpId);
    if (!ps || !ps->present) {
      return ing.liquid + " pump " + std::to_string(pc->pumpId) + " is not responding";
    }
    if (!ps->fwOk) {
      return ing.liquid + " pump " + std::to_string(pc->pumpId) + " firmware is too old";
    }
    if (ps->hasWeight && ps->levelMl <= config_.levels.actualEmptyMl) {
      return ing.liquid + " is empty";
    }
  }
  return "";
}

void App::handle(Command& cmd) {
  using T = Command::Type;
  switch (cmd.type) {
    case T::SetRecipe: {
      if (!config_.findRecipe(cmd.text)) {
        reply(cmd, failure(ErrCode::kNotFound, "No recipe with id '" + cmd.text + "'"));
        return;
      }
      activeRecipeId_ = cmd.text;
      stateDirty_ = true;
      reply(cmd, {});
      return;
    }

    case T::SetVolume: {
      const DispenseSettings& d = config_.dispense;
      if (!(cmd.number >= d.minVolumeMl && cmd.number <= d.maxVolumeMl)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "Volume must be between %.0f and %.0f ml", d.minVolumeMl, d.maxVolumeMl);
        reply(cmd, failure(ErrCode::kInvalid, buf));
        return;
      }
      activeVolumeMl_ = cmd.number;
      stateDirty_ = true;
      reply(cmd, {});
      return;
    }

    case T::Dispense: {
      std::string recipeId = cmd.text.empty() ? activeRecipeId_ : cmd.text;
      float volume = std::isnan(cmd.number) ? activeVolumeMl_ : cmd.number;
      LOG_I("DISP", "Dispense requested (%s): %s, %.0f ml", cmd.source.c_str(), recipeId.c_str(), volume);
      if (!guardIdle(cmd)) {
        return;
      }
      if (recipeId.empty()) {
        reply(cmd, failure(ErrCode::kInvalid, "No recipe selected"));
        return;
      }
      if (system_.hasBlockingError()) {
        reply(cmd, failure(ErrCode::kPrecheckFailed, "A blocking error is active"));
        return;
      }
      if (system_.hasLatchedError()) {
        reply(cmd, failure(ErrCode::kPrecheckFailed, "Clear errors before dispensing"));
        return;
      }
      auto replyFn = cmd.reply;
      OpResult r = dispenser_.start(recipeId, volume, [replyFn](const OpResult& started) {
        if (replyFn) {
          replyFn(fromOp(started));
        }
      });
      if (!r.ok) {
        LOG_W("DISP", "Dispense not started: %s", r.message.c_str());
        replyOp(cmd, r);
      }
      return;
    }

    case T::PumpStop: {
      LOG_I("PUMP", "Stop pump %d requested (%s)", cmd.pumpId, cmd.source.c_str());
      if (bulk_.singleActive()) {
        replyOp(cmd, bulk_.stopSingle());
      } else if (dispenser_.active() || bulk_.active()) {
        reply(cmd, failure(ErrCode::kBusy, "Use E-Stop to stop a dispense or Prime/Purge All"));
      } else {
        // Nothing of ours is running; stop the pump anyway in case the UI knows better
        engine_.submit(cmd.pumpId, "stop", nullptr, true);
        reply(cmd, {});
      }
      return;
    }

    case T::Prime:
    case T::Purge:
    case T::PrimeAll:
    case T::PurgeAll: {
      bool single = cmd.type == T::Prime || cmd.type == T::Purge;
      // A manual prime/purge may replace another one (it is stopped first)
      if (!(single && bulk_.singleActive()) && !guardIdle(cmd)) {
        return;
      }
      if (system_.hasBlockingError()) {
        reply(cmd, failure(ErrCode::kPrecheckFailed, "A blocking error is active"));
        return;
      }
      BulkOp op = cmd.type == T::Prime      ? BulkOp::Prime
                  : cmd.type == T::Purge    ? BulkOp::Purge
                  : cmd.type == T::PrimeAll ? BulkOp::PrimeAll
                                            : BulkOp::PurgeAll;
      LOG_I("PUMP", "%s requested (%s)", bulkOpName(op), cmd.source.c_str());
      OpResult r = bulkOpIsAll(op) ? bulk_.startAll(op) : bulk_.startSingle(op, cmd.pumpId);
      replyOp(cmd, r);
      return;
    }

    case T::Estop:
      estop(cmd.source);
      reply(cmd, {});
      return;

    case T::ClearErrors: {
      int n = system_.clearLatched();
      CommandResult r;
      r.message = "Cleared " + std::to_string(n) + " error(s)";
      reply(cmd, r);
      return;
    }

    case T::Rescan:
      if (!guardIdle(cmd)) {
        return;
      }
      LOG_I("PUMP", "Rescan requested (%s)", cmd.source.c_str());
      pumps_.startDiscovery();
      reply(cmd, {});
      return;

    case T::Reboot:
      if (system_.busy()) {
        if (!cmd.flag) {
          reply(cmd, failure(ErrCode::kBusy, "A dispense or prime is running; reboot with force to stop it"));
          return;
        }
        estop(cmd.source);
      }
      LOG_W("SYS", "Reboot requested (%s)", cmd.source.c_str());
      reply(cmd, {});
      rebootPending_ = true;
      rebootAt_ = clock_.nowMs() + kRebootDelayMs;
      return;

    case T::SaveConfig: {
      if (system_.busy() || maintenanceBusy_) {
        reply(cmd, failure(ErrCode::kBusy, "Config can't be saved while a dispense or prime is running"));
        return;
      }
      JsonDocument doc;
      if (deserializeJson(doc, cmd.text) != DeserializationError::Ok) {
        reply(cmd, failure(ErrCode::kConfigInvalid, "Config is not valid JSON"));
        return;
      }
      migrateConfig(doc);
      Config cfg;
      auto errors = parseConfig(doc.as<JsonVariantConst>(), cfg);
      if (!errors.empty()) {
        LOG_W("CFG", "Config save rejected: %u problem(s), first %s: %s", static_cast<unsigned>(errors.size()),
              errors[0].path.c_str(), errors[0].message.c_str());
        CommandResult r = failure(ErrCode::kConfigInvalid, "Config has " + std::to_string(errors.size()) + " problem(s)");
        r.configErrors = std::move(errors);
        reply(cmd, r);
        return;
      }
      std::string json;
      serializeJson(doc, json);
      if (hooks_.saveConfig && !hooks_.saveConfig(json)) {
        reply(cmd, failure(ErrCode::kInternal, "Config could not be written to flash"));
        return;
      }
      LOG_I("CFG", "Config saved (%s)", cmd.source.c_str());
      system_.clearError(ErrorCode::Config, 0);
      applyConfig(json, cfg);
      reply(cmd, {});
      return;
    }

    case T::PumpTare:
      if (guardIdle(cmd)) {
        runMaintenance(cmd, {"tare"});
      }
      return;

    case T::PumpCalibrate: {
      if (!guardIdle(cmd)) {
        return;
      }
      if (!(cmd.number > 0)) {
        reply(cmd, failure(ErrCode::kInvalid, "Calibration weight must be greater than 0 g"));
        return;
      }
      char buf[32];
      snprintf(buf, sizeof(buf), "cal %.2f", cmd.number);
      runMaintenance(cmd, {buf});
      return;
    }

    case T::PumpSetId:
      if (guardIdle(cmd)) {
        handleSetId(cmd);
      }
      return;

    case T::PumpLedTest:
      if (guardIdle(cmd)) {
        handleLedTest(cmd);
      }
      return;

    case T::PumpRaw: {
      // Only one pump may run at a time (power budget, D-7). A raw motion
      // command would run a pump the controller doesn't track, so refuse it.
      std::string first = cmd.text.substr(0, cmd.text.find(' '));
      if (first == "dispense" || first == "prime" || first == "purge" || first == "bridge") {
        reply(cmd, failure(ErrCode::kInvalid, "Use Prime / Purge or a recipe to run a pump"));
        return;
      }
      if (guardIdle(cmd)) {
        runMaintenance(cmd, {cmd.text});
      }
      return;
    }

    case T::SimFault:
      handleSimFault(cmd);
      return;
  }
}

void App::applyConfig(const std::string& json, const Config& config) {
  config_ = config;
  configJson_ = json;
  if (!config_.findRecipe(activeRecipeId_)) {
    activeRecipeId_ = config_.defaults.recipeId;
  }
  if (activeVolumeMl_ < config_.dispense.minVolumeMl || activeVolumeMl_ > config_.dispense.maxVolumeMl) {
    activeVolumeMl_ = config_.defaults.volumeMl;
  }
  pumps_.applyConfig(config_);
  stateDirty_ = true;
  for (auto* l : listeners_) {
    l->onConfigChanged();
  }
}

void App::runMaintenance(Command& cmd, const std::vector<std::string>& commands) {
  bool known = pumps_.find(cmd.pumpId) != nullptr || cmd.pumpId == pumps_.bridgeId();
  if (!known) {
    reply(cmd, failure(ErrCode::kNotFound, "No pump with ID " + std::to_string(cmd.pumpId)));
    return;
  }
  LOG_I("PUMP", "Pump %d maintenance (%s): %s", cmd.pumpId, cmd.source.c_str(), commands.front().c_str());
  maintenanceBusy_ = true;
  system_.setActivity(Activity::Manual);

  struct Chain {
    std::vector<std::string> commands;
    size_t index = 0;
    CommandResult result;
  };
  auto chain = std::make_shared<Chain>();
  chain->commands = commands;
  auto replyFn = cmd.reply;
  int pumpId = cmd.pumpId;
  auto step = std::make_shared<std::function<void()>>();
  *step = [this, chain, replyFn, pumpId, step]() {
    engine_.submit(pumpId, chain->commands[chain->index], [this, chain, replyFn, step](const TxnResult& r) {
      for (const auto& line : r.data) {
        chain->result.lines.push_back(line);
      }
      chain->result.lines.push_back(std::string("R,") + txnStatusName(r.status));
      bool last = ++chain->index >= chain->commands.size();
      if (!r.ok()) {
        chain->result.ok = false;
        chain->result.code = failureCode(r.status);
        chain->result.message = std::string("Pump answered ") + txnStatusName(r.status);
      }
      if (!r.ok() || last) {
        maintenanceBusy_ = false;
        system_.setActivity(Activity::Idle);
        if (replyFn) {
          replyFn(chain->result);
        }
        *step = nullptr;  // break the self-reference
        return;
      }
      (*step)();
    });
  };
  (*step)();
}

void App::handleSetId(Command& cmd) {
  int oldId = cmd.pumpId;
  int newId = cmd.newId;
  if (oldId == kBridgeBoardId) {
    reply(cmd, failure(ErrCode::kInvalid, "The bridge board is always ID 0"));
    return;
  }
  if (newId < 1 || newId > 254) {
    reply(cmd, failure(ErrCode::kInvalid, "Pump ID must be 1-254"));
    return;
  }
  if (newId != oldId && config_.findPumpById(newId)) {
    reply(cmd, failure(ErrCode::kInvalid, "Pump ID " + std::to_string(newId) + " is already in use"));
    return;
  }
  bool isBridge = oldId == pumps_.bridgeId();
  // `scfg` must reach the pump at its new address; the bridge is always direct
  int saveTarget = isBridge ? TransactionEngine::kDirect : newId;
  auto replyFn = cmd.reply;
  LOG_I("PUMP", "Changing pump ID %d to %d (%s)", oldId, newId, cmd.source.c_str());
  maintenanceBusy_ = true;
  system_.setActivity(Activity::Manual);

  auto finish = [this, replyFn](const CommandResult& r) {
    maintenanceBusy_ = false;
    system_.setActivity(Activity::Idle);
    if (replyFn) {
      replyFn(r);
    }
  };

  engine_.submit(isBridge ? TransactionEngine::kDirect : oldId, "sid " + std::to_string(newId),
                 [this, finish, oldId, newId, saveTarget](const TxnResult& r) {
                   if (!r.ok()) {
                     finish(failure(failureCode(r.status), "Pump did not accept the new ID"));
                     return;
                   }
                   engine_.submit(saveTarget, "scfg", [this, finish, oldId, newId](const TxnResult& s) {
                     if (!s.ok()) {
                       finish(failure(failureCode(s.status), "New ID set but not saved on the pump (scfg failed)"));
                       return;
                     }
                     // Keep the config in step with the pump
                     JsonDocument doc;
                     deserializeJson(doc, configJson_);
                     bool updated = false;
                     for (JsonObject p : doc["pumps"].as<JsonArray>()) {
                       if (p["pumpId"].as<int>() == oldId) {
                         p["pumpId"] = newId;
                         updated = true;
                       }
                     }
                     CommandResult ok;
                     if (updated) {
                       std::string json;
                       serializeJson(doc, json);
                       Config cfg;
                       if (parseConfig(doc.as<JsonVariantConst>(), cfg).empty() &&
                           (!hooks_.saveConfig || hooks_.saveConfig(json))) {
                         maintenanceBusy_ = false;
                         applyConfig(json, cfg);  // rediscovers
                       } else {
                         ok = failure(ErrCode::kInternal, "Pump ID changed but the config could not be updated");
                       }
                     } else {
                       pumps_.startDiscovery();
                     }
                     finish(ok);
                   });
                 });
}

void App::handleLedTest(Command& cmd) {
  const PumpState* p = pumps_.find(cmd.pumpId);
  if (!p || !p->usable()) {
    reply(cmd, failure(ErrCode::kPrecheckFailed, "Pump " + std::to_string(cmd.pumpId) + " is not available"));
    return;
  }
  LOG_I("LED", "LED test on pump %d (%s)", cmd.pumpId, cmd.source.c_str());
  maintenanceBusy_ = true;
  system_.setActivity(Activity::Manual);
  ledTestPump_ = cmd.pumpId;
  uint32_t now = clock_.nowMs();
  ledTest_ = {{now, "ledtime 0"},
              {now, "ledcolor 255 0 0"},
              {now, "ledmode 1 255"},
              {now + 1000, "ledcolor 0 255 0"},
              {now + 2000, "ledcolor 0 0 255"},
              {now + 3000, ""}};  // "" = restore the idle state
  reply(cmd, {});
}

void App::handleSimFault(Command& cmd) {
  if (!sim_) {
    reply(cmd, failure(ErrCode::kInvalid, "Fault injection is only available in simulator mode"));
    return;
  }
  if (cmd.text == "missing") {
    sim_->setMissing(cmd.pumpId, true);
  } else if (cmd.text == "present") {
    sim_->setMissing(cmd.pumpId, false);
  } else if (cmd.text == "timeout") {
    sim_->ignoreNextCommand(cmd.pumpId);
  } else if (cmd.text == "overrun") {
    sim_->setStuck(cmd.pumpId, true);
  } else if (cmd.text == "weight" && !std::isnan(cmd.number)) {
    sim_->setWeight(cmd.pumpId, cmd.number);
  } else {
    reply(cmd, failure(ErrCode::kInvalid, "Unknown fault '" + cmd.text + "'"));
    return;
  }
  LOG_I("SIM", "Fault injected on pump %d: %s", cmd.pumpId, cmd.text.c_str());
  reply(cmd, {});
}

void App::estop(const std::string& source) {
  LOG_W("SYS", "E-STOP activated (source=%s)", source.c_str());
  // Abort first so the sequences ignore the cancellations below
  dispenser_.abort("E-Stop", false);
  bulk_.abort("E-Stop", false);
  if (!ledTest_.empty()) {
    ledTest_.clear();
    maintenanceBusy_ = false;
    system_.setActivity(Activity::Idle);
    pumps_.restoreIdleLed(ledTestPump_);
  }
  engine_.cancelQueuedMotion(TxnStatus::LinkError);

  // One broadcast reaches every pump; the addressed stops confirm each one
  engine_.submit(kBroadcastId, "stop", nullptr, true);
  int bridgeId = pumps_.bridgeId();
  engine_.submit(
      TransactionEngine::kDirect, "stop",
      [this, bridgeId](const TxnResult& r) {
        if (!r.ok()) {
          system_.raiseError(ErrorCode::StopUnconfirmed, bridgeId > 0 ? bridgeId : 0,
                             "The bridge did not confirm the E-Stop");
        }
      },
      true);
  for (const auto& p : pumps_.pumps()) {
    if (!p.present || p.pumpId == bridgeId) {
      continue;
    }
    int id = p.pumpId;
    engine_.submit(
        id, "stop",
        [this, id](const TxnResult& r) {
          if (!r.ok()) {
            system_.raiseError(ErrorCode::StopUnconfirmed, id,
                               "Pump " + std::to_string(id) + " did not confirm the E-Stop");
          }
        },
        true);
  }
}

}  // namespace drinkbot
