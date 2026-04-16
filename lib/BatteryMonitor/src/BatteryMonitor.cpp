#include "BatteryMonitor.h"

#include <Arduino.h>
#include <Wire.h>

BatteryMonitor::BatteryMonitor(uint8_t adcPin, float dividerMultiplier)
    : _adcPin(adcPin), _dividerMultiplier(dividerMultiplier) {}

bool BatteryMonitor::readRegister16(uint8_t reg, uint16_t& value) {
  Wire.beginTransmission(BQ27220_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom(BQ27220_ADDR, static_cast<uint8_t>(2)) != 2) {
    return false;
  }

  const uint8_t lo = Wire.read();
  const uint8_t hi = Wire.read();
  value = static_cast<uint16_t>((hi << 8) | lo);
  return true;
}

bool BatteryMonitor::readSignedRegister16(uint8_t reg, int16_t& value) {
  uint16_t raw = 0;
  if (!readRegister16(reg, raw)) {
    return false;
  }
  value = static_cast<int16_t>(raw);
  return true;
}

uint16_t BatteryMonitor::readPercentage() const {
  // Primary: read State-of-Charge directly from the BQ27220 fuel gauge.
  // The chip computes SOC internally via its CEDV algorithm (OCV + coulomb
  // counting + temperature compensation) — far more accurate than a simple
  // voltage-to-percentage polynomial.
  uint16_t soc = 0;
  if (readRegister16(REG_SOC, soc)) {
    return soc > 100 ? 100 : soc;
  }

  // Fallback: I2C failed — try voltage-based estimate.
  // Guard against the polynomial giving nonsense at 0 V (I2C failure returns 0 mV).
  const uint16_t mv = readMillivolts();
  if (mv < 3000 || mv > 4500) {
    return 0;  // I2C down or voltage out of LiPo range — report 0 rather than fake data
  }
  return percentageFromMillivolts(mv);
}

uint16_t BatteryMonitor::readMillivolts() const {
  uint16_t millivolts = 0;
  if (readRegister16(REG_VOLTAGE, millivolts)) {
    return millivolts;
  }
  return 0;
}

uint16_t BatteryMonitor::readRawMillivolts() const { return readMillivolts(); }

double BatteryMonitor::readVolts() const { return static_cast<double>(readMillivolts()) / 1000.0; }

uint16_t BatteryMonitor::percentageFromMillivolts(uint16_t millivolts) {
  if (millivolts < 3000 || millivolts > 4500) return 0;
  const double volts = millivolts / 1000.0;
  double y = -144.9390 * volts * volts * volts + 1655.8629 * volts * volts - 6158.8520 * volts + 7501.3202;
  y = y < 0.0 ? 0.0 : y;
  y = y > 100.0 ? 100.0 : y;
  return static_cast<uint16_t>(round(y));
}

uint16_t BatteryMonitor::millivoltsFromRawAdc(uint16_t adc_raw) { return adc_raw; }
