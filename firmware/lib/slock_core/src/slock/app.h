#pragma once
// Application glue for the v1 framework (docs/v1-framework.md): BLE connections, authentication,
// per-phone side tracking, the gyro door estimator, the unlock rule, relocking and the event log.
// The board's BLE layer and the PC digital twin both drive this class.
//
// Auto-unlock (section 8) for a connected phone needs ALL of: logged in, side Outside (and no side
// decision pending), at the door (~1 m), signs "inside home area", carried recently (signed steps),
// the handle turned, and the bolt locked. When a phone reaches the door, and when the door opens,
// the lock asks it for fresh signed context.
//
// Relocking (section 9): after an auto-unlock, relock after relock_unopened_ms if the door wasn't
// opened. After a trusted close, relock after relock_after_close_ms. Never lock into a door that is
// known to be open.

#include "door.h"
#include "event_log.h"
#include "lock_controller.h"
#include "policy.h"
#include "presence.h"
#include "protocol.h"
#include "session.h"
#include "side.h"

namespace slock {

struct AppConfig {
  bool strict = true;                     // refuse phones that don't sign home context and steps
  uint32_t intent_hold_ms = 1500;         // a handle turn stays valid this long
  uint32_t carried_s = 30;                // "carried recently": a step within this many seconds
  uint32_t relock_unopened_ms = 15000;    // after an auto-unlock, if the door isn't opened
  uint32_t relock_after_close_ms = 3000;  // after a trusted close; 0 = off
  uint32_t refresh_min_ms = 3000;         // ask a phone for fresh context at most this often
  // Pairing needs physical access: turning the knob (or key) this many times within gesture_ms
  // opens the enrolment window for enroll_window_ms.
  uint8_t enroll_gesture_turns = 3;
  uint32_t enroll_gesture_ms = 10000;
  uint32_t enroll_window_ms = 60000;
  PresenceConfig presence;
  SessionConfig session;
  DoorConfig door;
  SideConfig side;
};

enum class CmdResult : uint8_t {
  Ok, NotAuthenticated, BadCommand, Busy, NotReady, AlreadyThere, LowBattery, NotAllowed, DoorOpen
};

// Snapshot of one connection, for status packets, the twin and tests.
struct LinkInfo {
  bool used = false;
  uint16_t conn = 0;
  int key_id = -1;            // enrolled key that logged in on this link (kept while connected)
  bool authenticated = false;
  Zone zone = Zone::Gone;
  float rssi = -127.f;
  float fast_rssi = -127.f;
  bool at_door = false;
  PhoneContext context;
  bool home_ok = false;
  bool carried = false;
  Side side = Side::Unknown;
  SideReason side_reason = SideReason::Boot;
  bool side_pending = false;
  bool ready = false;         // would unlock on a handle turn (if locked)
};

// Storage record ids.
enum : uint16_t { kStoreKeys = 1, kStoreSettings = 2, kStoreDriveCal = 3 };

class App {
 public:
  static constexpr int kMaxLinks = 3;

  App(LockController& lock, KeyStore& keys, Verifier& verifier, hal::Random& rng, hal::Battery& battery,
      const uint8_t* lock_id, const AppConfig& cfg, hal::Storage* storage = nullptr);

  void begin(Millis now);  // loads storage, every phone Unknown, homes the drive
  void tick(Millis now);   // call every 1-5 ms

  // BLE link events. conn = the BLE stack's connection handle (any value, unique while connected).
  const uint8_t* on_connect(uint16_t conn, Millis now);  // challenge nonce to publish, or nullptr if full
  void on_disconnect(uint16_t conn, Millis now);
  AuthResult on_auth_response(uint16_t conn, const uint8_t* data, size_t len, Millis now);
  CmdResult on_command(uint16_t conn, const uint8_t* data, size_t len, Millis now);
  void on_rssi(uint16_t conn, int8_t dbm, Millis now);
  // A fresh challenge to publish on this link (renewal, fresh context, Rechallenge), else nullptr.
  const uint8_t* take_new_challenge(uint16_t conn);
  int on_enroll(const uint8_t* pubkey, size_t len, Millis now);  // new key id, or -1
  // Log characteristic value for this link: count | events. Advances the link's read cursor.
  size_t log_page(uint16_t conn, uint8_t* out, size_t cap);

  // Physical inputs
  void on_intent(Millis now);  // handle turn detected
  // IMU sample: hinge-axis rate (deg/s, + = opening) and accelerometer deviation (milli-g).
  void on_imu(float rate_dps, float accel_dev_mg, Millis now);
  void open_enrollment(Millis now, uint32_t ms);  // e.g. after a knob gesture on the lock

  bool enrollment_open(Millis now) const;
  bool connected(uint16_t conn) const { return find(conn) != nullptr; }
  LinkInfo link_info(uint16_t conn, Millis now) const;
  protocol::StatusPacket status(uint16_t conn, Millis now) const;
  AppConfig& config() { return cfg_; }
  const DoorEstimator& door() const { return door_; }
  const DoorCalibration& door_calibration() const { return door_cal_; }
  const SideTracker& sides() const { return sides_; }
  const EventLog& log() const { return log_; }
  uint32_t auto_unlocks() const { return auto_unlocks_; }

 private:
  struct Link {
    bool used = false;
    uint16_t conn = 0;
    Session session;
    ZoneTracker zones;
    int key = -1;
    bool was_auth = false;
    bool logged_in_once = false;
    const uint8_t* fresh = nullptr;
    Millis ctx_at = 0;          // when the current context was signed
    bool refreshed = false;
    Millis refresh_at = 0;
    bool was_at_door = false;
    uint32_t log_cursor = 0;
    CmdResult last_cmd = CmdResult::Ok;
    AuthResult last_auth = AuthResult::NoChallenge;
  };

  Link* find(uint16_t conn);
  const Link* find(uint16_t conn) const;
  void reset_link(Link& l);
  bool key_on_other_link(const Link& l, int key) const;
  void on_authenticated(Link& l, Millis now);
  void refresh(Link& l, Millis now);
  PolicyInputs inputs(const Link& l, Millis now) const;
  bool home_ok(const PhoneContext& c) const;
  bool carried(const Link& l, Millis now) const;
  bool door_known_open() const;
  void door_closed(Millis at, bool trusted, Millis now);
  void watch_bolt(Millis now);
  void on_hand_turn(Millis now);
  void save_keys();
  void save_settings();

  LockController& lock_;
  KeyStore& keys_;
  Verifier& verifier_;
  hal::Random& rng_;
  hal::Battery& battery_;
  hal::Storage* storage_;
  uint8_t lock_id_[kLockIdLen];
  AppConfig cfg_;
  AuthGuard guard_;
  Link links_[kMaxLinks];
  DoorEstimator door_;
  DoorCalibration door_cal_;
  SideTracker sides_;
  EventLog log_;

  bool intent_pending_ = false;
  Millis intent_at_ = 0;
  bool enroll_open_ = false;
  Millis enroll_at_ = 0;
  uint32_t enroll_ms_ = 0;
  int last_enrolled_ = -1;
  uint32_t auto_unlocks_ = 0;

  bool relock_unopened_ = false;
  Millis relock_unopened_at_ = 0;
  bool relock_closed_ = false;
  Millis relock_closed_at_ = 0;

  LockState last_end_ = LockState::Uncalibrated;  // last Locked/Unlocked seen
  bool own_move_ = false;                          // a drive operation ran since then
  LockState last_state_ = LockState::Uncalibrated;
  bool cal_saved_ = false;
  Millis turns_[8] = {};  // times of recent knob/key turns (enrolment gesture)
  uint8_t turn_n_ = 0;
};

}  // namespace slock
