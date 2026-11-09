#pragma once
// Application glue: BLE connections, authentication, arrival detection, the unlock policy and the
// lock state machine. The board's BLE layer and the PC digital twin both drive this class.
//
// Unlock rules (see docs/scenarios.md for why each exists):
//  * ARRIVAL is judged per phone, at its first login on a connection: the phone (its key) must have
//    been disconnected for at least min_away_ms. Renewed logins on the same connection never count.
//  * An arrival stays ARMED for up to armed_ms while the phone stays connected.
//  * The unlock WINDOW opens only when the armed phone reaches the Close zone, lasts door_window_ms,
//    and closes as soon as the phone leaves Close (walking past the door disarms nothing, but
//    can't unlock either).
//  * If the phone signed HOME CONTEXT and has a home geofence, it must say it is inside and entered
//    within max_minutes_since_home_entry. This defeats relays (your phone isn't home) and phones
//    that never left (entered hours ago). require_home_context also refuses phones without it.
//  * Several phones can be connected; each is judged on its own.

#include "lock_controller.h"
#include "policy.h"
#include "presence.h"
#include "protocol.h"
#include "session.h"

namespace slock {

struct AppConfig {
  UnlockMode mode = UnlockMode::ApproachAndIntent;
  uint32_t intent_hold_ms = 1500;          // a handle touch stays valid this long
  bool require_home_context = false;       // refuse auto-unlock for phones that don't report home context
  uint8_t max_minutes_since_home_entry = 10;
  PresenceConfig presence;
  SessionConfig session;
};

enum class CmdResult : uint8_t {
  Ok, NotAuthenticated, BadCommand, Busy, NotReady, AlreadyThere, LowBattery, NotAllowed
};

// Snapshot of one connection, for status packets, the twin and tests.
struct LinkInfo {
  bool used = false;
  uint16_t conn = 0;
  int key_id = -1;            // enrolled key that logged in on this link (kept while connected)
  bool authenticated = false;
  Zone zone = Zone::Gone;
  float rssi = -127.f;
  bool armed = false;
  uint32_t armed_ms_left = 0;
  bool at_door = false;
  PhoneContext context;
  bool context_ok = false;
};

class App {
 public:
  static constexpr int kMaxLinks = 3;

  App(LockController& lock, KeyStore& keys, Verifier& verifier, hal::Random& rng, hal::Battery& battery,
      const uint8_t* lock_id, const AppConfig& cfg);

  void begin(Millis now);
  void tick(Millis now);  // call every 1-5 ms

  // BLE link events. conn = the BLE stack's connection handle (any value, unique while connected).
  const uint8_t* on_connect(uint16_t conn, Millis now);  // challenge nonce to publish, or nullptr if full
  void on_disconnect(uint16_t conn, Millis now);
  AuthResult on_auth_response(uint16_t conn, const uint8_t* data, size_t len, Millis now);
  CmdResult on_command(uint16_t conn, const uint8_t* data, size_t len, Millis now);
  void on_rssi(uint16_t conn, int8_t dbm, Millis now);
  // A fresh challenge to publish on this link (login renewal or Rechallenge), else nullptr.
  const uint8_t* take_new_challenge(uint16_t conn);
  int on_enroll(const uint8_t* pubkey, size_t len, Millis now);  // new key id, or -1

  // Physical inputs
  void on_intent(Millis now);                     // IntentDetector fired
  void open_enrollment(Millis now, uint32_t ms);  // e.g. after a knob gesture on the lock

  bool enrollment_open(Millis now) const;
  bool connected(uint16_t conn) const { return find(conn) != nullptr; }
  LinkInfo link_info(uint16_t conn, Millis now) const;
  protocol::StatusPacket status(uint16_t conn, Millis now) const;
  UnlockMode mode() const { return cfg_.mode; }
  bool set_mode(UnlockMode m);  // false if not allowed (approach-only needs a door sensor + auto-relock)
  AppConfig& config() { return cfg_; }
  uint32_t auto_unlocks() const { return auto_unlocks_; }

 private:
  struct Link {
    bool used = false;
    uint16_t conn = 0;
    Session session;
    ZoneTracker zones;
    int key = -1;
    bool was_auth = false;
    const uint8_t* fresh = nullptr;
    bool arrival_checked = false;
    bool armed = false;
    Millis armed_at = 0;
    bool in_close = false;
    Millis close_at = 0;
    CmdResult last_cmd = CmdResult::Ok;
    AuthResult last_auth = AuthResult::NoChallenge;
  };

  Link* find(uint16_t conn);
  const Link* find(uint16_t conn) const;
  void reset_link(Link& l);
  void on_authenticated(Link& l, Millis now);
  bool at_door(const Link& l, Millis now) const;
  bool context_ok(const PhoneContext& c) const;
  bool approach_only_allowed() const;

  LockController& lock_;
  KeyStore& keys_;
  Verifier& verifier_;
  hal::Random& rng_;
  hal::Battery& battery_;
  uint8_t lock_id_[kLockIdLen];
  AppConfig cfg_;
  AuthGuard guard_;
  Link links_[kMaxLinks];

  Millis boot_at_ = 0;
  bool seen_[KeyStore::kCapacity] = {};       // key has disconnected at least once since boot
  Millis last_down_[KeyStore::kCapacity] = {};

  bool intent_pending_ = false;
  Millis intent_at_ = 0;
  bool enroll_open_ = false;
  Millis enroll_at_ = 0;
  uint32_t enroll_ms_ = 0;
  int last_enrolled_ = -1;
  uint32_t auto_unlocks_ = 0;
};

}  // namespace slock
