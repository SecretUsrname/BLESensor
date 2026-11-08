#pragma once
// Motor control for the lost-motion drive (see docs/02-mech-design.md).
//
// The output gear carries an arc slot; the hub lug rides in it. The gear only touches the lug
// while pushing the bolt. After every move the gear returns to its PARK position, where the slot
// spans the whole bolt travel, so the knob and the exterior key always turn freely.
//
// Calibration (bolt must start LOCKED):
//   1. Drive + slowly until stall, record encoder and hub angle.
//   2. Drive - slowly until stall, record again. The bolt has now travelled end to end.
//   3. Whichever direction moved the bolt away from "locked" is the unlock direction.
//      The encoder distance between the two stalls = (bolt travel + slot span) gear degrees,
//      and the park position is exactly halfway between the two stall positions.
// Homing (after every boot; the encoder count is lost on power-down):
//   push gently toward the end the bolt is already at. The bolt can't move, the slot end stalls
//   against the lug, and that stall is a known reference. Then park.

#include "angle.h"
#include "hal.h"

namespace slock {

struct DriveConfig {
  int16_t fast_duty = 1000;         // lock/unlock moves
  int16_t slow_duty = 450;          // calibration and homing
  uint16_t stall_ma = 600;          // current at or above this...
  uint16_t stall_ms = 60;           // ...for this long counts as a stall
  uint16_t no_motion_ms = 150;      // or the encoder stays still this long while driven
  int32_t no_motion_counts = 3;
  uint32_t step_timeout_ms = 4000;
  float slot_span_deg = 102.f;      // lug-centre span of the gear's arc slot (CAD: travel + 2 * deadband)
  float nominal_cpd = 0.f;          // expected encoder counts per output-gear degree; 0 = don't check
  float cpd_tolerance = 0.25f;      // measured cpd must be within +-25% of nominal
  float min_travel_deg = 45.f;      // bolt travel seen by the hub sensor must exceed this
  float end_tolerance = 0.12f;      // hub progress within this of 0 or 1 counts as "at that end"
  int32_t park_tolerance = 0;       // encoder counts; 0 = one gear degree
};

struct Calibration {
  bool valid = false;
  int8_t unlock_dir = 1;            // raw motor sign that turns the gear toward unlocked
  float cpd = 0.f;                  // measured encoder counts per gear degree
  float hub_locked = 0.f;           // hub angle (deg) at the locked end
  float hub_unlocked = 0.f;         // hub angle (deg) at the unlocked end
  int32_t stall_span = 0;           // encoder counts between the lock-push and unlock-push stalls
};

enum class DriveOp : uint8_t { None, Calibrate, Home, Unlock, Lock, Park };

enum class DriveStatus : uint8_t {
  Idle,
  Busy,
  Ok,
  Jammed,           // bolt didn't reach the end, or the gear couldn't park
  Timeout,
  NotHomed,         // calibrated, but the encoder reference is unknown (run Home)
  NotCalibrated,
  BadCalibration,   // calibration measurements didn't make sense
  UnknownPosition,  // can't home: bolt is between the ends (turn it by hand first)
};

class DriveController {
 public:
  DriveController(hal::Motor& motor, hal::Encoder& encoder, hal::HubSensor& hub,
                  hal::CurrentSense& current, const DriveConfig& cfg);

  void set_calibration(const Calibration& cal);  // e.g. loaded from flash; requires Home afterwards
  const Calibration& calibration() const { return cal_; }
  bool homed() const { return homed_; }
  int32_t park_position() const { return park_; }

  // Starts an operation. Returns false if busy or a precondition fails; status() says why.
  bool start(DriveOp op, Millis now);
  // Call every 1-5 ms while an operation runs. Returns Busy until it finishes.
  DriveStatus tick(Millis now);

  DriveOp op() const { return op_; }
  DriveStatus status() const { return status_; }

  float hub_angle();
  float hub_progress();  // 0 = locked, 1 = unlocked; needs a valid calibration

 private:
  enum class Step : uint8_t { ToStall, ToPark };
  enum class StepEnd : uint8_t { Stalled, Reached, Timeout };

  void begin_stall(int dir, int16_t duty, Millis now);
  void begin_park(Millis now);
  void reset_step(Millis now);
  bool stalled(int32_t count, Millis now);
  DriveStatus step_done(StepEnd end, int32_t count, Millis now);
  DriveStatus finish(DriveStatus st);
  DriveStatus finish_park(StepEnd end);
  bool reject(DriveStatus st);
  bool compute_calibration();
  int32_t park_tol() const;
  bool at_unlocked_end();
  bool at_locked_end();

  hal::Motor& motor_;
  hal::Encoder& enc_;
  hal::HubSensor& hub_;
  hal::CurrentSense& cur_;
  DriveConfig cfg_;
  Calibration cal_;

  bool homed_ = false;
  int32_t park_ = 0;

  DriveOp op_ = DriveOp::None;
  DriveStatus status_ = DriveStatus::Idle;
  DriveStatus failure_ = DriveStatus::Ok;  // remembered while the gear parks after a failed move
  int phase_ = 0;

  Step step_ = Step::ToStall;
  int step_dir_ = 1;
  int16_t step_duty_ = 0;
  Millis step_start_ = 0;
  bool over_ = false;
  Millis over_since_ = 0;
  int32_t last_count_ = 0;
  Millis last_move_ = 0;

  // calibration / homing scratch
  float h0_ = 0.f, ha_ = 0.f, hb_ = 0.f;
  int32_t ea_ = 0, eb_ = 0;
  int home_from_ = 0;  // -1 = homed from the locked end, +1 = from the unlocked end
};

}  // namespace slock
