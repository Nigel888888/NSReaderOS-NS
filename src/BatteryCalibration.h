#pragma once

#include <cstdint>

// Host-side battery percentage display calibration.
//
// Read Pico's state of charge is computed inside the CW32L010 PMU and can only
// be read by the host (the PMU rejects a battery-profile write), so the
// correction is applied to the *displayed* percentage, not to the battery
// gauge itself. 100 in settings means no correction; 90..110 maps to -10..+10
// percentage points.
//
// This header is deliberately dependency-free: the HAL layer includes it in
// translation units that do not have ArduinoJson available, so the offset is
// kept here as a plain global that CrossPointSettings synchronizes on load and
// on save.
namespace BatteryCalibration {

// -10..+10, 0 = factory default. Defined in CrossPointSettings.cpp.
extern int8_t offsetPercent;

inline uint16_t apply(const uint16_t rawPercent) {
  int value = static_cast<int>(rawPercent) + static_cast<int>(offsetPercent);
  if (value < 0) value = 0;
  if (value > 100) value = 100;
  return static_cast<uint16_t>(value);
}

}  // namespace BatteryCalibration
