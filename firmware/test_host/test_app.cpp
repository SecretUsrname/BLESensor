#include "check.h"
#include "fakes.h"
#include "sim_plant.h"
#include "slock/app.h"

using namespace slock;

namespace {

const uint8_t kLockId[kLockIdLen] = {'T', 'E', 'S', 'T', 'L', 'O', 'C', 'K'};

DriveConfig cfg_for(const SimPlant& p) {
  DriveConfig c;
  c.slot_span_deg = p.slot_span;
  c.nominal_cpd = p.cpd;
  return c;
}

struct AppRig {
  SimPlant p;
  fakes::FakeBattery bat;
  fakes::FakeVerifier ver;
  fakes::CountingRng rng;
  KeyStore keys;
  DriveController drive{p, p, p, p, cfg_for(p)};
  LockController lock{drive, bat, nullptr, LockConfig{}};
  Session session{keys, ver, rng, kLockId, SessionConfig{}};
  PresenceTracker presence{PresenceConfig{}};
  App app;
  uint8_t pub[kPubKeyLen];
  int id = -1;
  Millis now = 0;

  explicit AppRig(UnlockMode mode) : app(lock, session, presence, keys, bat, AppConfig{mode, 1500}) {
    for (size_t i = 0; i < kPubKeyLen; ++i) pub[i] = static_cast<uint8_t>(i * 7 + 3);
    id = keys.add(pub);
    app.begin(now);
    CHECK(lock.request_calibration(now) == Request::Accepted);
    run(5000);
    CHECK(lock.state() == LockState::Locked);
  }

  void run(uint32_t ms) {
    for (uint32_t i = 0; i < ms; ++i) {
      app.tick(now);
      p.step(1);
      ++now;
    }
  }

  AuthResult connect_and_auth() {
    const uint8_t* nonce = app.on_connect(now);
    uint8_t msg[kAuthMsgLen];
    Session::build_message(kLockId, nonce, msg);
    uint8_t resp[1 + kSigLen];
    resp[0] = static_cast<uint8_t>(id);
    fakes::fake_sign(pub, msg, kAuthMsgLen, resp + 1);
    return app.on_auth_response(resp, sizeof(resp), now);
  }

  void rssi_for(int8_t dbm, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 100) {
      app.on_rssi(dbm, now);
      run(100);
    }
  }
};

}  // namespace

TEST(app_phone_inside_never_auto_unlocks) {
  AppRig r(UnlockMode::ApproachAndIntent);
  CHECK(r.connect_and_auth() == AuthResult::Ok);  // connected within a minute of boot: not an arrival
  r.rssi_for(-50, 2000);
  r.app.on_intent(r.now);  // someone inside rattles the handle
  r.run(3000);
  CHECK(r.lock.state() == LockState::Locked);
  CHECK(r.app.auto_unlocks() == 0);
}

TEST(app_arrival_plus_handle_touch_unlocks_once) {
  AppRig r(UnlockMode::ApproachAndIntent);
  r.run(61000);  // phone away for over a minute
  CHECK(r.connect_and_auth() == AuthResult::Ok);
  r.rssi_for(-60, 1000);
  CHECK(r.lock.state() == LockState::Locked);  // close, but nobody touched the handle yet
  r.app.on_intent(r.now);
  r.run(3000);
  CHECK(r.lock.state() == LockState::Unlocked);
  CHECK(r.app.auto_unlocks() == 1);

  uint8_t lock_cmd[] = {static_cast<uint8_t>(protocol::Opcode::Lock)};
  CHECK(r.app.on_command(lock_cmd, 1, r.now) == CmdResult::Ok);
  r.run(3000);
  CHECK(r.lock.state() == LockState::Locked);
  r.app.on_intent(r.now);  // arrival already used up
  r.run(3000);
  CHECK(r.lock.state() == LockState::Locked);
  CHECK(r.app.auto_unlocks() == 1);
}

TEST(app_handle_touch_with_phone_far_does_nothing) {
  AppRig r(UnlockMode::ApproachAndIntent);
  r.run(61000);
  CHECK(r.connect_and_auth() == AuthResult::Ok);
  r.rssi_for(-95, 1000);
  r.app.on_intent(r.now);
  r.run(3000);
  CHECK(r.lock.state() == LockState::Locked);
}

TEST(app_approach_only_mode_unlocks_when_close) {
  AppRig r(UnlockMode::ApproachOnly);
  r.run(61000);
  CHECK(r.connect_and_auth() == AuthResult::Ok);
  r.rssi_for(-70, 1000);
  CHECK(r.lock.state() == LockState::Locked);  // Near isn't enough in this mode
  r.rssi_for(-60, 1000);
  r.run(3000);
  CHECK(r.lock.state() == LockState::Unlocked);
}

TEST(app_commands_need_authentication) {
  AppRig r(UnlockMode::ApproachAndIntent);
  uint8_t unlock[] = {static_cast<uint8_t>(protocol::Opcode::Unlock)};
  CHECK(r.app.on_command(unlock, 1, r.now) == CmdResult::NotAuthenticated);
  r.app.on_connect(r.now);
  CHECK(r.app.on_command(unlock, 1, r.now) == CmdResult::NotAuthenticated);  // connected, not authenticated
  r.app.on_disconnect(r.now);

  CHECK(r.connect_and_auth() == AuthResult::Ok);
  CHECK(r.app.on_command(unlock, 1, r.now) == CmdResult::Ok);
  r.run(3000);
  CHECK(r.lock.state() == LockState::Unlocked);

  uint8_t mode_a[] = {static_cast<uint8_t>(protocol::Opcode::SetMode), 0};
  CHECK(r.app.on_command(mode_a, 2, r.now) == CmdResult::Ok);
  CHECK(r.app.mode() == UnlockMode::ApproachOnly);
  uint8_t bogus[] = {0x99};
  CHECK(r.app.on_command(bogus, 1, r.now) == CmdResult::BadCommand);
  uint8_t relock[] = {static_cast<uint8_t>(protocol::Opcode::SetRelock), 30, 0};
  CHECK(r.app.on_command(relock, 3, r.now) == CmdResult::Ok);
  CHECK(r.lock.auto_relock_ms() == 30000);

  CHECK(r.app.status(r.now).last_cmd_result == static_cast<uint8_t>(CmdResult::Ok));
  CHECK(r.app.status(r.now).auth == 1);
}

TEST(app_enrollment_only_inside_window) {
  AppRig r(UnlockMode::ApproachAndIntent);
  uint8_t pub2[kPubKeyLen] = {1};
  CHECK(r.app.on_enroll(pub2, kPubKeyLen, r.now) == -1);
  r.app.open_enrollment(r.now, 60000);
  CHECK(r.app.enrollment_open(r.now));
  CHECK(r.app.on_enroll(pub2, kPubKeyLen, r.now) == 1);
  CHECK(r.app.status(r.now).last_enrolled_id == 1);  // how the phone learns its key id
  CHECK(!r.app.enrollment_open(r.now));  // one phone per window
  r.app.open_enrollment(r.now, 60000);
  r.run(61000);
  CHECK(r.app.on_enroll(pub2, kPubKeyLen, r.now) == -1);
  CHECK(r.app.status(r.now).last_enrolled_id == -1);
}
