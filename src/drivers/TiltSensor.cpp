#include "TiltSensor.h"

#include <Arduino.h>
#include <Logging.h>
#include <Wire.h>
#include <math.h>

#define TAG "TILT"

namespace {
constexpr uint8_t kOrientationPortrait = 0;
constexpr uint8_t kOrientationLandscapeCW = 1;
constexpr uint8_t kOrientationInverted = 2;
constexpr uint8_t kOrientationLandscapeCCW = 3;
}  // namespace

namespace papyrix {
namespace drivers {

bool TiltSensor::writeReg(uint8_t reg, uint8_t val) const {
  Wire.beginTransmission(i2cAddr_);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool TiltSensor::readReg(uint8_t reg, uint8_t* val) const {
  Wire.beginTransmission(i2cAddr_);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  Wire.requestFrom(i2cAddr_, static_cast<uint8_t>(1));
  if (Wire.available() < 1) {
    return false;
  }

  *val = Wire.read();
  return true;
}

bool TiltSensor::readGyro(float& gx, float& gy, float& gz) const {
  Wire.beginTransmission(i2cAddr_);
  Wire.write(REG_GX_L);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  Wire.requestFrom(i2cAddr_, static_cast<uint8_t>(6));
  if (Wire.available() < 6) {
    return false;
  }

  auto readInt16 = []() -> int16_t {
    const uint8_t lo = Wire.read();
    const uint8_t hi = Wire.read();
    return static_cast<int16_t>((static_cast<uint16_t>(hi) << 8) | lo);
  };

  constexpr float kScale = 1.0f / 64.0f;
  gx = readInt16() * kScale;
  gy = readInt16() * kScale;
  gz = readInt16() * kScale;
  return true;
}

void TiltSensor::pushEvent(const Event& event) {
  if (!queue_) {
    return;
  }

  if (event.type == EventType::TiltForward) {
    tiltForwardEvent_ = true;
  } else if (event.type == EventType::TiltBack) {
    tiltBackEvent_ = true;
  }

  queue_->push(event);
  hadActivity_ = true;
}

Result<void> TiltSensor::init(EventQueue& eventQueue) {
  if (initialized_) {
    return Ok();
  }

  queue_ = &eventQueue;
  initialized_ = true;
  available_ = false;
  clearPendingEvents();
  inTilt_ = false;
  isAwake_ = false;
  lastTiltMs_ = 0;
  lastPollMs_ = millis();
  wakeMs_ = 0;
  biasX_ = 0.0f;
  biasY_ = 0.0f;
  biasZ_ = 0.0f;
  calibSamples_ = 0;
  calibAccX_ = 0.0f;
  calibAccY_ = 0.0f;
  calibAccZ_ = 0.0f;
  calibrated_ = false;

  uint8_t whoAmI = 0;
  i2cAddr_ = I2C_ADDR_PRIMARY;
  if (!readReg(REG_WHO_AM_I, &whoAmI) || whoAmI != WHO_AM_I_VALUE) {
    i2cAddr_ = I2C_ADDR_ALT;
    if (!readReg(REG_WHO_AM_I, &whoAmI) || whoAmI != WHO_AM_I_VALUE) {
      LOG_INF(TAG, "QMI8658 IMU not found");
      return Ok();
    }
  }

  if (!writeReg(REG_CTRL1, 0x41) ||
      !writeReg(REG_CTRL3, 0x58) ||
      !writeReg(REG_CTRL7, 0x00)) {
    LOG_ERR(TAG, "QMI8658 register configuration failed");
    return Ok();
  }

  available_ = true;
  LOG_INF(TAG, "QMI8658 gyroscope initialized at 0x%02X in sleep mode", i2cAddr_);
  return Ok();
}

void TiltSensor::shutdown() {
  sleep();
  clearPendingEvents();
  queue_ = nullptr;
  initialized_ = false;
  available_ = false;
  isAwake_ = false;
}

void TiltSensor::wake() {
  if (!initialized_ || !available_ || isAwake_) {
    return;
  }

  if (!writeReg(REG_CTRL1, 0x40) || !writeReg(REG_CTRL7, 0x02)) {
    LOG_ERR(TAG, "Failed to wake QMI8658 gyroscope");
    return;
  }

  isAwake_ = true;
  lastPollMs_ = millis();
  lastTiltMs_ = lastPollMs_;
  wakeMs_ = lastPollMs_;
  biasX_ = 0.0f;
  biasY_ = 0.0f;
  biasZ_ = 0.0f;
  calibSamples_ = 0;
  calibAccX_ = 0.0f;
  calibAccY_ = 0.0f;
  calibAccZ_ = 0.0f;
  calibrated_ = false;
  LOG_INF(TAG, "QMI8658 gyroscope woke up");
}

void TiltSensor::sleep() {
  if (!initialized_ || !available_) {
    return;
  }

  if (!writeReg(REG_CTRL7, 0x00) || !writeReg(REG_CTRL1, 0x41)) {
    LOG_ERR(TAG, "Failed to put QMI8658 into sleep mode");
    return;
  }

  isAwake_ = false;
  inTilt_ = false;
  biasX_ = 0.0f;
  biasY_ = 0.0f;
  biasZ_ = 0.0f;
  calibSamples_ = 0;
  calibAccX_ = 0.0f;
  calibAccY_ = 0.0f;
  calibAccZ_ = 0.0f;
  calibrated_ = false;
  clearPendingEvents();
  LOG_INF(TAG, "QMI8658 entered sleep mode");
}

void TiltSensor::poll(uint8_t orientation, bool enabled) {
  if (!initialized_ || !available_) {
    return;
  }

  if (enabled && !isAwake_) {
    wake();
    return;
  }

  if (!enabled) {
    if (isAwake_) {
      sleep();
    }
    return;
  }

  const unsigned long now = millis();
  if (now - lastPollMs_ < POLL_INTERVAL_MS) {
    return;
  }
  lastPollMs_ = now;

  float gx = 0.0f;
  float gy = 0.0f;
  float gz = 0.0f;
  if (!readGyro(gx, gy, gz)) {
    return;
  }

  if (now - wakeMs_ < WAKE_STABILIZE_MS) {
    return;
  }

  if (!calibrated_) {
    calibAccX_ += gx;
    calibAccY_ += gy;
    calibAccZ_ += gz;
    ++calibSamples_;

    if (calibSamples_ >= CALIB_SAMPLE_COUNT) {
      biasX_ = calibAccX_ / CALIB_SAMPLE_COUNT;
      biasY_ = calibAccY_ / CALIB_SAMPLE_COUNT;
      biasZ_ = calibAccZ_ / CALIB_SAMPLE_COUNT;
      calibrated_ = true;
      LOG_INF(TAG, "Gyro calibrated: bias=(%.1f, %.1f, %.1f) dps", biasX_, biasY_, biasZ_);
    }
    return;
  }

  gx -= biasX_;
  gy -= biasY_;
  gz -= biasZ_;

  float tiltAxis = -gx;
  switch (orientation) {
    case kOrientationPortrait:
      tiltAxis = -gx;
      break;
    case kOrientationLandscapeCW:
      tiltAxis = gy;
      break;
    case kOrientationInverted:
      tiltAxis = gx;
      break;
    case kOrientationLandscapeCCW:
      tiltAxis = -gy;
      break;
    default:
      break;
  }

  if (inTilt_) {
    if (fabsf(tiltAxis) < NEUTRAL_RATE_DPS) {
      inTilt_ = false;
    }
    return;
  }

  if (now - lastTiltMs_ < COOLDOWN_MS) {
    return;
  }

  if (tiltAxis > RATE_THRESHOLD_DPS) {
    inTilt_ = true;
    lastTiltMs_ = now;
    pushEvent(Event::tiltForward());
  } else if (tiltAxis < -RATE_THRESHOLD_DPS) {
    inTilt_ = true;
    lastTiltMs_ = now;
    pushEvent(Event::tiltBack());
  }
}

bool TiltSensor::hadActivity() {
  const bool value = hadActivity_;
  hadActivity_ = false;
  return value;
}

void TiltSensor::clearPendingEvents() {
  tiltForwardEvent_ = false;
  tiltBackEvent_ = false;
  hadActivity_ = false;
}

}  // namespace drivers
}  // namespace papyrix
