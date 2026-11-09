#pragma once
// Top-level lock state machine. Owns the DriveController and turns requests (from BLE, the
// unlock policy and relock rules in App) into drive operations. Also notices manual and key turns.

#include "drive.h"

namespace slock {

enum class LockState : uint8_t {
  Uncalibrated,
  Calibrating,
  Homing,
  NeedsHoming,  // calibrated, but the bolt is mid-travel: waiting for it to reach an end
  Locked,
  Unlocked,
  Between,      // bolt part-way (someone is turning the knob or key)
  Unlocking,
  Locking,
  Jammed,
};

enum class Request : uint8_t { Accepted, Busy, NotReady, AlreadyThere, LowBattery };

struct LockConfig {
  uint16_t min_lock_mv = 4400;  // refuse to LOCK below this: a brown-out mid-lock can block the key
  uint32_t poll_ms = 20;
};

class LockController {
 public:
  LockController(DriveController& drive, hal::Battery& battery, const LockConfig& cfg);

  void begin(Millis now);  // homes if a calibration is loaded
  Request request_unlock(Millis now);
  Request request_lock(Millis now);
  Request request_calibration(Millis now);  // bolt must be locked by hand first
  void tick(Millis now);

  LockState state() const { return state_; }
  DriveStatus last_drive_status() const { return last_status_; }
  uint32_t change_count() const { return changes_; }  // bumps on every state change (for BLE notify)
  const DriveController& drive() const { return drive_; }
  DriveController& drive() { return drive_; }

 private:
  bool busy() const { return drive_.op() != DriveOp::None; }
  bool begin_op(DriveOp op, LockState transient, Millis now);
  void on_drive_done(DriveStatus s, Millis now);
  LockState classify();
  void set_state(LockState s, Millis now);

  DriveController& drive_;
  hal::Battery& battery_;
  LockConfig cfg_;
  LockState state_ = LockState::Uncalibrated;
  DriveStatus last_status_ = DriveStatus::Idle;
  uint32_t changes_ = 0;
  Millis last_poll_ = 0;
  float end_tol_ = 0.12f;
};

}  // namespace slock
