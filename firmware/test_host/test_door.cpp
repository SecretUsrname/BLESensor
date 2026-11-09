// Gyro-only door estimator (docs/v1-framework.md section 4) against a simulated door.

#include <vector>

#include "check.h"
#include "door_sim.h"
#include "slock/door.h"

using namespace slock;

namespace {

struct Rig {
  DoorSim door;
  DoorEstimator est;
  Millis now = 0;
  bool locked = false;
  std::vector<DoorEvent> events;

  void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 10) {
      float accel = 0.f;
      float rate = door.step(10, accel);
      now += 10;
      DoorEvent ev = est.on_sample(rate, accel, locked, now);
      if (ev.type != DoorEventType::None) events.push_back(ev);
    }
  }
  int count(DoorEventType type) const {
    int n = 0;
    for (const auto& e : events) n += e.type == type ? 1 : 0;
    return n;
  }
  const DoorEvent* last(DoorEventType type) const {
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
      if (it->type == type) return &*it;
    }
    return nullptr;
  }
  // Open, hold, slam shut, settle: establishes the closed reference.
  void slam_cycle() {
    door.open();
    run(3000);
    door.close(true);
    run(3000);
  }
};

}  // namespace

TEST(door_starts_unsure_and_a_slam_is_a_trusted_close) {
  Rig r;
  r.run(2000);
  CHECK(r.est.state() == DoorState::Unsure);
  r.door.open();
  r.run(3000);
  CHECK(r.est.state() == DoorState::Open);
  CHECK(r.count(DoorEventType::Opened) == 1);
  CHECK_NEAR(r.est.angle(), 80.f, 3.f);
  r.door.close(true);
  r.run(3000);
  CHECK(r.est.state() == DoorState::Closed);
  CHECK(r.est.trusted());
  CHECK(r.est.reference_known());
  CHECK_NEAR(r.est.angle(), 0.f, 0.01f);
}

TEST(door_soft_close_needs_a_known_reference) {
  Rig r;
  r.door.open();
  r.run(3000);
  r.door.close(false);  // no impact, reference unknown since boot
  r.run(3000);
  CHECK(r.est.state() == DoorState::Closed);
  CHECK(!r.est.trusted());

  r.slam_cycle();
  CHECK(r.est.trusted());
  r.door.open();
  r.run(3000);
  r.door.close(false);
  r.run(1000);
  Millis reached = r.now - 1000 + 0;  // the sim reached 0 at (80 deg / 90 dps) = ~889 ms after close()
  r.run(2000);
  const DoorEvent* c = r.last(DoorEventType::Closed);
  CHECK(c != nullptr && c->trusted);
  // The close moment is when the door got back to the frame, not when it settled.
  CHECK(c != nullptr && c->at <= reached + 950 && c->at + 300 >= reached + 800);
}

TEST(door_does_not_drift_at_rest) {
  Rig r;
  r.slam_cycle();
  size_t before = r.events.size();
  r.door.bias_drift_dps_per_min = 0.5f;  // temperature change
  r.run(10 * 60 * 1000);
  CHECK(r.est.state() == DoorState::Closed);
  CHECK_NEAR(r.est.angle(), 0.f, 1.f);
  CHECK(r.events.size() == before);  // nothing happened
  CHECK_NEAR(r.est.bias(), r.door.bias_dps, 0.3f);
}

TEST(door_fifty_soft_cycles_stay_trusted) {
  Rig r;
  r.door.bias_dps = 1.2f;
  r.slam_cycle();
  int trusted = 0;
  for (int i = 0; i < 50; ++i) {
    r.door.open(30.f + static_cast<float>(i % 5) * 15.f);
    r.run(4000);
    r.door.close(false);
    r.run(3000);
    if (r.est.state() == DoorState::Closed && r.est.trusted()) ++trusted;
  }
  CHECK(trusted == 50);
  CHECK(r.count(DoorEventType::Opened) == 51);
}

TEST(door_left_ajar_is_not_a_trusted_close) {
  Rig r;
  r.slam_cycle();
  r.door.open();
  r.run(3000);
  r.door.target = 4.f;  // pushed to, but not into, the frame
  r.run(3000);
  CHECK(r.est.state() == DoorState::Closed);
  CHECK(!r.est.trusted());
  r.door.open();
  r.run(3000);
  r.door.target = 12.f;
  r.run(3000);
  CHECK(r.est.state() == DoorState::Open);  // clearly open: not closed at all
}

TEST(door_knocks_are_not_closes) {
  Rig r;
  r.slam_cycle();
  size_t before = r.events.size();
  r.door.knock_mg = 800.f;
  r.run(1000);
  CHECK(r.events.size() == before);
  r.door.open(40.f);
  r.run(3000);
  r.door.knock_mg = 800.f;  // someone knocks on the open, resting door
  r.run(1000);
  CHECK(r.est.state() == DoorState::Open);
}

TEST(door_rotation_while_locked_is_tamper) {
  Rig r;
  r.slam_cycle();
  r.locked = true;
  r.door.forced_dps = 5.f;  // someone forcing the door
  r.run(1500);
  r.door.forced_dps = 0.f;
  r.run(500);
  CHECK(r.count(DoorEventType::Tamper) == 1);
  CHECK(r.est.tamper());
}
