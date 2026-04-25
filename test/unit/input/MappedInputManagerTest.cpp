#include "test_utils.h"

#include <cstdint>

#include "MappedInputManager.h"
#include "core/PapyrixSettings.h"

namespace {

bool expectPressedMapping(TestUtils::TestRunner& runner, MappedInputManager& mapped, InputManager& input,
                          MappedInputManager::Button logicalButton, uint8_t physicalButton,
                          const std::string& testName) {
  input.reset();
  input.setWasPressed(physicalButton, true);
  return runner.expectTrue(mapped.wasPressed(logicalButton), testName);
}

}  // namespace

int main() {
  TestUtils::TestRunner runner("MappedInputManager");

  InputManager input;
  MappedInputManager mapped(input);

  {
    expectPressedMapping(runner, mapped, input, MappedInputManager::Button::Back, InputManager::BTN_BACK,
                         "default_back_maps_to_back");
    expectPressedMapping(runner, mapped, input, MappedInputManager::Button::Confirm, InputManager::BTN_CONFIRM,
                         "default_confirm_maps_to_confirm");
    expectPressedMapping(runner, mapped, input, MappedInputManager::Button::PageBack, InputManager::BTN_UP,
                         "default_page_back_maps_to_up");
    expectPressedMapping(runner, mapped, input, MappedInputManager::Button::PageForward, InputManager::BTN_DOWN,
                         "default_page_forward_maps_to_down");
  }

  {
    papyrix::Settings settings;
    settings.frontButtonLayout = papyrix::Settings::FrontLRBC;
    mapped.setSettings(&settings);

    expectPressedMapping(runner, mapped, input, MappedInputManager::Button::Back, InputManager::BTN_LEFT,
                         "lrbc_back_maps_to_left");
    expectPressedMapping(runner, mapped, input, MappedInputManager::Button::Left, InputManager::BTN_BACK,
                         "lrbc_left_maps_to_back");
    expectPressedMapping(runner, mapped, input, MappedInputManager::Button::Right, InputManager::BTN_CONFIRM,
                         "lrbc_right_maps_to_confirm");
  }

  {
    papyrix::Settings settings;
    settings.sideButtonLayout = papyrix::Settings::NextPrev;
    mapped.setSettings(&settings);

    expectPressedMapping(runner, mapped, input, MappedInputManager::Button::Up, InputManager::BTN_DOWN,
                         "nextprev_up_maps_to_down");
    expectPressedMapping(runner, mapped, input, MappedInputManager::Button::Down, InputManager::BTN_UP,
                         "nextprev_down_maps_to_up");
    expectPressedMapping(runner, mapped, input, MappedInputManager::Button::PageBack, InputManager::BTN_DOWN,
                         "nextprev_page_back_maps_to_down");
    expectPressedMapping(runner, mapped, input, MappedInputManager::Button::PageForward, InputManager::BTN_UP,
                         "nextprev_page_forward_maps_to_up");
  }

  {
    input.reset();
    input.setWasReleased(InputManager::BTN_POWER, true);
    runner.expectTrue(mapped.wasReleased(MappedInputManager::Button::Power), "power_release_passthrough");
  }

  {
    input.reset();
    input.setIsPressed(InputManager::BTN_CONFIRM, true);
    papyrix::Settings settings;
    mapped.setSettings(&settings);
    runner.expectTrue(mapped.isPressed(MappedInputManager::Button::Confirm), "confirm_is_pressed_passthrough");
  }

  {
    input.reset();
    input.setWasAnyPressed(true);
    input.setWasAnyReleased(true);
    input.setHeldTime(275);
    runner.expectTrue(mapped.wasAnyPressed(), "was_any_pressed_passthrough");
    runner.expectTrue(mapped.wasAnyReleased(), "was_any_released_passthrough");
    runner.expectEq(275ul, mapped.getHeldTime(), "held_time_passthrough");
  }

  return runner.allPassed() ? 0 : 1;
}
