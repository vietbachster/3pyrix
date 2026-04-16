#include "Input.h"

#include <Arduino.h>
#include <InputManager.h>
#include <MappedInputManager.h>

// Global input managers (defined in main.cpp)
extern InputManager inputManager;
extern MappedInputManager& mappedInput;

namespace {

// Centralised mapping from papyrix::Button to MappedInputManager::Button.
// Having this in one place prevents the two copies in checkButton/isPressed from
// drifting apart and eliminates the uninitialized-variable UB that existed when
// the switch had no default case.
MappedInputManager::Button toMappedButton(papyrix::Button btn) {
  switch (btn) {
    case papyrix::Button::Up:
      return MappedInputManager::Button::Up;
    case papyrix::Button::Down:
      return MappedInputManager::Button::Down;
    case papyrix::Button::Left:
      return MappedInputManager::Button::Left;
    case papyrix::Button::Right:
      return MappedInputManager::Button::Right;
    case papyrix::Button::Center:
      return MappedInputManager::Button::Confirm;
    case papyrix::Button::Back:
      return MappedInputManager::Button::Back;
    case papyrix::Button::Power:
    default:
      return MappedInputManager::Button::Power;
  }
}

}  // namespace

namespace papyrix {
namespace drivers {

Result<void> Input::init(EventQueue& eventQueue) {
  if (initialized_) {
    return Ok();
  }

  queue_ = &eventQueue;
  lastActivityMs_ = millis();
  prevButtonState_ = 0;
  currButtonState_ = 0;
  initialized_ = true;

  return Ok();
}

void Input::shutdown() {
  queue_ = nullptr;
  initialized_ = false;
}

void Input::poll() {
  if (!initialized_ || !queue_) {
    return;
  }

  // Save previous state
  prevButtonState_ = currButtonState_;
  currButtonState_ = 0;

  // Check each button
  checkButton(Button::Up, 1 << 0);
  checkButton(Button::Down, 1 << 1);
  checkButton(Button::Left, 1 << 2);
  checkButton(Button::Right, 1 << 3);
  checkButton(Button::Center, 1 << 4);
  checkButton(Button::Back, 1 << 5);
  checkButton(Button::Power, 1 << 6);
}

void Input::checkButton(Button btn, uint8_t mask) {
  bool wasDown = (prevButtonState_ & mask) != 0;

  bool isDown = mappedInput.isPressed(toMappedButton(btn));

  if (isDown) {
    currButtonState_ |= mask;
  }

  int idx = static_cast<int>(btn);

  // Button just pressed
  if (isDown && !wasDown) {
    uint32_t now = millis();
    pressStartMs_[idx] = now;
    lastRepeatMs_[idx] = now;
    longPressFired_[idx] = false;
    queue_->push(Event::buttonPress(btn));
    lastActivityMs_ = now;
  }

  // Button held - check for long press and repeat
  if (isDown && wasDown) {
    uint32_t now = millis();
    uint32_t heldMs = now - pressStartMs_[idx];

    // Directional buttons use repeat instead of long press
    if (mask & REPEAT_BUTTON_MASK) {
      uint32_t sinceLastRepeat = now - lastRepeatMs_[idx];
      uint32_t threshold = (lastRepeatMs_[idx] == pressStartMs_[idx]) ? REPEAT_START_MS : REPEAT_INTERVAL_MS;
      if (sinceLastRepeat >= threshold) {
        queue_->push(Event::buttonRepeat(btn));
        lastRepeatMs_[idx] = now;
        lastActivityMs_ = now;
      }
    } else if (!longPressFired_[idx] && heldMs >= LONG_PRESS_MS) {
      queue_->push(Event::buttonLongPress(btn));
      longPressFired_[idx] = true;
    }
  }

  // Button released
  if (!isDown && wasDown) {
    queue_->push(Event::buttonRelease(btn));
    lastActivityMs_ = millis();
  }
}

uint32_t Input::idleTimeMs() const { return millis() - lastActivityMs_; }

void Input::resetIdleTimer() { lastActivityMs_ = millis(); }

bool Input::isPressed(Button btn) const { return mappedInput.isPressed(toMappedButton(btn)); }

void Input::resyncState() {
  currButtonState_ = 0;
  if (isPressed(Button::Up)) currButtonState_ |= (1 << 0);
  if (isPressed(Button::Down)) currButtonState_ |= (1 << 1);
  if (isPressed(Button::Left)) currButtonState_ |= (1 << 2);
  if (isPressed(Button::Right)) currButtonState_ |= (1 << 3);
  if (isPressed(Button::Center)) currButtonState_ |= (1 << 4);
  if (isPressed(Button::Back)) currButtonState_ |= (1 << 5);
  if (isPressed(Button::Power)) currButtonState_ |= (1 << 6);
  prevButtonState_ = currButtonState_;
}

MappedInputManager& Input::raw() { return mappedInput; }

}  // namespace drivers
}  // namespace papyrix
