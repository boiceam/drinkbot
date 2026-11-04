#include "core/UiController.h"

#include "core/Log.h"

namespace drinkbot {

UiInputs UiController::inputs() const {
  UiInputs in;
  in.state = app_.system().state();
  in.hasErrors = app_.system().hasErrors();
  in.wifi = wifi_;
  in.activeRecipeId = app_.activeRecipeId();
  in.activeVolumeMl = app_.activeVolumeMl();
  in.buttons = app_.config().buttons;
  return in;
}

void UiController::handle(const std::vector<ButtonEvent>& events) {
  for (const auto& e : events) {
    if (e.kind == ButtonEvent::Kind::Press) {
      press(e.index);
    } else {
      longPress(e.index);
    }
  }
}

void UiController::press(size_t index) {
  const ButtonConfig& b = app_.config().buttons[index];
  LOG_I("BTN", "Button %u pressed (%s)", static_cast<unsigned>(index + 1), buttonActionName(b.action));
  model_.flashPress(index);

  Command cmd;
  cmd.source = "button " + std::to_string(index + 1);
  switch (b.action) {
    case ButtonAction::None:
      return;
    case ButtonAction::SetRecipe:
      cmd.type = Command::Type::SetRecipe;
      cmd.text = b.recipeId;
      break;
    case ButtonAction::SetVolume:
      cmd.type = Command::Type::SetVolume;
      cmd.number = b.volumeMl;
      break;
    case ButtonAction::Dispense:
      cmd.type = Command::Type::Dispense;
      cmd.reply = [this, index](const CommandResult& r) {
        if (!r.ok) {
          LOG_W("BTN", "Button %u dispense refused: %s", static_cast<unsigned>(index + 1), r.message.c_str());
          model_.flashFailure(index);
        }
      };
      break;
    case ButtonAction::Estop:
      cmd.type = Command::Type::Estop;
      break;
  }
  app_.post(std::move(cmd));
}

void UiController::longPress(size_t index) {
  // Holding an E-Stop button for 3 s also clears latched errors (D-4)
  if (app_.config().buttons[index].action != ButtonAction::Estop) {
    return;
  }
  Command cmd;
  cmd.type = Command::Type::ClearErrors;
  cmd.source = "button " + std::to_string(index + 1) + " long press";
  cmd.reply = [this, index](const CommandResult&) {
    LOG_I("BTN", "Latched errors cleared by long press");
    model_.flashConfirm(index);
  };
  app_.post(std::move(cmd));
}

}  // namespace drinkbot
