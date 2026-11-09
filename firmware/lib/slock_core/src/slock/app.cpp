#include "app.h"

#include <cstring>

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

App::App(LockController& lock, KeyStore& keys, Verifier& verifier, hal::Random& rng, hal::Battery& battery,
         const uint8_t* lock_id, const AppConfig& cfg)
    : lock_(lock), keys_(keys), verifier_(verifier), rng_(rng), battery_(battery), cfg_(cfg) {
  std::memcpy(lock_id_, lock_id, kLockIdLen);
  for (Link& l : links_) {
    l.session.bind(&keys_, &verifier_, &rng_, lock_id_, cfg_.session, &guard_);
    l.zones.bind(&cfg_.presence);
  }
}

void App::begin(Millis now) {
  boot_at_ = now;  // every phone counts as "away since boot"
  lock_.begin(now);
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

void App::reset_link(Link& l) {
  l.used = false;
  l.session.end();
  l.zones.clear();
  l.key = -1;
  l.was_auth = false;
  l.fresh = nullptr;
  l.arrival_checked = false;
  l.armed = false;
  l.in_close = false;
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
  if (l->key >= 0) {
    seen_[l->key] = true;
    last_down_[l->key] = now;
  }
  reset_link(*l);
}

void App::on_authenticated(Link& l, Millis now) {
  l.key = l.session.key_id();
  if (l.arrival_checked) return;  // renewed login on the same connection: never a new arrival
  l.arrival_checked = true;

  for (const Link& other : links_) {  // the same phone already connected another way: not arriving
    if (&other != &l && other.used && other.key == l.key) return;
  }
  Millis gone_since = seen_[l.key] ? last_down_[l.key] : boot_at_;
  if (since(now, gone_since) >= cfg_.presence.min_away_ms) {
    l.armed = true;
    l.armed_at = now;
  }
}

AuthResult App::on_auth_response(uint16_t conn, const uint8_t* data, size_t len, Millis now) {
  Link* l = find(conn);
  if (l == nullptr) return AuthResult::NoChallenge;
  AuthResult r;
  if (len == 1 + kSigLen) {
    r = l->session.respond(data[0], data + 1, now);
  } else if (len == 1 + kContextLen + kSigLen) {
    r = l->session.respond_with_context(data[0], data + 1, data + 1 + kContextLen, now);
  } else {
    r = AuthResult::BadSignature;
  }
  l->last_auth = r;
  if (r == AuthResult::Ok) on_authenticated(*l, now);
  return r;
}

bool App::approach_only_allowed() const {
  return lock_.has_door_sensor() && lock_.auto_relock_ms() > 0;
}

bool App::set_mode(UnlockMode m) {
  if (m == UnlockMode::ApproachOnly && !approach_only_allowed()) return false;
  cfg_.mode = m;
  return true;
}

CmdResult App::on_command(uint16_t conn, const uint8_t* data, size_t len, Millis now) {
  Link* l = find(conn);
  if (l == nullptr) return CmdResult::NotAuthenticated;
  CmdResult r = CmdResult::BadCommand;
  if (!l->session.authenticated(now)) {
    r = CmdResult::NotAuthenticated;
  } else if (len >= 1) {
    switch (static_cast<protocol::Opcode>(data[0])) {
      case protocol::Opcode::Unlock: r = from_request(lock_.request_unlock(now)); break;
      case protocol::Opcode::Lock: r = from_request(lock_.request_lock(now)); break;
      case protocol::Opcode::Calibrate: r = from_request(lock_.request_calibration(now)); break;
      case protocol::Opcode::SetMode:
        if (len >= 2 && data[1] <= 1) {
          r = set_mode(data[1] == 0 ? UnlockMode::ApproachOnly : UnlockMode::ApproachAndIntent)
                  ? CmdResult::Ok : CmdResult::NotAllowed;
        }
        break;
      case protocol::Opcode::SetRelock:
        if (len >= 3) {
          uint32_t seconds = static_cast<uint32_t>(data[1] | (data[2] << 8));
          if (seconds == 0 && cfg_.mode == UnlockMode::ApproachOnly) {
            r = CmdResult::NotAllowed;  // approach-only depends on auto-relock
          } else {
            lock_.set_auto_relock_ms(seconds * 1000u);
            r = CmdResult::Ok;
          }
        }
        break;
      case protocol::Opcode::RevokeKey:
        if (len >= 2 && keys_.remove(data[1])) {
          for (Link& other : links_) {
            if (other.used && other.key == data[1]) {
              other.session.end();
              other.key = -1;
              other.armed = false;
              other.was_auth = false;
            }
          }
          r = CmdResult::Ok;
        }
        break;
      case protocol::Opcode::Rechallenge:
        l->fresh = l->session.new_challenge();
        l->was_auth = false;  // the expected gap until the answer isn't an expiry
        r = CmdResult::Ok;
        break;
    }
  }
  if (find(conn) != nullptr) l->last_cmd = r;
  return r;
}

void App::on_rssi(uint16_t conn, int8_t dbm, Millis now) {
  (void)now;
  Link* l = find(conn);
  if (l != nullptr) l->zones.on_rssi(dbm);
}

const uint8_t* App::take_new_challenge(uint16_t conn) {
  Link* l = find(conn);
  if (l == nullptr) return nullptr;
  const uint8_t* c = l->fresh;
  l->fresh = nullptr;
  return c;
}

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
  if (id >= 0) {
    enroll_open_ = false;  // one phone per window
    seen_[id] = false;
  }
  last_enrolled_ = id;
  return id;
}

bool App::at_door(const Link& l, Millis now) const {
  if (!l.armed || !l.in_close) return false;
  // The window starts at whichever came later: reaching Close or being armed.
  Millis start = since(l.close_at, l.armed_at) < 0x80000000u ? l.close_at : l.armed_at;
  return since(now, start) < cfg_.presence.door_window_ms;
}

bool App::context_ok(const PhoneContext& c) const {
  if (!c.provided || !c.has_home) return !cfg_.require_home_context;
  return c.inside_home && c.minutes_since_entry <= cfg_.max_minutes_since_home_entry;
}

void App::tick(Millis now) {
  lock_.tick(now);
  if (intent_pending_ && since(now, intent_at_) > cfg_.intent_hold_ms) intent_pending_ = false;

  for (Link& l : links_) {
    if (!l.used) continue;
    bool auth = l.session.authenticated(now);
    if (l.was_auth && !auth && l.key >= 0 && keys_.get(static_cast<uint8_t>(l.key)) != nullptr) {
      l.fresh = l.session.new_challenge();  // login expired on a live link: ask again
    }
    l.was_auth = auth;
    if (l.armed && since(now, l.armed_at) >= cfg_.presence.armed_ms) l.armed = false;
    if (l.zones.zone() == Zone::Close) {
      if (!l.in_close) {
        l.in_close = true;
        l.close_at = now;
      }
    } else {
      l.in_close = false;
    }
  }

  bool locked = lock_.state() == LockState::Locked;
  for (Link& l : links_) {
    if (!l.used) continue;
    PolicyInputs in;
    in.authenticated = l.session.authenticated(now);
    in.armed = l.armed;
    in.at_door = at_door(l, now);
    in.context_ok = context_ok(l.session.context());
    in.intent = intent_pending_;
    in.locked = locked;
    if (decide(cfg_.mode, in) == Decision::Unlock && lock_.request_unlock(now) == Request::Accepted) {
      l.armed = false;  // one unlock per arrival
      intent_pending_ = false;
      ++auto_unlocks_;
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
  info.armed = l->armed;
  info.armed_ms_left = l->armed ? cfg_.presence.armed_ms - since(now, l->armed_at) : 0;
  info.at_door = at_door(*l, now);
  info.context = l->session.context();
  info.context_ok = context_ok(info.context);
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
  s.mode = static_cast<uint8_t>(cfg_.mode);
  s.last_cmd_result = static_cast<uint8_t>(l != nullptr ? l->last_cmd : CmdResult::Ok);
  s.last_auth_result = static_cast<uint8_t>(l != nullptr ? l->last_auth : AuthResult::NoChallenge);
  s.zone = static_cast<uint8_t>(li.zone);
  s.flags = static_cast<uint8_t>((li.armed ? 1 : 0) | (enrollment_open(now) ? 2 : 0) | (li.at_door ? 4 : 0) |
                                 (li.context_ok ? 8 : 0));
  s.last_enrolled_id = static_cast<int8_t>(last_enrolled_);
  return s;
}

}  // namespace slock
