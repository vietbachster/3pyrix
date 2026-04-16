#pragma once

#include <cstddef>
#include <cstdint>

#include "../core/Result.h"

class EInkDisplay;

namespace papyrix {
namespace drivers {

class Display {
 public:
  enum class RefreshMode : uint8_t {
    Full,
    Half,
    Fast,
  };

  Result<void> init();
  void shutdown();

  // Buffer access
  uint8_t* getBuffer();
  const uint8_t* getBuffer() const;
  static constexpr size_t bufferSize();

  // Dimensions
  static constexpr uint16_t width();
  static constexpr uint16_t height();

  // Rendering control
  void markDirty() { dirty_ = true; }
  bool isDirty() const { return dirty_; }
  void flush(RefreshMode mode = RefreshMode::Fast);
  void clear(uint8_t color = 0xFF);

  // Power management
  void sleep();
  void wake();

  // Access underlying display (for legacy code during migration)
  EInkDisplay& raw();

 private:
  bool dirty_ = false;
  bool initialized_ = false;
};

// Inline constexpr methods
constexpr size_t Display::bufferSize() {
  return 52272;  // 792 * 528 / 8 = 52272 bytes
}

constexpr uint16_t Display::width() { return 792; }

constexpr uint16_t Display::height() { return 528; }

}  // namespace drivers
}  // namespace papyrix
