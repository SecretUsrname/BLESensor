#include "door.h"

#include <cmath>

namespace slock {

DoorEvent DoorEstimator::close(bool trusted, Millis at, bool rezero) {
  state_ = DoorState::Closed;
  trusted_ = trusted;
  if (rezero) angle_ = 0.f;
  if (trusted) ref_known_ = true;
  crossed_ = false;
  swing_from_ = angle_;
  tamper_ = false;
  DoorEvent ev;
  ev.type = DoorEventType::Closed;
  ev.trusted = trusted;
  ev.at = at;
  return ev;
}

DoorEvent DoorEstimator::on_sample(float rate_dps, float accel_dev_mg, bool bolt_locked, Millis now) {
  float dt = 0.f;
  if (have_time_) {
    dt = static_cast<float>(since(now, last_)) / 1000.f;
    if (dt > 0.05f) dt = 0.05f;  // a gap in samples: don't integrate across it
  }
  have_time_ = true;
  last_ = now;

  // Zero-velocity logic: integrate only while moving, re-measure bias while at rest.
  const float corr = rate_dps - bias_;
  if (std::fabs(corr) > cfg_.still_dps) {
    last_motion_ = now;
    at_rest_ = false;
  } else if (!at_rest_ && since(now, last_motion_) >= cfg_.still_ms) {
    at_rest_ = true;
  }
  if (at_rest_) {
    if (std::fabs(rate_dps) < cfg_.max_bias_dps) bias_ += cfg_.bias_alpha * (rate_dps - bias_);
  } else {
    angle_ += corr * dt;
  }

  // An impact only counts while the door is (or just was) moving: a knock on a resting door isn't a close.
  const bool impact = accel_dev_mg >= cfg_.impact_mg && since(now, last_motion_) <= 300;
  DoorEvent ev;

  switch (state_) {
    case DoorState::Unsure:
      if (at_rest_) {
        swing_from_ = angle_;
      } else if (angle_ - swing_from_ >= cfg_.open_deg) {
        state_ = DoorState::Open;  // reference still unknown
        ev.type = DoorEventType::Opened;
        ev.at = now;
      } else if (swing_from_ - angle_ >= cfg_.open_deg) {
        state_ = DoorState::Open;  // it was open at start-up and is closing: no Opened event
      }
      break;

    case DoorState::Closed:
      if (ref_known_ && angle_ <= -cfg_.overshoot_deg) {
        angle_ = 0.f;  // can't be past closed: the reference drifted
        ev.type = DoorEventType::Corrected;
        ev.at = now;
      } else if (angle_ - swing_from_ >= cfg_.open_deg) {
        state_ = DoorState::Open;
        crossed_ = false;
        ev.type = DoorEventType::Opened;
        ev.at = now;
      }
      break;

    case DoorState::Open:
      if (ref_known_ && angle_ <= cfg_.close_tol_deg) {
        if (!crossed_) {
          crossed_ = true;
          crossed_at_ = now;
        }
      } else {
        crossed_ = false;
      }
      if (impact && (!ref_known_ || angle_ <= cfg_.impact_zone_deg)) {
        ev = close(true, now, true);
      } else if (ref_known_ && angle_ <= -cfg_.overshoot_deg) {
        ev = close(false, now, true);
        ev.type = DoorEventType::Corrected;
      } else if (at_rest_) {
        if (ref_known_ && angle_ <= cfg_.close_tol_deg) {
          ev = close(true, crossed_ ? crossed_at_ : now, true);
        } else if (ref_known_ && angle_ <= cfg_.loose_close_deg) {
          ev = close(false, now, false);
        } else if (!ref_known_ && std::fabs(angle_ - swing_from_) <= cfg_.close_tol_deg) {
          ev = close(false, now, false);  // back where it started, but the start is unverified
        }
      }
      break;
  }

  if (tamper_ && at_rest_ && std::fabs(angle_ - swing_from_) <= cfg_.close_tol_deg) tamper_ = false;
  if (ev.type == DoorEventType::None && bolt_locked && !tamper_ && std::fabs(angle_ - swing_from_) >= cfg_.tamper_deg) {
    tamper_ = true;  // a locked door can't swing
    ev.type = DoorEventType::Tamper;
    ev.at = now;
  }
  return ev;
}

}  // namespace slock
