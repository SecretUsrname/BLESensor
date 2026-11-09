#pragma once
// Door state from the board's gyro alone (no magnet sensor). See docs/v1-framework.md section 4.
//
//  * STILL (corrected rate below still_dps for still_ms): rotation is exactly zero and the gyro
//    bias is re-measured, so drift never builds up while the door rests.
//  * MOVING: the bias-corrected rate is integrated. A swing lasts a few seconds.
//  * TRUSTED CLOSE: a closing impact near the closed angle, or (once the closed reference is known)
//    net rotation back within close_tol_deg and settled. Either re-zeroes the angle.
//  * The door can't rotate past closed: a clearly negative angle means the reference was wrong.
//    It is corrected and reported, and the close isn't trusted.
//  * A locked door can't swing: rotation while the bolt is locked is reported as TAMPER.
//  * Start-up: Unsure until the first close.
//
// Sign convention: positive rate = opening. The board layer flips the axis if needed.

#include "types.h"

namespace slock {

enum class DoorState : uint8_t { Unsure, Closed, Open };

struct DoorConfig {
  float still_dps = 2.0f;          // corrected rate below this counts as still
  uint32_t still_ms = 400;         // ...for this long before the door is at rest
  float bias_alpha = 0.02f;        // bias re-measurement speed while at rest
  float max_bias_dps = 15.f;       // a "rest" reading above this isn't bias (ignored)
  float open_deg = 6.f;            // opened once the angle passes this
  float close_tol_deg = 2.f;       // within this of 0 and settled = trusted close (reference known)
  float loose_close_deg = 5.f;     // settled within this = closed, but not trusted
  float impact_mg = 400.f;         // accelerometer deviation counting as the door hitting the frame
  float impact_zone_deg = 15.f;    // an impact only counts as a close below this angle
  uint32_t swing_memory_ms = 3000; // while Unsure, an impact this soon after a swing is a close
  float overshoot_deg = 4.f;       // below -this the reference was wrong
  float tamper_deg = 4.f;          // rotation while locked beyond this = tamper
};

enum class DoorEventType : uint8_t { None, Opened, Closed, Tamper, Corrected };

struct DoorEvent {
  DoorEventType type = DoorEventType::None;
  bool trusted = false;  // Closed: impact or net-zero with a known reference
  Millis at = 0;         // Closed: the moment the door actually reached the frame
};

class DoorEstimator {
 public:
  explicit DoorEstimator(const DoorConfig& cfg = DoorConfig{}) : cfg_(cfg) {}

  // One IMU sample: hinge-axis rate (deg/s, raw incl. bias) and accelerometer deviation from
  // gravity (milli-g, magnitude). bolt_locked = the hub sensor says the bolt is thrown.
  DoorEvent on_sample(float rate_dps, float accel_dev_mg, bool bolt_locked, Millis now);

  DoorState state() const { return state_; }
  bool trusted() const { return trusted_; }          // the last close was trusted
  bool reference_known() const { return ref_known_; }
  float angle() const { return angle_; }
  float bias() const { return bias_; }
  bool at_rest() const { return at_rest_; }
  bool tamper() const { return tamper_; }            // latched until the door is back at rest near 0
  void set_bias(float dps) { bias_ = dps; }          // e.g. from a factory measurement

 private:
  DoorEvent close(bool trusted, Millis at, bool rezero);

  DoorConfig cfg_;
  DoorState state_ = DoorState::Unsure;
  bool trusted_ = false;
  bool ref_known_ = false;
  float angle_ = 0.f;
  float bias_ = 0.f;
  bool have_time_ = false;
  Millis last_ = 0;
  Millis last_motion_ = 0;
  bool at_rest_ = true;
  Millis crossed_at_ = 0;   // when the angle came back within close_tol_deg (0 = not yet)
  bool crossed_ = false;
  bool swung_ = false;      // Unsure: a real swing happened recently
  float swing_from_ = 0.f;
  bool tamper_ = false;
};

}  // namespace slock
