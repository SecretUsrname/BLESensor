#pragma once
// Hub angle from the two hall channels.
//
// The magnet only turns about 90 degrees, so the sensors never see a full sine period and their
// gains can't be calibrated from min/max. That's fine: atan2 of the raw channels is monotonic over
// the travel, and the lock/unlock ends are recorded in the same units during calibration.

#include <cmath>
#include <cstdint>

namespace slock {

inline float wrap180(float deg) {
  while (deg >= 180.f) deg -= 360.f;
  while (deg < -180.f) deg += 360.f;
  return deg;
}

inline float hub_angle_deg(int16_t a, int16_t b) {
  return std::atan2(static_cast<float>(b), static_cast<float>(a)) * 57.2957795f;
}

// 0 at the locked end, 1 at the unlocked end; slightly outside [0, 1] is normal.
// Handles ends that straddle the +-180 degree wrap.
inline float progress_between(float angle, float locked, float unlocked) {
  float span = wrap180(unlocked - locked);
  if (std::fabs(span) < 1.f) return 0.f;
  return wrap180(angle - locked) / span;
}

}  // namespace slock
