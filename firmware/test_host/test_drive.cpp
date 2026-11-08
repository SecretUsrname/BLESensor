#include "check.h"
#include "sim_plant.h"
#include "slock/angle.h"
#include "slock/drive.h"

using namespace slock;

namespace {

DriveConfig cfg_for(const SimPlant& p) {
  DriveConfig c;
  c.slot_span_deg = p.slot_span;
  c.nominal_cpd = p.cpd;
  return c;
}

DriveStatus run(DriveController& d, SimPlant& p, Millis& now, uint32_t max_ms = 20000) {
  DriveStatus s = d.status();
  for (uint32_t i = 0; i < max_ms; ++i) {
    s = d.tick(now);
    if (s != DriveStatus::Busy) return s;
    p.step(1);
    ++now;
  }
  return s;
}

// Parked = the slot spans the whole bolt travel with equal free play at both ends.
void check_parked(const SimPlant& p) {
  float play = (p.slot_span - p.travel) / 2.f;
  CHECK_NEAR(p.g, -play, 1.5);
}

// The knob/key can take the bolt end to end without fighting the gear.
void check_key_free(SimPlant& p) {
  float u0 = p.u;
  CHECK(p.turn_by_hand(p.travel));
  CHECK(p.turn_by_hand(0.f));
  p.turn_by_hand(u0);
}

Calibration calibrate(DriveController& d, SimPlant& p, Millis& now) {
  CHECK(d.start(DriveOp::Calibrate, now));
  CHECK(run(d, p, now) == DriveStatus::Ok);
  return d.calibration();
}

}  // namespace

TEST(angle_helpers) {
  CHECK_NEAR(wrap180(190.f), -170.f, 1e-4);
  CHECK_NEAR(wrap180(-190.f), 170.f, 1e-4);
  CHECK_NEAR(progress_between(45.f, 0.f, 90.f), 0.5, 1e-4);
  // ends straddling the +-180 wrap: locked 170, unlocked -100 (= 260)
  CHECK_NEAR(progress_between(-145.f, 170.f, -100.f), 0.5, 1e-3);
  CHECK_NEAR(progress_between(-100.f, 170.f, -100.f), 1.0, 1e-3);
}

TEST(calibrate_right_handed_door) {
  SimPlant p;
  DriveController d(p, p, p, p, cfg_for(p));
  Millis now = 0;
  Calibration c = calibrate(d, p, now);
  CHECK(c.valid);
  CHECK(c.unlock_dir == 1);
  CHECK_NEAR(c.cpd, p.cpd, p.cpd * 0.02);
  CHECK(d.homed());
  check_parked(p);
  CHECK_NEAR(p.u, 0.f, 0.5);  // calibration ends locked, as it started
  check_key_free(p);
}

TEST(calibrate_left_handed_door_across_angle_wrap) {
  SimPlant p;
  p.plant_dir = -1;
  p.sensor_sign = -1.f;
  p.sensor_offset = -150.f;  // hub angles run -150 .. -240 (= 120), crossing +-180
  DriveController d(p, p, p, p, cfg_for(p));
  Millis now = 0;
  Calibration c = calibrate(d, p, now);
  CHECK(c.valid);
  CHECK(c.unlock_dir == -1);
  check_parked(p);
  check_key_free(p);
}

TEST(unlock_then_lock_parks_every_time) {
  SimPlant p;
  DriveController d(p, p, p, p, cfg_for(p));
  Millis now = 0;
  calibrate(d, p, now);

  Millis t0 = now;
  CHECK(d.start(DriveOp::Unlock, now));
  CHECK(run(d, p, now) == DriveStatus::Ok);
  CHECK_NEAR(p.u, p.travel, 0.5);
  check_parked(p);
  check_key_free(p);
  CHECK(now - t0 < 1500);

  CHECK(d.start(DriveOp::Lock, now));
  CHECK(run(d, p, now) == DriveStatus::Ok);
  CHECK_NEAR(p.u, 0.f, 0.5);
  check_parked(p);
  check_key_free(p);
}

TEST(unlock_when_already_unlocked_only_parks) {
  SimPlant p;
  DriveController d(p, p, p, p, cfg_for(p));
  Millis now = 0;
  calibrate(d, p, now);
  CHECK(p.turn_by_hand(p.travel));
  CHECK(d.start(DriveOp::Unlock, now));
  CHECK(run(d, p, now) == DriveStatus::Ok);
  CHECK_NEAR(p.u, p.travel, 0.5);
  check_parked(p);
}

TEST(home_after_power_cycle_does_not_move_bolt) {
  SimPlant p;
  DriveController d(p, p, p, p, cfg_for(p));
  Millis now = 0;
  Calibration c = calibrate(d, p, now);

  CHECK(p.turn_by_hand(p.travel));  // someone unlocked by hand while the power was off
  p.power_cycle();
  DriveController d2(p, p, p, p, cfg_for(p));
  d2.set_calibration(c);
  CHECK(!d2.homed());
  CHECK(d2.start(DriveOp::Home, now));
  CHECK(run(d2, p, now) == DriveStatus::Ok);
  CHECK(d2.homed());
  CHECK_NEAR(p.u, p.travel, 0.5);  // homing pushed against the end the bolt was already at
  check_parked(p);

  CHECK(d2.start(DriveOp::Lock, now));
  CHECK(run(d2, p, now) == DriveStatus::Ok);
  CHECK_NEAR(p.u, 0.f, 0.5);
  check_parked(p);
}

TEST(home_refuses_when_bolt_mid_travel) {
  SimPlant p;
  DriveController d(p, p, p, p, cfg_for(p));
  Millis now = 0;
  Calibration c = calibrate(d, p, now);
  CHECK(p.turn_by_hand(45.f));
  p.power_cycle();
  DriveController d2(p, p, p, p, cfg_for(p));
  d2.set_calibration(c);
  CHECK(!d2.start(DriveOp::Home, now));
  CHECK(d2.status() == DriveStatus::UnknownPosition);
  CHECK_NEAR(p.u, 45.f, 1e-3);
}

TEST(jam_reports_and_still_parks_so_key_works) {
  SimPlant p;
  DriveController d(p, p, p, p, cfg_for(p));
  Millis now = 0;
  calibrate(d, p, now);
  p.jam_at = 40.f;  // bolt binds on the strike plate part-way
  CHECK(d.start(DriveOp::Unlock, now));
  CHECK(run(d, p, now) == DriveStatus::Jammed);
  CHECK_NEAR(p.u, 40.f, 0.5);
  CHECK(d.homed());
  check_parked(p);
  check_key_free(p);  // the person outside can still finish with the key
}

TEST(lock_refused_before_calibration_or_homing) {
  SimPlant p;
  DriveController d(p, p, p, p, cfg_for(p));
  Millis now = 0;
  CHECK(!d.start(DriveOp::Lock, now));
  CHECK(d.status() == DriveStatus::NotCalibrated);
  Calibration c = calibrate(d, p, now);
  DriveController d2(p, p, p, p, cfg_for(p));
  d2.set_calibration(c);
  CHECK(!d2.start(DriveOp::Unlock, now));
  CHECK(d2.status() == DriveStatus::NotHomed);
}

TEST(calibration_rejects_implausible_travel) {
  SimPlant p;
  p.travel = 30.f;  // e.g. the hub is slipping on the tailpiece
  DriveController d(p, p, p, p, cfg_for(p));
  Millis now = 0;
  CHECK(d.start(DriveOp::Calibrate, now));
  CHECK(run(d, p, now) == DriveStatus::BadCalibration);
  CHECK(!d.calibration().valid);
}

TEST(calibration_rejects_wrong_gearing) {
  SimPlant p;
  DriveConfig cfg = cfg_for(p);
  cfg.nominal_cpd = p.cpd * 2.f;  // config says a different motor ratio than the one fitted
  DriveController d(p, p, p, p, cfg);
  Millis now = 0;
  CHECK(d.start(DriveOp::Calibrate, now));
  CHECK(run(d, p, now) == DriveStatus::BadCalibration);
}
