#pragma once
#include <cstdint>

class BatteryMonitor {
 public:
  // Constructor shape kept for source compatibility; X3 uses BQ27220 over I2C.
  explicit BatteryMonitor(uint8_t adcPin = 0, float dividerMultiplier = 1.0f);

  // Read voltage and return percentage (0-100). Returns 0 on I2C failure.
  uint16_t readPercentage() const;

  // Read the battery voltage in millivolts from BQ27220 (REG_VOLTAGE 0x08).
  // Returns 0 on I2C failure.
  uint16_t readMillivolts() const;

  // Read raw millivolts (alias for readMillivolts on X3 — no ADC divider).
  uint16_t readRawMillivolts() const;

  // Read the battery voltage in volts.
  double readVolts() const;

  // Percentage (0-100) from a millivolt value using LiPo polynomial curve.
  // Input must be in valid LiPo range (3000–4500 mV); returns 0 outside range.
  static uint16_t percentageFromMillivolts(uint16_t millivolts);

  // Calibrate a raw ADC reading and return millivolts (no-op on X3).
  static uint16_t millivoltsFromRawAdc(uint16_t adc_raw);

 private:
  uint8_t _adcPin;
  float _dividerMultiplier;

  static constexpr uint8_t BQ27220_ADDR = 0x55;
  static constexpr uint8_t REG_VOLTAGE  = 0x08;  // mV
  static constexpr uint8_t REG_CURRENT  = 0x0C;  // mA (signed)
  static constexpr uint8_t REG_SOC      = 0x2C;  // State of Charge, 0–100 %

  static bool readRegister16(uint8_t reg, uint16_t& value);
  static bool readSignedRegister16(uint8_t reg, int16_t& value);
};
