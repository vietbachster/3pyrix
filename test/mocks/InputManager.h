#pragma once

#include <array>
#include <cstdint>

class InputManager {
 public:
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;

  void reset() {
    wasPressed_.fill(false);
    wasReleased_.fill(false);
    isPressed_.fill(false);
    anyPressed_ = false;
    anyReleased_ = false;
    heldTime_ = 0;
  }

  void setWasPressed(uint8_t button, bool value) { wasPressed_[button] = value; }
  void setWasReleased(uint8_t button, bool value) { wasReleased_[button] = value; }
  void setIsPressed(uint8_t button, bool value) { isPressed_[button] = value; }
  void setWasAnyPressed(bool value) { anyPressed_ = value; }
  void setWasAnyReleased(bool value) { anyReleased_ = value; }
  void setHeldTime(unsigned long value) { heldTime_ = value; }

  bool wasPressed(uint8_t button) const { return wasPressed_[button]; }
  bool wasReleased(uint8_t button) const { return wasReleased_[button]; }
  bool isPressed(uint8_t button) const { return isPressed_[button]; }
  bool wasAnyPressed() const { return anyPressed_; }
  bool wasAnyReleased() const { return anyReleased_; }
  unsigned long getHeldTime() const { return heldTime_; }

 private:
  std::array<bool, 7> wasPressed_{};
  std::array<bool, 7> wasReleased_{};
  std::array<bool, 7> isPressed_{};
  bool anyPressed_ = false;
  bool anyReleased_ = false;
  unsigned long heldTime_ = 0;
};
