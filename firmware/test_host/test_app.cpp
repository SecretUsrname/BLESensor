// The v1 framework end to end: docs/v1-framework.md section 12, one test per scenario row (plus
// the supporting rules). A simulated door with a gyro, simulated phones that walk between places
// with signal levels that depend on the door, and the real App on top.

#include <vector>

#include "check.h"
#include "door_sim.h"
#include "fakes.h"
#include "sim_plant.h"
#include "slock/app.h"

using namespace slock;

namespace {

const uint8_t kLockId[kLockIdLen] = {'T', 'E', 'S', 'T', 'L', 'O', 'C', 'K'};
constexpr uint32_t kMin = 60000;

DriveConfig cfg_for(const SimPlant& p) {
  DriveConfig c;
  c.slot_span_deg = p.slot_span;
  c.nominal_cpd = p.cpd;
  return c;
}

enum class Pos { Hallway, TwoMetres, OutsideDoor, InsideLock, InsideRoom, Bedroom };

struct Phone {
  uint8_t pub[kPubKeyLen];
  int id = -1;
  uint16_t conn = 0;
  bool connected = false;
  Pos pos = Pos::Hallway;
  int login_version = 3;
  bool has_home = true;
  bool inside_home = true;
  bool have_exit = false, have_entry = false;
  Millis exit_at = 0, entry_at = 0;
  bool walking = false;
  bool ever_stepped = false;
  Millis last_step = 0;
  uint32_t steps = 0;
  uint32_t rng = 777;
};

struct Rig {
  SimPlant p;
  fakes::FakeBattery bat;
  fakes::FakeVerifier ver;
  fakes::CountingRng rng;
  fakes::MemStorage store;
  KeyStore keys;
  DriveController drive{p, p, p, p, cfg_for(p)};
  LockController lock{drive, bat, LockConfig{}};
  App app;
  DoorSim door;
  Phone a, b;
  Millis now = 0;

  explicit Rig(AppConfig cfg = {}) : app(lock, keys, ver, rng, bat, kLockId, cfg, &store) {
    for (size_t i = 0; i < kPubKeyLen; ++i) {
      a.pub[i] = static_cast<uint8_t>(i * 7 + 3);
      b.pub[i] = static_cast<uint8_t>(i * 13 + 5);
    }
    a.id = keys.add(a.pub);
    b.id = keys.add(b.pub);
    a.conn = 1;
    b.conn = 2;
    b.rng = 999;
    app.begin(now);
    CHECK(lock.request_calibration(now) == Request::Accepted);
    run(5000);
    CHECK(locked());
    // Install: someone unlocks, opens and slams the door once, so the gyro knows "closed".
    CHECK(lock.request_unlock(now) == Request::Accepted);
    run(2000);
    open_door();
    close_door(true);
    run(5000);  // relocks after the trusted close
    CHECK(locked());
    CHECK(app.door().reference_known());
  }

  bool door_open() const { return door.angle > 6.f; }

  int8_t level(Phone& ph) {
    float v = -80.f;
    switch (ph.pos) {
      case Pos::Hallway: v = -80.f; break;
      case Pos::TwoMetres: v = -68.f; break;
      case Pos::OutsideDoor: v = door_open() ? -50.f : -58.f; break;  // the door blocks ~8 dB
      case Pos::InsideLock: v = -47.f; break;
      case Pos::InsideRoom: v = -52.f; break;
      case Pos::Bedroom: v = -75.f; break;
    }
    ph.rng = ph.rng * 1664525u + 1013904223u;
    v += static_cast<float>((ph.rng >> 16) % 3) - 1.f;  // +-1 dB noise
    return static_cast<int8_t>(v);
  }

  void ctx_bytes(const Phone& ph, uint8_t* c) {
    auto minutes = [&](bool have, Millis t) -> uint16_t {
      return have ? static_cast<uint16_t>(since(now, t) / kMin) : kNoMinutes;
    };
    uint16_t entry = minutes(ph.have_entry, ph.entry_at);
    uint16_t exit = minutes(ph.have_exit, ph.exit_at);
    uint16_t still = ph.ever_stepped ? static_cast<uint16_t>(since(now, ph.last_step) / 1000u) : 0xFFFF;
    c[0] = static_cast<uint8_t>((ph.has_home ? 1 : 0) | (ph.inside_home ? 2 : 0) | 4);
    c[1] = static_cast<uint8_t>(entry);
    c[2] = static_cast<uint8_t>(entry >> 8);
    c[3] = static_cast<uint8_t>(exit);
    c[4] = static_cast<uint8_t>(exit >> 8);
    for (int i = 0; i < 4; ++i) c[5 + i] = static_cast<uint8_t>(ph.steps >> (8 * i));
    c[9] = static_cast<uint8_t>(still);
    c[10] = static_cast<uint8_t>(still >> 8);
  }

  // What the iOS app does with a challenge.
  AuthResult answer(Phone& ph, const uint8_t* nonce) {
    if (nonce == nullptr) return AuthResult::NoChallenge;
    uint8_t resp[1 + kContextLenV3 + kSigLen];
    resp[0] = static_cast<uint8_t>(ph.id);
    if (ph.login_version == 1) {
      uint8_t msg[kAuthMsgLen];
      Session::build_message(kLockId, nonce, msg);
      fakes::fake_sign(ph.pub, msg, kAuthMsgLen, resp + 1);
      return app.on_auth_response(ph.conn, resp, 1 + kSigLen, now);
    }
    uint8_t c[kContextLenV3];
    ctx_bytes(ph, c);
    uint8_t msg[kAuthMsgLenV3];
    Session::build_message_v3(kLockId, nonce, c, msg);
    for (size_t i = 0; i < kContextLenV3; ++i) resp[1 + i] = c[i];
    fakes::fake_sign(ph.pub, msg, kAuthMsgLenV3, resp + 1 + kContextLenV3);
    return app.on_auth_response(ph.conn, resp, sizeof(resp), now);
  }

  AuthResult connect(Phone& ph) {
    ph.connected = true;
    return answer(ph, app.on_connect(ph.conn, now));
  }
  void disconnect(Phone& ph) {
    ph.connected = false;
    app.on_disconnect(ph.conn, now);
  }

  void run(uint32_t ms) {
    for (uint32_t i = 0; i < ms; ++i) {
      app.tick(now);
      p.step(1);
      ++now;
      if (now % 10 == 0) {
        float accel = 0.f;
        float rate = door.step(10, accel);
        app.on_imu(rate, accel, now);
      }
      for (Phone* ph : {&a, &b}) {
        if (ph->walking) {
          ph->ever_stepped = true;
          ph->last_step = now;
          if (now % 500 == 0) ++ph->steps;
        }
        if (!ph->connected) continue;
        if (now % 100 == 0) app.on_rssi(ph->conn, level(*ph), now);
        const uint8_t* c = app.take_new_challenge(ph->conn);
        if (c != nullptr) answer(*ph, c);
      }
    }
  }

  void open_door() {
    door.open();
    run(1500);
  }
  void close_door(bool slam = true) {
    door.close(slam);
    run(1500);
  }
  void turn_handle() {
    app.on_intent(now);
    run(2500);
  }
  void leave_home(Phone& ph) {
    ph.inside_home = false;
    ph.have_exit = true;
    ph.exit_at = now;
  }
  void enter_home(Phone& ph) {
    ph.inside_home = true;
    ph.have_entry = true;
    ph.entry_at = now;
  }
  // Phone leaves the home area (while disconnected), comes back later and walks up to the door.
  void come_home(Phone& ph, uint32_t away_ms = 20 * kMin) {
    if (ph.connected) disconnect(ph);
    run(2 * kMin);
    leave_home(ph);
    run(away_ms);
    enter_home(ph);
    run(30000);  // car park to lobby
    ph.walking = true;
    ph.pos = Pos::Hallway;
    CHECK(connect(ph) == AuthResult::Ok);
    run(20000);
    ph.pos = Pos::OutsideDoor;
    run(3000);
  }

  Side side(const Phone& ph) const { return app.sides().side(static_cast<uint8_t>(ph.id)); }
  bool locked() const { return lock.state() == LockState::Locked; }
  bool unlocked() const { return lock.state() == LockState::Unlocked; }
  CmdResult cmd(Phone& ph, std::initializer_list<uint8_t> bytes) {
    std::vector<uint8_t> v(bytes);
    return app.on_command(ph.conn, v.data(), v.size(), now);
  }
  int count(EventType t) const {
    Event ev[EventLog::kCapacity];
    uint16_t n = app.log().read(0, ev, EventLog::kCapacity);
    int c = 0;
    for (uint16_t i = 0; i < n; ++i) c += ev[i].type == static_cast<uint8_t>(t) ? 1 : 0;
    return c;
  }
};

}  // namespace

// Arrive, walk to the door, turn the handle.
TEST(v1_arrive_and_turn_handle_unlocks) {
  Rig r;
  r.come_home(r.a);
  CHECK(r.side(r.a) == Side::Outside);
  CHECK(r.app.link_info(1, r.now).at_door);
  CHECK(r.app.link_info(1, r.now).ready);
  CHECK(r.locked());
  r.turn_handle();
  CHECK(r.unlocked());
  CHECK(r.app.auto_unlocks() == 1);
  CHECK(r.count(EventType::AutoUnlock) == 1);
}

// Nothing happens without a handle turn, and nothing for a phone that's never been away.
TEST(v1_no_handle_or_never_left_means_no_unlock) {
  Rig r;
  r.come_home(r.a);
  r.run(10000);
  CHECK(r.locked());  // at the door, but no handle turn

  r.b.pos = Pos::InsideRoom;
  r.b.walking = true;
  CHECK(r.connect(r.b) == AuthResult::Ok);
  CHECK(r.side(r.b) == Side::Unknown);  // since boot, never seen leave
  r.disconnect(r.a);
  r.b.pos = Pos::InsideLock;
  r.run(3000);
  r.turn_handle();
  CHECK(r.locked());
}

// Sit in the car first; chat at the door; retry after not going in (relock if not opened).
TEST(v1_relock_if_not_opened_and_retry) {
  Rig r;
  r.come_home(r.a, 40 * kMin);
  r.run(60000);  // chatting at the door for a minute
  r.turn_handle();
  CHECK(r.unlocked());
  r.run(14000);
  CHECK(r.unlocked());
  r.run(2000);  // 15 s without the door opening
  CHECK(r.locked());
  CHECK(r.count(EventType::AutoRelock) >= 1);
  CHECK(r.side(r.a) == Side::Outside);  // unchanged: retrying works
  r.turn_handle();
  CHECK(r.unlocked());
}

// Walking in: the side becomes Inside, the lock relocks after the trusted close, and a handle
// turn afterwards does nothing.
TEST(v1_walking_in_makes_you_inside) {
  Rig r;
  r.come_home(r.a);
  r.turn_handle();
  CHECK(r.unlocked());
  r.open_door();
  r.a.pos = Pos::InsideLock;
  r.run(1000);
  r.close_door(true);
  r.a.pos = Pos::InsideRoom;
  r.run(20000);
  CHECK(r.side(r.a) == Side::Inside);
  CHECK(r.locked());
  r.a.pos = Pos::InsideLock;
  r.run(2000);
  r.turn_handle();
  CHECK(r.locked());
}

// Fetch the mail and come back: leaving re-evaluates to Outside.
TEST(v1_mail_trip) {
  Rig r;
  r.come_home(r.a);
  r.turn_handle();
  r.open_door();
  r.a.pos = Pos::InsideLock;
  r.close_door(true);
  r.a.pos = Pos::InsideRoom;
  r.run(20000);
  CHECK(r.side(r.a) == Side::Inside);

  // Out to the mailbox: knob, open, step out, pull the door shut, walk off.
  r.a.pos = Pos::InsideLock;
  r.run(3000);
  CHECK(r.lock.request_unlock(r.now) == Request::Accepted);  // inside knob
  r.run(1500);
  r.open_door();
  r.a.pos = Pos::OutsideDoor;
  r.run(1000);
  r.close_door(true);
  r.run(2000);
  r.a.pos = Pos::Hallway;
  r.run(20000);
  CHECK(r.side(r.a) == Side::Outside);
  CHECK(r.locked());  // relocked behind them

  r.a.pos = Pos::OutsideDoor;
  r.run(3000);
  r.turn_handle();
  CHECK(r.unlocked());
}

// Unlock, open the door fully, don't go in, close it, leave; come back later.
TEST(v1_open_but_dont_go_in) {
  Rig r;
  r.come_home(r.a);
  r.turn_handle();
  r.open_door();
  r.run(3000);  // standing in the doorway, outside
  r.close_door(true);
  r.run(2000);
  r.a.pos = Pos::Hallway;
  r.run(20000);
  CHECK(r.side(r.a) == Side::Outside);
  CHECK(r.locked());
  r.a.pos = Pos::OutsideDoor;
  r.run(3000);
  r.turn_handle();
  CHECK(r.unlocked());
}

// A couple walks in together: both Inside.
TEST(v1_couple_walks_in_together) {
  Rig r;
  r.disconnect(r.b);
  r.come_home(r.a);
  r.b.walking = true;
  r.b.have_exit = true;
  r.b.exit_at = r.a.exit_at;
  r.b.have_entry = true;
  r.b.entry_at = r.a.entry_at;
  r.b.pos = Pos::OutsideDoor;
  CHECK(r.connect(r.b) == AuthResult::Ok);
  r.run(3000);
  CHECK(r.side(r.b) == Side::Outside);
  r.turn_handle();
  CHECK(r.unlocked());
  r.open_door();
  r.a.pos = Pos::InsideLock;
  r.b.pos = Pos::InsideLock;
  r.run(1500);
  r.close_door(true);
  r.a.pos = Pos::InsideRoom;
  r.b.pos = Pos::InsideRoom;
  r.run(20000);
  CHECK(r.side(r.a) == Side::Inside);
  CHECK(r.side(r.b) == Side::Inside);
}

// The partner inside opens the door to greet you: you become Inside, they stay Inside.
TEST(v1_partner_inside_greets_you) {
  Rig r;
  r.b.pos = Pos::InsideRoom;
  CHECK(r.connect(r.b) == AuthResult::Ok);
  CHECK(r.cmd(r.b, {0x08, 1}) == CmdResult::Ok);  // b confirmed "I'm inside" in the app
  r.come_home(r.a);
  r.b.walking = true;
  r.b.pos = Pos::InsideLock;
  r.run(3000);
  CHECK(r.lock.request_unlock(r.now) == Request::Accepted);  // b turns the knob
  r.run(1500);
  r.open_door();
  r.a.pos = Pos::InsideLock;
  r.run(1500);
  r.close_door(true);
  r.a.pos = Pos::InsideRoom;
  r.b.pos = Pos::InsideRoom;
  r.run(20000);
  CHECK(r.side(r.a) == Side::Inside);
  CHECK(r.side(r.b) == Side::Inside);
}

// Phone in the key bowl by the door, a stranger turns the handle. Even if the user had set their
// side wrongly to Outside, a phone that isn't being carried can't unlock.
TEST(v1_phone_in_key_bowl) {
  Rig r;
  r.come_home(r.a);
  r.turn_handle();
  r.open_door();
  r.a.pos = Pos::InsideLock;
  r.close_door(true);
  r.a.walking = false;  // put down in the bowl by the door
  r.run(20000);
  CHECK(r.side(r.a) == Side::Inside);
  r.run(60000);
  r.turn_handle();
  CHECK(r.locked());

  CHECK(r.cmd(r.a, {0x08, 2}) == CmdResult::Ok);  // wrong correction: "I'm outside"
  CHECK(r.side(r.a) == Side::Outside);
  r.turn_handle();
  CHECK(r.locked());  // not carried in the last 30 s
  CHECK(!r.app.link_info(1, r.now).carried);
}

// Relay attack while you're away: your phone signs "not home".
TEST(v1_relay_while_away) {
  Rig r;
  r.leave_home(r.a);
  r.a.walking = true;
  r.a.pos = Pos::OutsideDoor;  // the relay makes it look like it's at the door
  CHECK(r.connect(r.a) == AuthResult::Ok);
  r.run(3000);
  CHECK(r.side(r.a) == Side::Outside);  // away anchor
  CHECK(!r.app.link_info(1, r.now).home_ok);
  r.turn_handle();
  CHECK(r.locked());
}

// Relay attack while you're home or asleep: you're Inside.
TEST(v1_relay_while_home) {
  Rig r;
  r.come_home(r.a);
  r.turn_handle();
  r.open_door();
  r.a.pos = Pos::InsideLock;
  r.close_door(true);
  r.a.pos = Pos::Bedroom;
  r.run(20000);
  CHECK(r.side(r.a) == Side::Inside);
  r.a.pos = Pos::OutsideDoor;  // relayed signal
  r.run(3000);
  r.turn_handle();
  CHECK(r.locked());
}

// Bluetooth off outside, walk in, Bluetooth on, phone left by the door.
TEST(v1_bluetooth_off_walk_in) {
  Rig r;
  r.come_home(r.a);
  CHECK(r.side(r.a) == Side::Outside);
  r.disconnect(r.a);  // Bluetooth off at the door
  r.lock.request_unlock(r.now);  // in with the key
  r.run(2000);
  r.open_door();
  r.close_door(true);
  r.run(60000);
  r.a.walking = false;
  r.a.pos = Pos::InsideLock;  // left by the door
  r.run(60000);
  CHECK(r.connect(r.a) == AuthResult::Ok);  // Bluetooth back on
  r.run(3000);
  CHECK(r.side(r.a) == Side::Unknown);
  CHECK(r.count(EventType::SideChanged) >= 2);
  r.turn_handle();
  CHECK(r.locked());
}

// At work while someone at home takes a delivery: your Outside stands.
TEST(v1_delivery_while_away) {
  Rig r;
  r.run(2 * kMin);
  r.leave_home(r.a);
  r.run(10 * kMin);
  r.lock.request_unlock(r.now);  // someone at home opens the door for a delivery
  r.run(2000);
  r.open_door();
  r.close_door(true);
  r.run(30 * kMin);
  r.enter_home(r.a);
  r.run(30000);
  r.a.walking = true;
  r.a.pos = Pos::Hallway;
  CHECK(r.connect(r.a) == AuthResult::Ok);
  CHECK(r.side(r.a) == Side::Outside);
  r.a.pos = Pos::OutsideDoor;
  r.run(3000);
  r.turn_handle();
  CHECK(r.unlocked());
}

// A door event shortly before or after the return can't be explained: Unknown.
TEST(v1_door_event_near_return_is_unexplained) {
  Rig r;
  r.run(2 * kMin);
  r.leave_home(r.a);
  r.run(20 * kMin);
  r.enter_home(r.a);
  r.run(kMin);
  r.lock.request_unlock(r.now);
  r.run(2000);
  r.open_door();
  r.close_door(true);
  r.run(kMin);
  r.a.walking = true;
  r.a.pos = Pos::OutsideDoor;
  CHECK(r.connect(r.a) == AuthResult::Ok);
  CHECK(r.side(r.a) == Side::Unknown);
}

// Left by a second exit, came round to the front: no auto-unlock until the user says "outside".
TEST(v1_second_exit_then_user_correction) {
  Rig r;
  r.come_home(r.a);
  r.turn_handle();
  r.open_door();
  r.a.pos = Pos::InsideLock;
  r.close_door(true);
  r.a.pos = Pos::InsideRoom;
  r.run(20000);
  CHECK(r.side(r.a) == Side::Inside);
  r.a.pos = Pos::Bedroom;  // out the back door...
  r.run(30000);
  r.a.pos = Pos::Hallway;  // ...and round to the front
  r.run(30000);
  r.a.pos = Pos::OutsideDoor;
  r.run(3000);
  r.turn_handle();
  CHECK(r.locked());
  CHECK(r.cmd(r.a, {0x01, 2}) == CmdResult::Ok);  // app Unlock + "I'm outside"
  CHECK(r.side(r.a) == Side::Outside);
  CHECK(r.app.sides().reason(static_cast<uint8_t>(r.a.id)) == SideReason::User);
  r.run(2000);
  CHECK(r.unlocked());
  // Tracking carries on from the corrected side: walking in makes it Inside again.
  r.open_door();
  r.a.pos = Pos::InsideLock;
  r.close_door(true);
  r.a.pos = Pos::InsideRoom;
  r.run(20000);
  CHECK(r.side(r.a) == Side::Inside);
}

// While the side decision after a close is pending, the phone can't unlock.
TEST(v1_pending_side_blocks_unlock) {
  Rig r;
  r.come_home(r.a);
  r.turn_handle();
  r.open_door();
  r.close_door(false);  // closes while still outside
  CHECK(r.app.link_info(1, r.now).side_pending);
  CHECK(!r.app.link_info(1, r.now).ready);
}

// The door is never locked while it is known to be open.
TEST(v1_never_lock_into_open_door) {
  Rig r;
  r.come_home(r.a);
  CHECK(r.cmd(r.a, {0x01}) == CmdResult::Ok);
  r.run(2000);
  r.open_door();
  CHECK(r.cmd(r.a, {0x02}) == CmdResult::DoorOpen);
  r.run(30000);  // propped open: stays unlocked
  CHECK(r.unlocked());
  r.close_door(true);
  r.run(5000);
  CHECK(r.locked());  // relocked after the trusted close
}

// Strict mode (default) refuses phones that don't sign home context and steps.
TEST(v1_strict_mode) {
  Rig r;
  r.a.login_version = 1;
  r.a.walking = true;
  r.a.pos = Pos::OutsideDoor;
  CHECK(r.connect(r.a) == AuthResult::Ok);
  CHECK(r.cmd(r.a, {0x08, 2}) == CmdResult::Ok);
  r.run(3000);
  r.turn_handle();
  CHECK(r.locked());
  CHECK(r.cmd(r.a, {0x0A, 0}) == CmdResult::Ok);  // strict off
  r.turn_handle();
  CHECK(r.unlocked());
}

// Rotation of a locked door is tamper.
TEST(v1_tamper_is_logged) {
  Rig r;
  r.door.forced_dps = 6.f;
  r.run(1500);
  r.door.forced_dps = 0.f;
  r.run(500);
  CHECK(r.count(EventType::Tamper) == 1);
}

// Install wizard: the four door calibration steps.
TEST(v1_door_calibration_wizard) {
  Rig r;
  r.a.walking = true;
  r.a.pos = Pos::OutsideDoor;
  CHECK(r.connect(r.a) == AuthResult::Ok);
  r.run(3000);
  CHECK(r.cmd(r.a, {0x09, 1}) == CmdResult::Ok);
  r.a.pos = Pos::InsideLock;
  r.run(3000);
  CHECK(r.cmd(r.a, {0x09, 2}) == CmdResult::Ok);
  r.a.pos = Pos::TwoMetres;
  r.run(3000);
  CHECK(r.cmd(r.a, {0x09, 3}) == CmdResult::Ok);
  const DoorCalibration& c = r.app.door_calibration();
  CHECK_NEAR(c.outside_at_door, -58, 2);
  CHECK_NEAR(c.inside_at_lock, -47, 2);
  CHECK_NEAR(c.two_m, -68, 2);
  CHECK(c.steps_done == 7);

  r.a.pos = Pos::OutsideDoor;
  r.run(3000);
  CHECK(r.cmd(r.a, {0x09, 4}) == CmdResult::Ok);
  CHECK(r.cmd(r.a, {0x01}) == CmdResult::Ok);
  r.run(2000);
  r.open_door();
  r.run(2000);
  r.close_door(true);
  r.run(3000);
  CHECK_NEAR(r.app.door_calibration().block_db, 8, 2);
  CHECK(r.app.door_calibration().steps_done == 15);
  CHECK(r.count(EventType::DoorCalibrated) == 4);
}

// Keys, settings and the motor calibration survive a reboot; sides don't.
TEST(v1_reboot_keeps_setup_but_forgets_sides) {
  Rig r;
  r.come_home(r.a);
  CHECK(r.side(r.a) == Side::Outside);
  CHECK(r.cmd(r.a, {0x05, 10, 0}) == CmdResult::Ok);  // relock 10 s after a close

  KeyStore keys2;
  DriveController drive2{r.p, r.p, r.p, r.p, cfg_for(r.p)};
  LockController lock2{drive2, r.bat, LockConfig{}};
  App app2(lock2, keys2, r.ver, r.rng, r.bat, kLockId, AppConfig{}, &r.store);
  app2.begin(r.now);
  CHECK(keys2.count() == 2);
  CHECK(app2.config().relock_after_close_ms == 10000);
  CHECK(drive2.calibration().valid);
  CHECK(app2.sides().side(static_cast<uint8_t>(r.a.id)) == Side::Unknown);
  for (int i = 0; i < 3000; ++i) {
    app2.tick(r.now);
    r.p.step(1);
    ++r.now;
  }
  CHECK(lock2.state() == LockState::Locked);  // homed from the saved calibration
}

// The event log pages out over the Log characteristic.
TEST(v1_event_log_pages) {
  Rig r;
  r.come_home(r.a);
  r.turn_handle();
  CHECK(r.cmd(r.a, {0x0B, 1, 0, 0, 0}) == CmdResult::Ok);
  uint8_t page[1 + protocol::kLogPageEvents * sizeof(Event)];
  size_t n = r.app.log_page(1, page, sizeof(page));
  CHECK(n >= 1 + sizeof(Event));
  Event first;
  std::memcpy(&first, page + 1, sizeof(Event));
  CHECK(first.seq == 1);
  CHECK(first.type == static_cast<uint8_t>(EventType::Boot));
  // Keep reading until empty; the last event is the auto-unlock.
  Event last = first;
  for (int guard = 0; guard < 20; ++guard) {
    size_t m = r.app.log_page(1, page, sizeof(page));
    if (page[0] == 0) break;
    std::memcpy(&last, page + 1 + (page[0] - 1) * sizeof(Event), sizeof(Event));
    CHECK(m == 1 + page[0] * sizeof(Event));
  }
  CHECK(last.type == static_cast<uint8_t>(EventType::AutoUnlock));
  CHECK(r.app.status(1, r.now).log_next_seq == last.seq + 1);
}

// Status packet v2 carries the side and door state, and stays compatible in its first 13 bytes.
TEST(v1_status_packet) {
  Rig r;
  r.come_home(r.a);
  protocol::StatusPacket s = r.app.status(1, r.now);
  CHECK(s.version == 2);
  CHECK(s.auth == 1);
  CHECK(s.side == static_cast<uint8_t>(Side::Outside));
  CHECK(s.door_state == static_cast<uint8_t>(DoorState::Closed));
  CHECK((s.flags & 1) != 0);   // ready
  CHECK((s.flags & 4) != 0);   // at the door
  CHECK((s.flags & 32) != 0);  // strict
  CHECK(s.uptime_ms == r.now);
}

TEST(v1_commands_need_authentication_and_old_opcodes_are_gone) {
  Rig r;
  r.a.connected = true;
  r.app.on_connect(1, r.now);  // connected, not logged in
  CHECK(r.cmd(r.a, {0x01}) == CmdResult::NotAuthenticated);
  CHECK(r.cmd(r.a, {0x08, 2}) == CmdResult::NotAuthenticated);
  CHECK(r.answer(r.a, r.app.take_new_challenge(1)) == AuthResult::NoChallenge);
  r.disconnect(r.a);
  CHECK(r.connect(r.a) == AuthResult::Ok);
  CHECK(r.cmd(r.a, {0x04, 1}) == CmdResult::BadCommand);  // SetMode (rules v2) removed
  CHECK(r.cmd(r.a, {0x08, 3}) == CmdResult::BadCommand);
  CHECK(r.cmd(r.a, {0x09, 5}) == CmdResult::BadCommand);
}

TEST(v1_revoke_forgets_side_and_key) {
  Rig r;
  r.come_home(r.a);
  r.b.pos = Pos::InsideRoom;
  CHECK(r.connect(r.b) == AuthResult::Ok);
  CHECK(r.cmd(r.b, {0x06, static_cast<uint8_t>(r.a.id)}) == CmdResult::Ok);
  CHECK(r.side(r.a) == Side::Unknown);
  r.turn_handle();
  CHECK(r.locked());
  r.disconnect(r.a);
  CHECK(r.connect(r.a) == AuthResult::UnknownKey);
  CHECK(r.count(EventType::Revoked) == 1);
}

TEST(v1_lockout_is_shared_across_links) {
  Rig r;
  r.a.connected = true;
  r.b.connected = true;
  r.app.on_connect(1, r.now);
  r.app.on_connect(2, r.now);
  uint8_t junk[1 + kSigLen] = {0};
  for (int i = 0; i < 5; ++i) {
    r.app.on_auth_response(i % 2 == 0 ? 1 : 2, junk, sizeof(junk), r.now);
    r.answer(i % 2 == 0 ? r.a : r.b, r.app.on_connect(i % 2 == 0 ? 1 : 2, r.now));  // fresh nonces
    if (i < 4) continue;
  }
  r.app.on_connect(1, r.now);
  CHECK(r.app.on_auth_response(1, junk, sizeof(junk), r.now) == AuthResult::LockedOut);
}

TEST(v1_enrollment_only_inside_window) {
  Rig r;
  uint8_t pub[kPubKeyLen] = {9};
  CHECK(r.app.on_enroll(pub, kPubKeyLen, r.now) == -1);
  r.app.open_enrollment(r.now, 30000);
  int id = r.app.on_enroll(pub, kPubKeyLen, r.now);
  CHECK(id == 2);
  CHECK(r.app.on_enroll(pub, kPubKeyLen, r.now) == -1);  // one per window
  CHECK(r.count(EventType::Enrolled) == 1);
}
