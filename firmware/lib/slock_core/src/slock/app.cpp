#include "app.h"

namespace slock {

namespace {
CmdResult from_request(Request r) {
  switch (r) {
    case Request::Accepted: return CmdResult::Ok;
    case Request::Busy: return CmdResult::Busy;
    case Request::NotReady: return CmdResult::NotReady;
    case Request::AlreadyThere: return CmdResult::AlreadyThere;
    case Request::LowBattery: return CmdResult::LowBattery;
  }
  return CmdResult::NotReady;
}
}  // namespace

App::App(LockController& lock, Session& session, PresenceTracker& presence, KeyStore& keys,
         hal::Battery& battery, const AppConfig& cfg)
    : lock_(lock), session_(session), presence_(presence), keys_(keys), battery_(battery), cfg_(cfg) {}

void App::begin(Millis now) {
  presence_.begin(now);
  lock_.begin(now);
}

void App::tick(Millis now) {
  lock_.tick(now);

  if (intent_pending_ && since(now, intent_at_) > cfg_.intent_hold_ms) intent_pending_ = false;

  bool authenticated = connected_ && session_.authenticated(now);
  if (connected_ && was_authenticated_ && !authenticated) {
    fresh_challenge_ = session_.new_challenge();  // login expired on a live link: ask again
  }
  was_authenticated_ = authenticated;

  PolicyInputs in;
  in.authenticated = authenticated;
  in.arrival = presence_.arrival_active(now);
  in.zone = presence_.zone();
  in.intent = intent_pending_;
  in.locked = lock_.state() == LockState::Locked;
  if (decide(cfg_.mode, in) == Decision::Unlock && lock_.request_unlock(now) == Request::Accepted) {
    presence_.consume_arrival();
    intent_pending_ = false;
    ++auto_unlocks_;
  }
}

const uint8_t* App::on_connect(Millis now) {
  connected_ = true;
  was_authenticated_ = false;
  fresh_challenge_ = nullptr;
  presence_.on_connect(now);
  return session_.new_challenge();
}

const uint8_t* App::take_new_challenge() {
  const uint8_t* c = fresh_challenge_;
  fresh_challenge_ = nullptr;
  return c;
}

void App::on_disconnect(Millis now) {
  connected_ = false;
  was_authenticated_ = false;
  fresh_challenge_ = nullptr;
  presence_.on_disconnect(now);
  session_.end();
}

AuthResult App::on_auth_response(const uint8_t* data, size_t len, Millis now) {
  if (!connected_ || len != 1 + kSigLen) {
    last_auth_ = AuthResult::BadSignature;
  } else {
    last_auth_ = session_.respond(data[0], data + 1, now);
  }
  return last_auth_;
}

CmdResult App::on_command(const uint8_t* data, size_t len, Millis now) {
  CmdResult r = CmdResult::BadCommand;
  if (!connected_ || !session_.authenticated(now)) {
    r = CmdResult::NotAuthenticated;
  } else if (len >= 1) {
    switch (static_cast<protocol::Opcode>(data[0])) {
      case protocol::Opcode::Unlock: r = from_request(lock_.request_unlock(now)); break;
      case protocol::Opcode::Lock: r = from_request(lock_.request_lock(now)); break;
      case protocol::Opcode::Calibrate: r = from_request(lock_.request_calibration(now)); break;
      case protocol::Opcode::SetMode:
        if (len >= 2 && data[1] <= 1) {
          cfg_.mode = data[1] == 0 ? UnlockMode::ApproachOnly : UnlockMode::ApproachAndIntent;
          r = CmdResult::Ok;
        }
        break;
      case protocol::Opcode::SetRelock:
        if (len >= 3) {
          uint32_t seconds = static_cast<uint32_t>(data[1] | (data[2] << 8));
          lock_.set_auto_relock_ms(seconds * 1000u);
          r = CmdResult::Ok;
        }
        break;
    }
  }
  last_cmd_ = r;
  return r;
}

void App::on_rssi(int8_t dbm, Millis now) { presence_.on_rssi(dbm, now); }

void App::on_intent(Millis now) {
  intent_pending_ = true;
  intent_at_ = now;
}

void App::open_enrollment(Millis now, uint32_t ms) {
  enroll_open_ = true;
  enroll_at_ = now;
  enroll_ms_ = ms;
}

bool App::enrollment_open(Millis now) const {
  return enroll_open_ && since(now, enroll_at_) < enroll_ms_;
}

int App::on_enroll(const uint8_t* pubkey, size_t len, Millis now) {
  if (len != kPubKeyLen || !enrollment_open(now)) {
    last_enrolled_ = -1;
    return -1;
  }
  int id = keys_.add(pubkey);
  if (id >= 0) enroll_open_ = false;  // one phone per window
  last_enrolled_ = id;
  return id;
}

protocol::StatusPacket App::status(Millis now) const {
  protocol::StatusPacket s;
  s.lock_state = static_cast<uint8_t>(lock_.state());
  s.drive_status = static_cast<uint8_t>(lock_.last_drive_status());
  s.auth = (connected_ && session_.authenticated(now)) ? 1 : 0;
  s.battery_mv = battery_.millivolts();
  float r = presence_.rssi();
  s.rssi_dbm = static_cast<int8_t>(r < -127.f ? -127.f : (r > 0.f ? 0.f : r));
  s.mode = static_cast<uint8_t>(cfg_.mode);
  s.last_cmd_result = static_cast<uint8_t>(last_cmd_);
  s.last_auth_result = static_cast<uint8_t>(last_auth_);
  s.zone = static_cast<uint8_t>(presence_.zone());
  s.flags = static_cast<uint8_t>((presence_.arrival_active(now) ? 1 : 0) | (enrollment_open(now) ? 2 : 0));
  s.last_enrolled_id = static_cast<int8_t>(last_enrolled_);
  return s;
}

}  // namespace slock
