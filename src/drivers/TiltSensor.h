#pragma once

#include <cstdint>

#include "../core/EventQueue.h"
#include "../core/Result.h"

namespace papyrix {
namespace drivers {

class TiltSensor {
 public:
  Result<void> init(EventQueue& eventQueue);
  void shutdown();

  bool isAvailable() const { return available_; }

  // Transition the IMU into active gyroscope mode when motion page-turn is enabled.
  void wake();

  // Put the IMU into power-down mode to minimize current draw.
  void sleep();

  // Poll the gyroscope and emit flick gestures when enabled in reader mode.
  void poll(uint8_t orientation, bool enabled);

  // True if tilt activity occurred since the last call.
  bool hadActivity();

  // Drop any pending gesture flags while preserving the current in-tilt state.
  void clearPendingEvents();

 private:
  bool writeReg(uint8_t reg, uint8_t val) const;
  bool readReg(uint8_t reg, uint8_t* val) const;
  bool readGyro(float& gx, float& gy, float& gz) const;
  void pushEvent(const Event& event);

  EventQueue* queue_ = nullptr;
  bool initialized_ = false;
  bool available_ = false;
  uint8_t i2cAddr_ = 0;

  bool tiltForwardEvent_ = false;
  bool tiltBackEvent_ = false;
  bool hadActivity_ = false;
  bool inTilt_ = false;
  bool isAwake_ = false;
  unsigned long lastTiltMs_ = 0;
  unsigned long lastPollMs_ = 0;
  unsigned long wakeMs_ = 0;
  float biasX_ = 0.0f;
  float biasY_ = 0.0f;
  float biasZ_ = 0.0f;
  uint8_t calibSamples_ = 0;
  float calibAccX_ = 0.0f;
  float calibAccY_ = 0.0f;
  float calibAccZ_ = 0.0f;
  bool calibrated_ = false;

  static constexpr float RATE_THRESHOLD_DPS = 270.0f;
  static constexpr float NEUTRAL_RATE_DPS = 50.0f;
  static constexpr unsigned long COOLDOWN_MS = 600;
  static constexpr unsigned long POLL_INTERVAL_MS = 50;
  static constexpr unsigned long WAKE_STABILIZE_MS = 200;
  static constexpr uint8_t CALIB_SAMPLE_COUNT = 4;

  static constexpr uint8_t I2C_ADDR_PRIMARY = 0x6A;
  static constexpr uint8_t I2C_ADDR_ALT = 0x6B;
  static constexpr uint8_t REG_WHO_AM_I = 0x00;
  static constexpr uint8_t REG_CTRL1 = 0x02;
  static constexpr uint8_t REG_CTRL3 = 0x04;
  static constexpr uint8_t REG_CTRL7 = 0x08;
  static constexpr uint8_t REG_GX_L = 0x3B;
  static constexpr uint8_t WHO_AM_I_VALUE = 0x05;
};

}  // namespace drivers
}  // namespace papyrix
