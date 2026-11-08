#include "drive.h"

#include <cmath>
#include <cstdlib>

namespace slock {

DriveController::DriveController(hal::Motor& motor, hal::Encoder& encoder, hal::HubSensor& hub,
                                 hal::CurrentSense& current, const DriveConfig& cfg)
    : motor_(motor), enc_(encoder), hub_(hub), cur_(current), cfg_(cfg) {}

void DriveController::set_calibration(const Calibration& cal) {
  cal_ = cal;
  homed_ = false;
}

float DriveController::hub_angle() {
  int16_t a = 0, b = 0;
  hub_.read(a, b);
  return hub_angle_deg(a, b);
}

float DriveController::hub_progress() {
  if (!cal_.valid) return 0.f;
  return progress_between(hub_angle(), cal_.hub_locked, cal_.hub_unlocked);
}

bool DriveController::at_unlocked_end() { return hub_progress() >= 1.f - cfg_.end_tolerance; }
bool DriveController::at_locked_end() { return hub_progress() <= cfg_.end_tolerance; }

int32_t DriveController::park_tol() const {
  if (cfg_.park_tolerance > 0) return cfg_.park_tolerance;
  long t = std::lround(cal_.cpd);
  return t < 1 ? 1 : static_cast<int32_t>(t);
}

bool DriveController::reject(DriveStatus st) {
  op_ = DriveOp::None;
  status_ = st;
  return false;
}

bool DriveController::start(DriveOp op, Millis now) {
  if (op_ != DriveOp::None) return false;  // busy; status_ stays Busy
  failure_ = DriveStatus::Ok;
  phase_ = 0;
  op_ = op;

  switch (op) {
    case DriveOp::Calibrate:
      h0_ = hub_angle();
      homed_ = false;
      cal_.valid = false;
      begin_stall(+1, cfg_.slow_duty, now);
      return true;

    case DriveOp::Home:
      if (!cal_.valid) return reject(DriveStatus::NotCalibrated);
      if (at_locked_end()) {
        home_from_ = -1;
        begin_stall(-cal_.unlock_dir, cfg_.slow_duty, now);
      } else if (at_unlocked_end()) {
        home_from_ = +1;
        begin_stall(cal_.unlock_dir, cfg_.slow_duty, now);
      } else {
        return reject(DriveStatus::UnknownPosition);
      }
      return true;

    case DriveOp::Unlock:
    case DriveOp::Lock: {
      if (!cal_.valid) return reject(DriveStatus::NotCalibrated);
      if (!homed_) return reject(DriveStatus::NotHomed);
      bool unlock = op == DriveOp::Unlock;
      if (unlock ? at_unlocked_end() : at_locked_end()) {
        phase_ = 1;  // already there: just make sure the gear is parked
        begin_park(now);
      } else {
        begin_stall(unlock ? cal_.unlock_dir : -cal_.unlock_dir, cfg_.fast_duty, now);
      }
      return true;
    }

    case DriveOp::Park:
      if (!homed_) return reject(DriveStatus::NotHomed);
      begin_park(now);
      return true;

    case DriveOp::None:
      break;
  }
  return reject(DriveStatus::Idle);
}

void DriveController::reset_step(Millis now) {
  step_start_ = now;
  over_ = false;
  last_count_ = enc_.count();
  last_move_ = now;
  status_ = DriveStatus::Busy;
}

void DriveController::begin_stall(int dir, int16_t duty, Millis now) {
  step_ = Step::ToStall;
  step_dir_ = dir;
  step_duty_ = duty;
  reset_step(now);
}

void DriveController::begin_park(Millis now) {
  step_ = Step::ToPark;
  reset_step(now);
}

bool DriveController::stalled(int32_t count, Millis now) {
  if (cur_.milliamps() >= cfg_.stall_ma) {
    if (!over_) {
      over_ = true;
      over_since_ = now;
    } else if (since(now, over_since_) >= cfg_.stall_ms) {
      return true;
    }
  } else {
    over_ = false;
  }
  if (std::abs(count - last_count_) >= cfg_.no_motion_counts) {
    last_count_ = count;
    last_move_ = now;
  } else if (since(now, last_move_) >= cfg_.no_motion_ms) {
    return true;
  }
  return false;
}

DriveStatus DriveController::tick(Millis now) {
  if (op_ == DriveOp::None) return status_;
  int32_t e = enc_.count();

  if (since(now, step_start_) > cfg_.step_timeout_ms) {
    motor_.brake();
    return step_done(StepEnd::Timeout, e, now);
  }

  if (step_ == Step::ToStall) {
    motor_.drive(static_cast<int16_t>(step_dir_ * step_duty_));
  } else {
    int32_t err = park_ - e;
    if (std::abs(err) <= park_tol()) {
      motor_.brake();
      return step_done(StepEnd::Reached, e, now);
    }
    // Proportional approach: full speed until ~10 gear degrees out, never below slow_duty.
    float kp = static_cast<float>(cfg_.fast_duty) / (10.f * (cal_.cpd > 0.f ? cal_.cpd : 1.f));
    float duty = static_cast<float>(err) * kp;
    float mag = std::fabs(duty);
    if (mag > cfg_.fast_duty) mag = cfg_.fast_duty;
    if (mag < cfg_.slow_duty) mag = cfg_.slow_duty;
    motor_.drive(static_cast<int16_t>(err > 0 ? mag : -mag));
  }

  if (stalled(e, now)) {
    motor_.brake();
    return step_done(StepEnd::Stalled, e, now);
  }
  return status_;
}

DriveStatus DriveController::finish(DriveStatus st) {
  motor_.brake();
  op_ = DriveOp::None;
  status_ = st;
  return st;
}

DriveStatus DriveController::finish_park(StepEnd end) {
  if (end != StepEnd::Reached) {
    // The gear couldn't reach park: its position is no longer trustworthy.
    homed_ = false;
    return finish(end == StepEnd::Timeout ? DriveStatus::Timeout : DriveStatus::Jammed);
  }
  return finish(failure_ == DriveStatus::Ok ? DriveStatus::Ok : failure_);
}

DriveStatus DriveController::step_done(StepEnd end, int32_t e, Millis now) {
  if (step_ == Step::ToPark) return finish_park(end);

  switch (op_) {
    case DriveOp::Calibrate:
      if (end == StepEnd::Timeout) return finish(DriveStatus::Timeout);
      if (phase_ == 0) {
        ea_ = e;
        ha_ = hub_angle();
        phase_ = 1;
        begin_stall(-1, cfg_.slow_duty, now);
        return status_;
      }
      eb_ = e;
      hb_ = hub_angle();
      if (!compute_calibration()) return finish(DriveStatus::BadCalibration);
      homed_ = true;
      phase_ = 2;
      begin_park(now);
      return status_;

    case DriveOp::Home:
      if (end == StepEnd::Timeout) return finish(DriveStatus::Timeout);
      park_ = e - home_from_ * cal_.unlock_dir * (cal_.stall_span / 2);
      homed_ = true;
      phase_ = 1;
      begin_park(now);
      return status_;

    case DriveOp::Unlock:
    case DriveOp::Lock: {
      bool reached = op_ == DriveOp::Unlock ? at_unlocked_end() : at_locked_end();
      if (end == StepEnd::Timeout) {
        failure_ = DriveStatus::Timeout;
      } else if (!reached) {
        failure_ = DriveStatus::Jammed;
      }
      // Always park, even after a failure, so the key and knob are free again.
      phase_ = 1;
      begin_park(now);
      return status_;
    }

    case DriveOp::Park:
    case DriveOp::None:
      break;
  }
  return finish(DriveStatus::Idle);
}

bool DriveController::compute_calibration() {
  float travel = std::fabs(wrap180(ha_ - hb_));
  if (travel < cfg_.min_travel_deg || travel >= cfg_.slot_span_deg) return false;

  // The bolt started locked. If the + run moved it, + is the unlock direction.
  float moved_on_plus = std::fabs(wrap180(ha_ - h0_));
  Calibration c;
  int32_t e_unlock = 0, e_lock = 0;
  if (moved_on_plus > travel / 2.f) {
    c.unlock_dir = +1;
    e_unlock = ea_;
    c.hub_unlocked = ha_;
    e_lock = eb_;
    c.hub_locked = hb_;
  } else {
    c.unlock_dir = -1;
    e_lock = ea_;
    c.hub_locked = ha_;
    e_unlock = eb_;
    c.hub_unlocked = hb_;
  }

  int32_t span = c.unlock_dir * (e_unlock - e_lock);
  if (span <= 0) return false;
  c.cpd = static_cast<float>(span) / (travel + cfg_.slot_span_deg);
  if (cfg_.nominal_cpd > 0.f && std::fabs(c.cpd / cfg_.nominal_cpd - 1.f) > cfg_.cpd_tolerance) return false;

  c.stall_span = span;
  c.valid = true;
  cal_ = c;
  park_ = e_lock + c.unlock_dir * (span / 2);  // halfway between the two stalls
  return true;
}

}  // namespace slock
