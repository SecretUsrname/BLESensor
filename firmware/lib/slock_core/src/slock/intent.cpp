#include "intent.h"

#include <cmath>

namespace slock {

bool IntentDetector::on_sample(int16_t x_mg, int16_t y_mg, int16_t z_mg, Millis now) {
  float x = x_mg, y = y_mg, z = z_mg;
  float mag = std::sqrt(x * x + y * y + z * z);
  if (!have_baseline_) {
    baseline_ = mag;
    have_baseline_ = true;
    return false;
  }

  float dev = std::fabs(mag - baseline_);
  if (dev < cfg_.threshold_mg) {
    baseline_ += cfg_.baseline_alpha * (mag - baseline_);  // only track the baseline when quiet
    return false;
  }

  if (hits_ == 0 || since(now, first_hit_) > cfg_.window_ms) {
    hits_ = 1;
    first_hit_ = now;
  } else if (hits_ < 255) {
    ++hits_;
  }

  if (hits_ >= cfg_.min_hits) {
    hits_ = 0;
    if (fired_once_ && since(now, last_event_) < cfg_.refractory_ms) return false;
    fired_once_ = true;
    last_event_ = now;
    return true;
  }
  return false;
}

}  // namespace slock
