#include "app.h"

#include <cstring>
#include <type_traits>

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

struct Settings {
  uint8_t version = 1;
  uint8_t strict = 1;
  uint16_t relock_s = 3;
  DoorCalibration cal;
};

static_assert(std::is_trivially_copyable<KeyStore>::value, "KeyStore is saved as a blob");
static_assert(std::is_trivially_copyable<Settings>::value, "Settings is saved as a blob");
static_assert(std::is_trivially_copyable<Calibration>::value, "Calibration is saved as a blob");

uint8_t clamp_u8(int32_t v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

}  // namespace

App::App(LockController& lock, KeyStore& keys, Verifier& verifier, hal::Random& rng, hal::Battery& battery,
         const uint8_t* lock_id, const AppConfig& cfg, hal::Storage* storage)
    : lock_(lock), keys_(keys), verifier_(verifier), rng_(rng), battery_(battery), storage_(storage), cfg_(cfg),
      door_(cfg.door), sides_(cfg.side) {
  std::memcpy(lock_id_, lock_id, kLockIdLen);
  for (Link& l : links_) {
    l.session.bind(&keys_, &verifier_, &rng_, lock_id_, cfg_.session, &guard_);
    l.zones.bind(&cfg_.presence);
  }
  sides_.set_calibration(&door_cal_);
}

void App::begin(Millis now) {
  if (storage_ != nullptr) {
    KeyStore loaded;
    if (storage_->load(kStoreKeys, &loaded, sizeof(loaded))) keys_ = loaded;
    else if (keys_.count() > 0) save_keys();  // first boot with keys provisioned in code
    Settings s;
    if (storage_->load(kStoreSettings, &s, sizeof(s)) && s.version == 1) {
      cfg_.strict = s.strict != 0;
      cfg_.relock_after_close_ms = static_cast<uint32_t>(s.relock_s) * 1000u;
      door_cal_ = s.cal;
    }
    Calibration c;
    if (storage_->load(kStoreDriveCal, &c, sizeof(c)) && c.valid) {
      lock_.drive().set_calibration(c);
      cal_saved_ = true;
    }
  }
  sides_.begin(now);  // reboot: every phone Unknown
  log_.add(EventType::Boot, now);
  lock_.begin(now);
  last_state_ = lock_.state();
}

void App::save_keys() {
  if (storage_ != nullptr) storage_->save(kStoreKeys, &keys_, sizeof(keys_));
}

void App::save_settings() {
  if (storage_ == nullptr) return;
  Settings s;
  s.strict = cfg_.strict ? 1 : 0;
  uint32_t secs = cfg_.relock_after_close_ms / 1000u;
  s.relock_s = static_cast<uint16_t>(secs > 0xFFFF ? 0xFFFF : secs);
  s.cal = door_cal_;
  storage_->save(kStoreSettings, &s, sizeof(s));
}

App::Link* App::find(uint16_t conn) {
  for (Link& l : links_) {
    if (l.used && l.conn == conn) return &l;
  }
  return nullptr;
}

const App::Link* App::find(uint16_t conn) const {
  for (const Link& l : links_) {
    if (l.used && l.conn == conn) return &l;
  }
  return nullptr;
}

bool App::key_on_other_link(const Link& l, int key) const {
  for (const Link& o : links_) {
    if (&o != &l && o.used && o.key == key) return true;
  }
  return false;
}

void App::reset_link(Link& l) {
  l.used = false;
  l.session.end();
  l.zones.clear();
  l.key = -1;
  l.was_auth = false;
  l.logged_in_once = false;
  l.fresh = nullptr;
  l.refreshed = false;
  l.was_at_door = false;
  l.log_cursor = log_.next_seq();
  l.last_cmd = CmdResult::Ok;
  l.last_auth = AuthResult::NoChallenge;
}

const uint8_t* App::on_connect(uint16_t conn, Millis now) {
  (void)now;
  Link* l = find(conn);
  if (l == nullptr) {
    for (Link& candidate : links_) {
      if (!candidate.used) {
        l = &candidate;
        break;
      }
    }
  }
  if (l == nullptr) return nullptr;  // all slots busy
  reset_link(*l);
  l->used = true;
  l->conn = conn;
  l->zones.reset();
  return l->session.new_challenge();
}

void App::on_disconnect(uint16_t conn, Millis now) {
  Link* l = find(conn);
  if (l == nullptr) return;
  int key = l->key;
  if (key >= 0 && !key_on_other_link(*l, key)) sides_.on_disconnect(static_cast<uint8_t>(key), now);
  reset_link(*l);
}

void App::on_authenticated(Link& l, Millis now) {
  bool first = !l.logged_in_once;
  l.logged_in_once = true;
  int key = l.session.key_id();
  bool elsewhere = key_on_other_link(l, key);
  l.key = key;
  l.ctx_at = now;
  sides_.on_login(static_cast<uint8_t>(key), l.session.context(), first && !elsewhere, now);
}

AuthResult App::on_auth_response(uint16_t conn, const uint8_t* data, size_t len, Millis now) {
  Link* l = find(conn);
  if (l == nullptr) return AuthResult::NoChallenge;
  AuthResult r;
  if (len == 1 + kSigLen) {
    r = l->session.respond(data[0], data + 1, now);
  } else if (len == 1 + kContextLen + kSigLen) {
    r = l->session.respond_with_context(data[0], data + 1, data + 1 + kContextLen, now);
  } else if (len == 1 + kContextLenV3 + kSigLen) {
    r = l->session.respond_v3(data[0], data + 1, data + 1 + kContextLenV3, now);
  } else {
    r = AuthResult::BadSignature;
  }
  l->last_auth = r;
  if (r == AuthResult::Ok) {
    on_authenticated(*l, now);
  } else if (r == AuthResult::BadSignature || r == AuthResult::UnknownKey) {
    log_.add(EventType::LoginFailed, now, len >= 1 ? data[0] : -1, static_cast<uint8_t>(r));
  }
  return r;
}

void App::refresh(Link& l, Millis now) {
  if (!l.session.authenticated(now)) return;
  if (l.refreshed && since(now, l.refresh_at) < cfg_.refresh_min_ms) return;
  l.refreshed = true;
  l.refresh_at = now;
  l.fresh = l.session.new_challenge(/*keep_auth=*/true);
}

bool App::door_known_open() const {
  return door_.state() == DoorState::Open && door_.reference_known();
}

CmdResult App::on_command(uint16_t conn, const uint8_t* data, size_t len, Millis now) {
  Link* l = find(conn);
  if (l == nullptr) return CmdResult::NotAuthenticated;
  CmdResult r = CmdResult::BadCommand;
  const uint8_t key = static_cast<uint8_t>(l->key < 0 ? 0 : l->key);
  if (!l->session.authenticated(now)) {
    r = CmdResult::NotAuthenticated;
  } else if (len >= 1) {
    switch (static_cast<protocol::Opcode>(data[0])) {
      case protocol::Opcode::Unlock:
        if (len >= 2 && (data[1] == 1 || data[1] == 2)) {  // the user said which side they're on
          sides_.set_side(key, static_cast<Side>(data[1]), SideReason::User, now);
        }
        r = from_request(lock_.request_unlock(now));
        if (r == CmdResult::Ok) log_.add(EventType::AppUnlock, now, l->key);
        break;
      case protocol::Opcode::Lock:
        if (door_known_open()) {
          r = CmdResult::DoorOpen;  // never throw the bolt into an open door
        } else {
          r = from_request(lock_.request_lock(now));
          if (r == CmdResult::Ok) log_.add(EventType::AppLock, now, l->key);
        }
        break;
      case protocol::Opcode::Calibrate: r = from_request(lock_.request_calibration(now)); break;
      case protocol::Opcode::SetRelock:
        if (len >= 3) {
          cfg_.relock_after_close_ms = static_cast<uint32_t>(data[1] | (data[2] << 8)) * 1000u;
          save_settings();
          r = CmdResult::Ok;
        }
        break;
      case protocol::Opcode::RevokeKey:
        if (len >= 2 && keys_.remove(data[1])) {
          for (Link& other : links_) {
            if (other.used && other.key == data[1]) {
              other.session.end();
              other.key = -1;
              other.was_auth = false;
            }
          }
          sides_.forget(data[1], now);
          log_.add(EventType::Revoked, now, data[1], 0, key);
          save_keys();
          r = CmdResult::Ok;
        }
        break;
      case protocol::Opcode::Rechallenge:
        l->fresh = l->session.new_challenge();
        l->was_auth = false;  // the expected gap until the answer isn't an expiry
        r = CmdResult::Ok;
        break;
      case protocol::Opcode::SetSide:
        if (len >= 2 && (data[1] == 1 || data[1] == 2)) {
          sides_.set_side(key, static_cast<Side>(data[1]), SideReason::User, now);
          r = CmdResult::Ok;
        }
        break;
      case protocol::Opcode::DoorCal:
        if (len >= 2 && data[1] <= 4) {
          uint8_t step = data[1];
          if (step == 0) {
            door_cal_ = DoorCalibration{};
            save_settings();
            r = CmdResult::Ok;
          } else if (step == 4) {
            sides_.measure_block(key);  // finishes at the next close (see tick)
            r = CmdResult::Ok;
          } else if (l->zones.rssi() <= -126.f) {
            r = CmdResult::NotReady;  // no signal reading yet
          } else {
            int8_t v = static_cast<int8_t>(l->zones.rssi() - 0.5f);
            if (step == 1) door_cal_.outside_at_door = v;
            if (step == 2) door_cal_.inside_at_lock = v;
            if (step == 3) door_cal_.two_m = v;
            door_cal_.steps_done = static_cast<uint8_t>(door_cal_.steps_done | (1u << (step - 1)));
            save_settings();
            log_.add(EventType::DoorCalibrated, now, l->key, step, static_cast<uint8_t>(v));
            r = CmdResult::Ok;
          }
        }
        break;
      case protocol::Opcode::SetStrict:
        if (len >= 2 && data[1] <= 1) {
          cfg_.strict = data[1] == 1;
          save_settings();
          r = CmdResult::Ok;
        }
        break;
      case protocol::Opcode::ReadLog:
        if (len >= 5) {
          l->log_cursor = static_cast<uint32_t>(data[1]) | (static_cast<uint32_t>(data[2]) << 8) |
                          (static_cast<uint32_t>(data[3]) << 16) | (static_cast<uint32_t>(data[4]) << 24);
          r = CmdResult::Ok;
        }
        break;
      default:
        break;
    }
  }
  if (find(conn) != nullptr) l->last_cmd = r;
  return r;
}

void App::on_rssi(uint16_t conn, int8_t dbm, Millis now) {
  Link* l = find(conn);
  if (l == nullptr) return;
  l->zones.on_rssi(dbm, door_cal_.at_door_dbm());
  if (l->key >= 0) sides_.on_rssi(static_cast<uint8_t>(l->key), dbm, l->zones.at_door(), now);
}

const uint8_t* App::take_new_challenge(uint16_t conn) {
  Link* l = find(conn);
  if (l == nullptr) return nullptr;
  const uint8_t* c = l->fresh;
  l->fresh = nullptr;
  return c;
}

size_t App::log_page(uint16_t conn, uint8_t* out, size_t cap) {
  Link* l = find(conn);
  if (l == nullptr || cap < 1) return 0;
  size_t room = (cap - 1) / sizeof(Event);
  uint16_t max = static_cast<uint16_t>(room < protocol::kLogPageEvents ? room : protocol::kLogPageEvents);
  Event ev[protocol::kLogPageEvents];
  uint16_t n = log_.read(l->log_cursor, ev, max);
  out[0] = static_cast<uint8_t>(n);
  std::memcpy(out + 1, ev, n * sizeof(Event));
  if (n > 0) l->log_cursor = ev[n - 1].seq + 1;
  return 1 + n * sizeof(Event);
}

void App::on_intent(Millis now) {
  intent_pending_ = true;
  intent_at_ = now;
  // The decision needs current steps and home state: ask phones at the door to re-sign now. They
  // answer well within intent_hold_ms.
  for (Link& l : links_) {
    if (l.used && l.zones.at_door()) refresh(l, now);
  }
}

void App::door_closed(Millis at, bool trusted, Millis now) {
  sides_.on_door_closed(at, now);
  if (trusted && cfg_.relock_after_close_ms > 0 && lock_.state() == LockState::Unlocked) {
    relock_closed_ = true;
    relock_closed_at_ = now;
  }
}

void App::on_imu(float rate_dps, float accel_dev_mg, Millis now) {
  const bool was_open = door_.state() == DoorState::Open;
  DoorEvent ev = door_.on_sample(rate_dps, accel_dev_mg, lock_.state() == LockState::Locked, now);
  switch (ev.type) {
    case DoorEventType::Opened:
      sides_.on_door_opened(now);
      relock_unopened_ = false;
      relock_closed_ = false;
      for (Link& l : links_) {
        if (l.used) refresh(l, now);
      }
      break;
    case DoorEventType::Closed:
      door_closed(ev.at, ev.trusted, now);
      break;
    case DoorEventType::Corrected:
      if (was_open) door_closed(now, false, now);
      break;
    case DoorEventType::Tamper:
      log_.add(EventType::Tamper, now);
      break;
    case DoorEventType::None:
      break;
  }
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
  if (id >= 0) {
    enroll_open_ = false;  // one phone per window
    sides_.forget(static_cast<uint8_t>(id), now);
    log_.add(EventType::Enrolled, now, id);
    save_keys();
  }
  last_enrolled_ = id;
  return id;
}

bool App::home_ok(const PhoneContext& c) const {
  if (!c.provided || !c.has_home) return !cfg_.strict;
  return c.inside_home;
}

bool App::carried(const Link& l, Millis now) const {
  const PhoneContext& c = l.session.context();
  if (!c.provided || !c.has_steps || c.seconds_since_step == 0xFFFF) return !cfg_.strict;
  uint32_t secs = c.seconds_since_step + since(now, l.ctx_at) / 1000u;
  return secs <= cfg_.carried_s;
}

PolicyInputs App::inputs(const Link& l, Millis now) const {
  PolicyInputs in;
  if (l.key < 0) return in;
  const uint8_t key = static_cast<uint8_t>(l.key);
  in.authenticated = l.session.authenticated(now);
  in.outside = sides_.side(key) == Side::Outside && !sides_.pending(key);
  in.at_door = l.zones.at_door();
  in.home_ok = home_ok(l.session.context());
  in.carried = carried(l, now);
  in.intent = intent_pending_;
  in.locked = lock_.state() == LockState::Locked;
  return in;
}

void App::watch_bolt(Millis now) {
  LockState s = lock_.state();
  if (s == last_state_) return;
  switch (s) {
    case LockState::Unlocking:
    case LockState::Locking:
    case LockState::Calibrating:
    case LockState::Homing:
      own_move_ = true;
      break;
    case LockState::Locked:
    case LockState::Unlocked:
      if (last_end_ != LockState::Uncalibrated && s != last_end_ && !own_move_) {
        log_.add(EventType::BoltTurned, now, -1, s == LockState::Unlocked ? 1 : 0);
        on_hand_turn(now);
      }
      if (s == LockState::Locked && door_known_open()) log_.add(EventType::FalseLock, now);
      last_end_ = s;
      own_move_ = false;
      break;
    case LockState::Jammed:
      log_.add(EventType::Jam, now);
      own_move_ = false;
      break;
    default:
      break;
  }
  if (last_state_ == LockState::Calibrating && lock_.drive().calibration().valid && storage_ != nullptr) {
    Calibration c = lock_.drive().calibration();
    storage_->save(kStoreDriveCal, &c, sizeof(c));
    cal_saved_ = true;
  }
  last_state_ = s;
}

void App::tick(Millis now) {
  lock_.tick(now);
  watch_bolt(now);
  sides_.tick(now);
  if (intent_pending_ && since(now, intent_at_) > cfg_.intent_hold_ms) intent_pending_ = false;

  uint8_t db = 0;
  if (sides_.take_block_result(db)) {
    door_cal_.block_db = db;
    door_cal_.steps_done = static_cast<uint8_t>(door_cal_.steps_done | 8u);
    save_settings();
    log_.add(EventType::DoorCalibrated, now, -1, 4, db);
  }
  for (SideChange c = sides_.take_change(); c.changed; c = sides_.take_change()) {
    log_.add(EventType::SideChanged, now, c.key, static_cast<uint8_t>(c.side), static_cast<uint8_t>(c.reason));
  }

  for (Link& l : links_) {
    if (!l.used) continue;
    bool auth = l.session.authenticated(now);
    if (l.was_auth && !auth && l.key >= 0 && keys_.get(static_cast<uint8_t>(l.key)) != nullptr) {
      l.fresh = l.session.new_challenge();  // login expired on a live link: ask again
    }
    l.was_auth = auth;
    bool at_door = l.zones.at_door();
    if (at_door && !l.was_at_door) refresh(l, now);  // reached the door: get fresh context
    l.was_at_door = at_door;
  }

  // Relocking
  if (relock_unopened_ && since(now, relock_unopened_at_) >= cfg_.relock_unopened_ms) {
    relock_unopened_ = false;
    if (lock_.state() == LockState::Unlocked && door_.state() != DoorState::Open &&
        lock_.request_lock(now) == Request::Accepted) {
      log_.add(EventType::AutoRelock, now, -1, 1);
    }
  }
  if (relock_closed_ && since(now, relock_closed_at_) >= cfg_.relock_after_close_ms) {
    relock_closed_ = false;
    if (lock_.state() == LockState::Unlocked && door_.state() == DoorState::Closed && door_.trusted() &&
        lock_.request_lock(now) == Request::Accepted) {
      log_.add(EventType::AutoRelock, now, -1, 0);
    }
  }

  // Auto-unlock
  for (Link& l : links_) {
    if (!l.used) continue;
    if (decide(inputs(l, now)) == Decision::Unlock && lock_.request_unlock(now) == Request::Accepted) {
      intent_pending_ = false;
      ++auto_unlocks_;
      log_.add(EventType::AutoUnlock, now, l.key);
      relock_unopened_ = true;
      relock_unopened_at_ = now;
      break;
    }
  }
}

LinkInfo App::link_info(uint16_t conn, Millis now) const {
  LinkInfo info;
  const Link* l = find(conn);
  if (l == nullptr) return info;
  info.used = true;
  info.conn = conn;
  info.key_id = l->key;
  info.authenticated = l->session.authenticated(now);
  info.zone = l->zones.zone();
  info.rssi = l->zones.rssi();
  info.fast_rssi = l->zones.fast_rssi();
  info.at_door = l->zones.at_door();
  info.context = l->session.context();
  info.home_ok = home_ok(info.context);
  info.carried = carried(*l, now);
  if (l->key >= 0) {
    uint8_t k = static_cast<uint8_t>(l->key);
    info.side = sides_.side(k);
    info.side_reason = sides_.reason(k);
    info.side_pending = sides_.pending(k);
  }
  info.ready = ready(inputs(*l, now));
  return info;
}

protocol::StatusPacket App::status(uint16_t conn, Millis now) const {
  protocol::StatusPacket s;
  LinkInfo li = link_info(conn, now);
  const Link* l = find(conn);
  s.lock_state = static_cast<uint8_t>(lock_.state());
  s.drive_status = static_cast<uint8_t>(lock_.last_drive_status());
  s.auth = li.authenticated ? 1 : 0;
  s.battery_mv = battery_.millivolts();
  float r = li.rssi;
  s.rssi_dbm = static_cast<int8_t>(r < -127.f ? -127.f : (r > 0.f ? 0.f : r));
  s.last_cmd_result = static_cast<uint8_t>(l != nullptr ? l->last_cmd : CmdResult::Ok);
  s.last_auth_result = static_cast<uint8_t>(l != nullptr ? l->last_auth : AuthResult::NoChallenge);
  s.zone = static_cast<uint8_t>(li.zone);
  s.flags = static_cast<uint8_t>((li.ready ? 1 : 0) | (enrollment_open(now) ? 2 : 0) | (li.at_door ? 4 : 0) |
                                 (li.home_ok ? 8 : 0) | (li.carried ? 16 : 0) | (cfg_.strict ? 32 : 0) |
                                 (li.side_pending ? 64 : 0) | (door_.tamper() ? 128 : 0));
  s.last_enrolled_id = static_cast<int8_t>(last_enrolled_);
  s.side = static_cast<uint8_t>(li.side);
  s.side_reason = static_cast<uint8_t>(li.side_reason);
  s.door_state = static_cast<uint8_t>(door_.state());
  float a = door_.angle() * 10.f;
  s.door_angle_ddeg = static_cast<int16_t>(a > 32000.f ? 32000.f : (a < -32000.f ? -32000.f : a));
  s.door_flags = static_cast<uint8_t>((door_.trusted() ? 1 : 0) | (door_.reference_known() ? 2 : 0));
  s.uptime_ms = now;
  s.log_next_seq = log_.next_seq();
  s.door_cal_steps = door_cal_.steps_done;
  s.motor = lock_.drive().calibration().motor;
  s.relock_s = clamp_u8(static_cast<int32_t>(cfg_.relock_after_close_ms / 1000u));
  return s;
}

}  // namespace slock

namespace slock {

// The enrolment gesture: enroll_gesture_turns knob/key turns within enroll_gesture_ms.
void App::on_hand_turn(Millis now) {
  const uint8_t need = cfg_.enroll_gesture_turns;
  if (need == 0 || need > 8) return;
  if (turn_n_ == 8) {
    for (uint8_t i = 1; i < 8; ++i) turns_[i - 1] = turns_[i];
    --turn_n_;
  }
  turns_[turn_n_++] = now;
  if (turn_n_ >= need && since(now, turns_[turn_n_ - need]) <= cfg_.enroll_gesture_ms) {
    open_enrollment(now, cfg_.enroll_window_ms);
    turn_n_ = 0;
  }
}

}  // namespace slock
