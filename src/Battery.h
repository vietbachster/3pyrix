#pragma once
#include <BatteryMonitor.h>

// X3 uses a BQ27220 fuel gauge over I2C (SDA=20, SCL=0, addr=0x55).
// The adcPin/dividerMultiplier constructor args are unused on X3.
#define BAT_GPIO0 0  // Unused on X3, kept for BatteryMonitor constructor compatibility

inline BatteryMonitor& getBatteryMonitor() {
  static BatteryMonitor instance(BAT_GPIO0);
  return instance;
}

#define batteryMonitor getBatteryMonitor()
