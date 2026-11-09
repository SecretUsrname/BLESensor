#include "lock_controller.h"

namespace slock {

LockController::LockController(DriveController& drive, hal::Battery& battery, const LockConfig& cfg)
    : drive_(drive), battery_(battery), cfg_(cfg) {}

void LockController::set_state(LockState s, Millis now) {
  if (s == state_) return;
  state_ = s;
  ++changes_;
  (void)now;
}

LockState LockController::classify() {
  float p = drive_.hub_progress();
  if (p <= end_tol_) return LockState::Locked;
  if (p >= 1.f - end_tol_) return LockState::Unlocked;
  return LockState::Between;
}

bool LockController::begin_op(DriveOp op, LockState transient, Millis now) {
  if (drive_.start(op, now)) {
    set_state(transient, now);
    return true;
  }
  on_drive_done(drive_.status(), now);
  return false;
}

void LockController::begin(Millis now) {
  last_poll_ = now;
  if (drive_.calibration().valid) {
    begin_op(DriveOp::Home, LockState::Homing, now);
  } else {
    set_state(LockState::Uncalibrated, now);
  }
}

Request LockController::request_unlock(Millis now) {
  if (busy()) return Request::Busy;
  if (!drive_.calibration().valid || !drive_.homed()) return Request::NotReady;
  if (classify() == LockState::Unlocked) return Request::AlreadyThere;
  return begin_op(DriveOp::Unlock, LockState::Unlocking, now) ? Request::Accepted : Request::NotReady;
}

Request LockController::request_lock(Millis now) {
  if (busy()) return Request::Busy;
  if (!drive_.calibration().valid || !drive_.homed()) return Request::NotReady;
  if (classify() == LockState::Locked) return Request::AlreadyThere;
  if (battery_.millivolts() < cfg_.min_lock_mv) return Request::LowBattery;
  return begin_op(DriveOp::Lock, LockState::Locking, now) ? Request::Accepted : Request::NotReady;
}

Request LockController::request_calibration(Millis now) {
  if (busy()) return Request::Busy;
  return begin_op(DriveOp::Calibrate, LockState::Calibrating, now) ? Request::Accepted : Request::NotReady;
}

void LockController::on_drive_done(DriveStatus s, Millis now) {
  last_status_ = s;
  switch (s) {
    case DriveStatus::Ok:
      set_state(classify(), now);
      break;
    case DriveStatus::UnknownPosition:
    case DriveStatus::NotHomed:
      set_state(LockState::NeedsHoming, now);
      break;
    case DriveStatus::NotCalibrated:
    case DriveStatus::BadCalibration:
      set_state(LockState::Uncalibrated, now);
      break;
    case DriveStatus::Jammed:
    case DriveStatus::Timeout:
      set_state(LockState::Jammed, now);
      break;
    case DriveStatus::Idle:
    case DriveStatus::Busy:
      break;
  }
}

void LockController::tick(Millis now) {
  if (busy()) {
    DriveStatus s = drive_.tick(now);
    if (s != DriveStatus::Busy) on_drive_done(s, now);
    return;
  }
  if (since(now, last_poll_) < cfg_.poll_ms) return;
  last_poll_ = now;

  if (!drive_.calibration().valid) {
    set_state(LockState::Uncalibrated, now);
    return;
  }

  LockState seen = classify();

  if (!drive_.homed()) {
    // Lost the encoder reference (boot, or a failed park). Re-home once the bolt is at an end.
    if (seen == LockState::Between) {
      set_state(LockState::NeedsHoming, now);
    } else {
      begin_op(DriveOp::Home, LockState::Homing, now);
    }
    return;
  }

  // A jam stays reported until the bolt is seen at an end (e.g. someone finished it by hand).
  if (state_ == LockState::Jammed && seen == LockState::Between) return;

  set_state(seen, now);  // picks up knob and key turns
}

}  // namespace slock
