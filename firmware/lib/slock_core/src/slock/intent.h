#pragma once
// Handle-intent detection from the XIAO Sense's built-in IMU.
//
// PLACEHOLDER ALGORITHM: a burst detector on acceleration magnitude. Several samples deviating
// from a slow baseline within a short window = "someone is working the handle or the door".
// The real thresholds (and possibly a better feature) come from the Phase 1 intent spike data.

#include "types.h"

namespace slock {

struct IntentConfig {
  float baseline_alpha = 0.02f;  // slow EMA of |a| (gravity + mounting tilt)
  uint16_t threshold_mg = 60;    // [TUNE] deviation that counts as a hit
  uint8_t min_hits = 3;          // [TUNE] hits needed...
  uint16_t window_ms = 200;      // ...within this window
  uint16_t refractory_ms = 1500; // ignore further bursts for this long after an event
};

class IntentDetector {
 public:
  explicit IntentDetector(const IntentConfig& cfg) : cfg_(cfg) {}
  // Feed every IMU sample (milli-g). Returns true exactly once per detected burst.
  bool on_sample(int16_t x_mg, int16_t y_mg, int16_t z_mg, Millis now);

 private:
  IntentConfig cfg_;
  bool have_baseline_ = false;
  float baseline_ = 0.f;
  uint8_t hits_ = 0;
  Millis first_hit_ = 0;
  bool fired_once_ = false;
  Millis last_event_ = 0;
};

}  // namespace slock
