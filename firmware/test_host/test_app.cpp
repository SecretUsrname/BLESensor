// Real-life scenarios against the App: see docs/scenarios.md for the numbered cases.

#include "check.h"
#include "fakes.h"
#include "sim_plant.h"
#include "slock/app.h"

using namespace slock;

namespace {

const uint8_t kLockId[kLockIdLen] = {'T', 'E', 'S', 'T', 'L', 'O', 'C', 'K'};
constexpr int8_t kFar = -92, kNear = -72, kClose = -55;

DriveConfig cfg_for(const SimPlant& p) {
  DriveConfig c;
  c.slot_span_deg = p.slot_span;
  c.nominal_cpd = p.cpd;
  return c;
}

// Home context bytes as the iOS app sends them.
struct Ctx {
  bool has_home;
  bool inside;
  uint8_t minutes;
};
const Ctx kJustHome{true, true, 1};
const Ctx kHomeForHours{true, true, 200};
const Ctx kNotHome{true, false, 255};
const Ctx kNoGeofence{false, false, 255};

struct Phone {
  uint8_t pub[kPubKeyLen];
  int id = -1;
};

struct Rig {
  SimPlant p;
  fakes::FakeBattery bat;
  fakes::FakeDoor door;
  fakes::FakeVerifier ver;
  fakes::CountingRng rng;
  KeyStore keys;
  DriveController drive{p, p, p, p, cfg_for(p)};
  LockController lock;
  App app;
  Phone a, b;
  Millis now = 0;

  explicit Rig(UnlockMode mode = UnlockMode::ApproachAndIntent, bool with_door = false, AppConfig cfg = {})
      : lock(drive, bat, with_door ? &door : nullptr, LockConfig{}),
        app(lock, keys, ver, rng, bat, kLockId, with_mode(cfg, mode)) {
    for (size_t i = 0; i < kPubKeyLen; ++i) {
      a.pub[i] = static_cast<uint8_t>(i * 7 + 3);
      b.pub[i] = static_cast<uint8_t>(i * 13 + 5);
    }
    a.id = keys.add(a.pub);
    b.id = keys.add(b.pub);
    app.begin(now);
    CHECK(lock.request_calibration(now) == Request::Accepted);
    run(5000);
    CHECK(lock.state() == LockState::Locked);
  }

  static AppConfig with_mode(AppConfig c, UnlockMode m) {
    c.mode = m;
    return c;
  }

  void run(uint32_t ms) {
    for (uint32_t i = 0; i < ms; ++i) {
      app.tick(now);
      p.step(1);
      ++now;
    }
  }

  AuthResult answer(uint16_t conn, const Phone& ph, const uint8_t* nonce, const Ctx* ctx = nullptr) {
    if (nonce == nullptr) return AuthResult::NoChallenge;  // nothing to sign (e.g. no free link)
    uint8_t resp[1 + kContextLen + kSigLen];
    resp[0] = static_cast<uint8_t>(ph.id);
    if (ctx == nullptr) {  // v1 login (old app)
      uint8_t msg[kAuthMsgLen];
      Session::build_message(kLockId, nonce, msg);
      fakes::fake_sign(ph.pub, msg, kAuthMsgLen, resp + 1);
      return app.on_auth_response(conn, resp, 1 + kSigLen, now);
    }
    uint8_t c[kContextLen] = {static_cast<uint8_t>((ctx->has_home ? 1 : 0) | (ctx->inside ? 2 : 0)), ctx->minutes};
    uint8_t msg[kAuthMsgLenV2];
    Session::build_message_v2(kLockId, nonce, c, msg);
    resp[1] = c[0];
    resp[2] = c[1];
    fakes::fake_sign(ph.pub, msg, kAuthMsgLenV2, resp + 1 + kContextLen);
    return app.on_auth_response(conn, resp, sizeof(resp), now);
  }

  AuthResult login(uint16_t conn, const Phone& ph, const Ctx* ctx = nullptr) {
    return answer(conn, ph, app.on_connect(conn, now), ctx);
  }

  void rssi(uint16_t conn, int8_t dbm, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 100) {
      app.on_rssi(conn, dbm, now);
      run(100);
    }
  }

  void touch_handle() {
    app.on_intent(now);
    run(2500);
  }

  bool locked() const { return lock.state() == LockState::Locked; }
  bool unlocked() const { return lock.state() == LockState::Unlocked; }

  CmdResult cmd(uint16_t conn, std::initializer_list<uint8_t> bytes) {
    std::vector<uint8_t> v(bytes);
    return app.on_command(conn, v.data(), v.size(), now);
  }
};

}  // namespace

// #2 / #3-ish: a phone that never left doesn't arm the lock.
TEST(app_phone_inside_never_auto_unlocks) {
  Rig r;
  CHECK(r.login(1, r.a) == AuthResult::Ok);  // within a minute of boot: not an arrival
  r.rssi(1, kClose, 2000);
  r.touch_handle();
  CHECK(r.locked());
  CHECK(!r.app.link_info(1, r.now).armed);
  CHECK(r.app.auto_unlocks() == 0);
}

// The basic flow, and one unlock per arrival.
TEST(app_arrival_door_touch_unlocks_once) {
  Rig r;
  r.run(61000);
  CHECK(r.login(1, r.a) == AuthResult::Ok);
  CHECK(r.app.link_info(1, r.now).armed);
  r.rssi(1, kNear, 1000);
  r.touch_handle();
  CHECK(r.locked());  // Near isn't at the door
  r.rssi(1, kClose, 1000);
  CHECK(r.app.link_info(1, r.now).at_door);
  r.touch_handle();
  CHECK(r.unlocked());
  CHECK(r.app.auto_unlocks() == 1);

  CHECK(r.cmd(1, {0x02}) == CmdResult::Ok);
  r.run(3000);
  CHECK(r.locked());
  r.rssi(1, kClose, 500);
  r.touch_handle();
  CHECK(r.locked());  // arrival used up
}

// #1: reconnecting far away (lobby, elevator) no longer wastes the window.
TEST(app_slow_approach_still_works) {
  Rig r;
  r.run(61000);
  CHECK(r.login(1, r.a) == AuthResult::Ok);
  r.rssi(1, kFar, 100000);  // 100 s to get from the car to the door
  r.rssi(1, kClose, 1000);
  r.touch_handle();
  CHECK(r.unlocked());
}

TEST(app_arrival_expires_after_five_minutes) {
  Rig r;
  r.run(61000);
  CHECK(r.login(1, r.a) == AuthResult::Ok);
  r.rssi(1, kFar, 301000);
  CHECK(!r.app.link_info(1, r.now).armed);
  r.rssi(1, kClose, 1000);
  r.touch_handle();
  CHECK(r.locked());
}

// #5: walking past your door doesn't leave it unlockable.
TEST(app_walking_past_the_door) {
  Rig r;
  r.run(61000);
  CHECK(r.login(1, r.a) == AuthResult::Ok);
  r.rssi(1, kClose, 2000);
  r.rssi(1, kNear, 4000);  // on to the trash chute
  CHECK(!r.app.link_info(1, r.now).at_door);
  r.touch_handle();  // someone else tries the handle
  CHECK(r.locked());
  r.rssi(1, kClose, 1000);  // back at the door, still armed
  r.touch_handle();
  CHECK(r.unlocked());
}

TEST(app_door_window_times_out) {
  Rig r;
  r.run(61000);
  CHECK(r.login(1, r.a) == AuthResult::Ok);
  r.rssi(1, kClose, 31000);  // standing at the door for over 30 s
  r.touch_handle();
  CHECK(r.locked());
}

// A login renewed on the same connection is never a new arrival.
TEST(app_renewed_login_is_not_an_arrival) {
  Rig r;
  CHECK(r.login(1, r.a) == AuthResult::Ok);
  r.rssi(1, kClose, 125000);  // past the 2-minute login
  const uint8_t* fresh = r.app.take_new_challenge(1);
  CHECK(fresh != nullptr);
  CHECK(r.app.take_new_challenge(1) == nullptr);  // handed out once
  CHECK(r.answer(1, r.a, fresh) == AuthResult::Ok);
  CHECK(!r.app.link_info(1, r.now).armed);
  CHECK(r.cmd(1, {0x01}) == CmdResult::Ok);  // commands work again
}

// #8: two phones are judged separately.
TEST(app_two_phones) {
  Rig r;
  CHECK(r.login(1, r.a) == AuthResult::Ok);  // A is inside
  r.rssi(1, kClose, 61000);
  CHECK(r.login(2, r.b) == AuthResult::Ok);  // B arrives
  CHECK(!r.app.link_info(1, r.now).armed);
  CHECK(r.app.link_info(2, r.now).armed);
  for (int i = 0; i < 10; ++i) {
    r.app.on_rssi(1, kClose, r.now);
    r.app.on_rssi(2, kClose, r.now);
    r.run(100);
  }
  r.touch_handle();
  CHECK(r.unlocked());
  CHECK(r.app.link_info(2, r.now).key_id == r.b.id);
}

TEST(app_link_slots_are_limited) {
  Rig r;
  CHECK(r.app.on_connect(1, r.now) != nullptr);
  CHECK(r.app.on_connect(2, r.now) != nullptr);
  CHECK(r.app.on_connect(3, r.now) != nullptr);
  CHECK(r.app.on_connect(4, r.now) == nullptr);
  r.app.on_disconnect(2, r.now);
  CHECK(r.app.on_connect(4, r.now) != nullptr);
}

// #7: a relayed phone is somewhere else, and it says so.
TEST(app_relay_blocked_by_home_context) {
  Rig r;
  r.run(61000);
  CHECK(r.login(1, r.a, &kNotHome) == AuthResult::Ok);
  r.rssi(1, kClose, 1000);  // the relay makes it look close
  CHECK(!r.app.link_info(1, r.now).context_ok);
  r.touch_handle();         // the attacker touches the handle
  CHECK(r.locked());
}

// #3: phone was home all along, just out of range for a while.
TEST(app_phone_home_for_hours_blocked) {
  Rig r;
  r.run(61000);
  CHECK(r.login(1, r.a, &kHomeForHours) == AuthResult::Ok);
  r.rssi(1, kClose, 1000);
  r.touch_handle();
  CHECK(r.locked());

  r.app.on_disconnect(1, r.now);
  r.run(61000);
  CHECK(r.login(1, r.a, &kJustHome) == AuthResult::Ok);  // a real return
  r.rssi(1, kClose, 1000);
  r.touch_handle();
  CHECK(r.unlocked());
}

TEST(app_require_home_context) {
  AppConfig cfg;
  cfg.require_home_context = true;
  Rig r(UnlockMode::ApproachAndIntent, false, cfg);
  r.run(61000);
  CHECK(r.login(1, r.a) == AuthResult::Ok);  // old app, no context
  r.rssi(1, kClose, 1000);
  r.touch_handle();
  CHECK(r.locked());
  r.app.on_disconnect(1, r.now);
  r.run(61000);
  CHECK(r.login(1, r.a, &kNoGeofence) == AuthResult::Ok);  // new app, no geofence set up
  r.rssi(1, kClose, 1000);
  r.touch_handle();
  CHECK(r.locked());
}

// Updated context without reconnecting (e.g. the geofence entry fired late).
TEST(app_rechallenge_updates_context) {
  Rig r;
  r.run(61000);
  CHECK(r.login(1, r.a, &kNotHome) == AuthResult::Ok);
  CHECK(r.cmd(1, {0x07}) == CmdResult::Ok);
  const uint8_t* fresh = r.app.take_new_challenge(1);
  CHECK(fresh != nullptr);
  CHECK(r.answer(1, r.a, fresh, &kJustHome) == AuthResult::Ok);
  CHECK(r.app.link_info(1, r.now).context_ok);
  CHECK(r.app.link_info(1, r.now).armed);  // still the same arrival
  r.rssi(1, kClose, 1000);
  r.touch_handle();
  CHECK(r.unlocked());
}

// #9: a lost phone's key can be revoked from another phone.
TEST(app_revoke_key) {
  Rig r;
  CHECK(r.login(1, r.a) == AuthResult::Ok);
  CHECK(r.login(2, r.b) == AuthResult::Ok);
  CHECK(r.cmd(2, {0x06, static_cast<uint8_t>(r.a.id)}) == CmdResult::Ok);
  CHECK(!r.app.link_info(1, r.now).authenticated);
  CHECK(r.cmd(1, {0x01}) == CmdResult::NotAuthenticated);
  r.app.on_disconnect(1, r.now);
  CHECK(r.login(1, r.a) == AuthResult::UnknownKey);
  CHECK(r.cmd(2, {0x06, 7}) == CmdResult::BadCommand);  // no such key
}

// #11: approach-only only with a door sensor and auto-relock.
TEST(app_approach_only_needs_door_sensor_and_relock) {
  Rig no_sensor;
  CHECK(no_sensor.login(1, no_sensor.a) == AuthResult::Ok);
  CHECK(no_sensor.cmd(1, {0x05, 30, 0}) == CmdResult::Ok);
  CHECK(no_sensor.cmd(1, {0x04, 0}) == CmdResult::NotAllowed);

  Rig r(UnlockMode::ApproachAndIntent, /*with_door=*/true);
  CHECK(r.login(1, r.a) == AuthResult::Ok);
  CHECK(r.cmd(1, {0x04, 0}) == CmdResult::NotAllowed);  // relock off
  CHECK(r.cmd(1, {0x05, 30, 0}) == CmdResult::Ok);
  CHECK(r.cmd(1, {0x04, 0}) == CmdResult::Ok);
  CHECK(r.app.mode() == UnlockMode::ApproachOnly);
  CHECK(r.cmd(1, {0x05, 0, 0}) == CmdResult::NotAllowed);  // can't turn relock off now
}

TEST(app_approach_only_unlocks_at_door_without_touch) {
  Rig r(UnlockMode::ApproachAndIntent, true);
  r.lock.set_auto_relock_ms(30000);
  CHECK(r.app.set_mode(UnlockMode::ApproachOnly));
  r.run(61000);
  CHECK(r.login(1, r.a) == AuthResult::Ok);
  r.rssi(1, kNear, 1000);
  CHECK(r.locked());
  r.rssi(1, kClose, 3000);
  CHECK(r.unlocked());
}

TEST(app_commands_need_authentication) {
  Rig r;
  CHECK(r.cmd(9, {0x01}) == CmdResult::NotAuthenticated);  // unknown connection
  r.app.on_connect(1, r.now);
  CHECK(r.cmd(1, {0x01}) == CmdResult::NotAuthenticated);  // connected, not logged in
  r.app.on_disconnect(1, r.now);
  CHECK(r.login(1, r.a) == AuthResult::Ok);
  CHECK(r.cmd(1, {0x01}) == CmdResult::Ok);
  r.run(3000);
  CHECK(r.unlocked());
  CHECK(r.cmd(1, {0x99}) == CmdResult::BadCommand);
  CHECK(r.cmd(1, {0x05, 30, 0}) == CmdResult::Ok);
  CHECK(r.lock.auto_relock_ms() == 30000);
  CHECK(r.app.status(1, r.now).last_cmd_result == static_cast<uint8_t>(CmdResult::Ok));
  CHECK(r.app.status(1, r.now).auth == 1);
}

TEST(app_lockout_is_shared_across_links) {
  Rig r;
  uint8_t bad[1 + kSigLen] = {};
  for (int i = 0; i < 5; ++i) {
    r.app.on_connect(1, r.now);
    CHECK(r.app.on_auth_response(1, bad, sizeof(bad), r.now) == AuthResult::BadSignature);
  }
  CHECK(r.login(2, r.a) == AuthResult::LockedOut);  // a second connection gets no fresh guesses
}

TEST(app_enrollment_only_inside_window) {
  Rig r;
  uint8_t pub3[kPubKeyLen] = {1};
  CHECK(r.app.on_enroll(pub3, kPubKeyLen, r.now) == -1);
  CHECK(r.app.status(0, r.now).last_enrolled_id == -1);
  r.app.open_enrollment(r.now, 60000);
  CHECK(r.app.enrollment_open(r.now));
  CHECK(r.app.on_enroll(pub3, kPubKeyLen, r.now) == 2);
  CHECK(r.app.status(0, r.now).last_enrolled_id == 2);  // how the phone learns its key id
  CHECK(!r.app.enrollment_open(r.now));                  // one phone per window
  r.app.open_enrollment(r.now, 60000);
  r.run(61000);
  CHECK(r.app.on_enroll(pub3, kPubKeyLen, r.now) == -1);
}
