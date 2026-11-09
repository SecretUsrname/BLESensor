#include "check.h"
#include "fakes.h"
#include "sim_plant.h"
#include "slock/lock_controller.h"

using namespace slock;
using fakes::FakeBattery;

namespace {

DriveConfig cfg_for(const SimPlant& p) {
  DriveConfig c;
  c.slot_span_deg = p.slot_span;
  c.nominal_cpd = p.cpd;
  return c;
}

void run(LockController& lc, SimPlant& p, Millis& now, uint32_t ms) {
  for (uint32_t i = 0; i < ms; ++i) {
    lc.tick(now);
    p.step(1);
    ++now;
  }
}

struct Rig {
  SimPlant p;
  FakeBattery bat;
  DriveController drive{p, p, p, p, cfg_for(p)};
  LockController lc;
  Millis now = 0;
  explicit Rig(const LockConfig& cfg = LockConfig{}) : lc(drive, bat, cfg) {
    lc.begin(now);
  }
  void calibrate() {
    CHECK(lc.request_calibration(now) == Request::Accepted);
    run(lc, p, now, 5000);
    CHECK(lc.state() == LockState::Locked);
  }
};

}  // namespace

TEST(lock_controller_basic_flow) {
  Rig r;
  CHECK(r.lc.state() == LockState::Uncalibrated);
  CHECK(r.lc.request_unlock(r.now) == Request::NotReady);
  r.calibrate();

  CHECK(r.lc.request_unlock(r.now) == Request::Accepted);
  CHECK(r.lc.state() == LockState::Unlocking);
  run(r.lc, r.p, r.now, 3000);
  CHECK(r.lc.state() == LockState::Unlocked);
  CHECK(r.lc.request_unlock(r.now) == Request::AlreadyThere);

  CHECK(r.lc.request_lock(r.now) == Request::Accepted);
  run(r.lc, r.p, r.now, 3000);
  CHECK(r.lc.state() == LockState::Locked);
}

TEST(low_battery_blocks_lock_but_not_unlock) {
  Rig r;
  r.calibrate();
  r.bat.mv = 4000;
  CHECK(r.lc.request_unlock(r.now) == Request::Accepted);
  run(r.lc, r.p, r.now, 3000);
  CHECK(r.lc.state() == LockState::Unlocked);
  CHECK(r.lc.request_lock(r.now) == Request::LowBattery);
  run(r.lc, r.p, r.now, 100);
  CHECK(r.lc.state() == LockState::Unlocked);
}

TEST(manual_and_key_turns_are_tracked) {
  Rig r;
  r.calibrate();
  uint32_t c0 = r.lc.change_count();
  CHECK(r.p.turn_by_hand(45.f));
  run(r.lc, r.p, r.now, 100);
  CHECK(r.lc.state() == LockState::Between);
  CHECK(r.p.turn_by_hand(r.p.travel));
  run(r.lc, r.p, r.now, 100);
  CHECK(r.lc.state() == LockState::Unlocked);
  CHECK(r.lc.change_count() >= c0 + 2);
}

TEST(boot_waits_for_bolt_at_an_end_then_homes) {
  Rig r;
  r.calibrate();
  Calibration cal = r.drive.calibration();
  CHECK(r.p.turn_by_hand(50.f));
  r.p.power_cycle();

  DriveController d2(r.p, r.p, r.p, r.p, cfg_for(r.p));
  d2.set_calibration(cal);
  LockController lc2(d2, r.bat, LockConfig{});
  lc2.begin(r.now);
  CHECK(lc2.state() == LockState::NeedsHoming);
  run(lc2, r.p, r.now, 200);
  CHECK(lc2.state() == LockState::NeedsHoming);  // nothing moves while the bolt is mid-travel

  CHECK(r.p.turn_by_hand(0.f));
  run(lc2, r.p, r.now, 3000);
  CHECK(d2.homed());
  CHECK(lc2.state() == LockState::Locked);
}

TEST(jam_stays_reported_until_bolt_reaches_an_end) {
  Rig r;
  r.calibrate();
  r.p.jam_at = 40.f;
  CHECK(r.lc.request_unlock(r.now) == Request::Accepted);
  run(r.lc, r.p, r.now, 3000);
  CHECK(r.lc.state() == LockState::Jammed);
  run(r.lc, r.p, r.now, 500);
  CHECK(r.lc.state() == LockState::Jammed);
  CHECK(r.p.turn_by_hand(r.p.travel));  // finished by hand / key
  run(r.lc, r.p, r.now, 100);
  CHECK(r.lc.state() == LockState::Unlocked);
}
