#pragma once
// Which side of the door each enrolled phone is on. See docs/v1-framework.md sections 5-7.
//
//  * States: Outside, Inside, Unknown (Unknown behaves like Inside: no auto-unlock).
//  * Anchors: the phone signs "not in the home area" -> Outside. Lock reboot -> Unknown.
//  * Continuity: a side only stands if the lock saw every door event since it was set. Door
//    events while a phone was disconnected make it Unknown on reconnect, unless the phone's signed
//    diary shows it was away from home for them (it left the home area after disconnecting, and
//    the events happened before it came back, with a margin for geofence lag).
//  * Re-evaluation: every phone that was AT THE DOOR while the door was open gets a fresh decision
//    after the door closes, from the signal step at the close moment (the door now between phone
//    and lock = a drop of about the calibrated blocking amount), whether it stayed at outside
//    levels, and what the signal does over the next seconds. Steps and the previous side are only
//    secondary. Outside (the risky answer) needs strong evidence; mixed evidence -> Unknown.
//  * Users may set their side at any time; that is trusted like a measured side.
//
// Indexed by enrolled key id, so a side survives reconnections. RSSI comes from whichever link the
// phone is logged in on.

#include "session.h"

namespace slock {

enum class Side : uint8_t { Unknown = 0, Inside = 1, Outside = 2 };
enum class SideReason : uint8_t { Boot, Transit, Away, User, Continuity, Revoked };

// Per-door signal references from the install wizard (docs section 5). Defaults until calibrated.
struct DoorCalibration {
  int8_t outside_at_door = -58;  // phone outside, at the door, door closed
  int8_t inside_at_lock = -48;   // phone inside, at the lock
  int8_t two_m = -68;            // phone about 2 m away
  uint8_t block_db = 8;          // drop when the door closes between phone and lock
  uint8_t steps_done = 0;        // bit n = wizard step n+1 recorded
  // "At the door" (about 1 m): halfway between the at-door and 2 m references.
  int8_t at_door_dbm() const { return static_cast<int8_t>((outside_at_door + two_m) / 2); }
};

struct SideConfig {
  uint32_t pre_ms = 1500;          // signal averaged over this long before the close moment...
  uint32_t pre_gap_ms = 150;       // ...stopping this long before it
  uint32_t post_from_ms = 300;     // signal after the close, from here...
  uint32_t post_to_ms = 1500;      // ...to here (short: the door blocks the instant it shuts)
  uint32_t follow_ms = 15000;      // decide at the latest this long after the close
  uint32_t settle_ms = 5000;       // the first part of the follow window, judged on its own too
  uint8_t min_follow_samples = 5;
  uint32_t diary_margin_ms = 120000;  // geofence lag allowance around exit/entry times
  uint32_t stationary_s = 60;      // no steps for this long at the close = it wasn't carried through
};

struct SideChange {
  bool changed = false;
  uint8_t key = 0;
  Side side = Side::Unknown;
  SideReason reason = SideReason::Boot;
};

class SideTracker {
 public:
  static constexpr uint8_t kKeys = KeyStore::kCapacity;
  static constexpr uint8_t kHistory = 48;    // recent RSSI samples per phone
  static constexpr uint8_t kDoorEvents = 16; // recent door event times (continuity rule)

  explicit SideTracker(const SideConfig& cfg = SideConfig{}) : cfg_(cfg) {}
  void set_calibration(const DoorCalibration* cal) { cal_ = cal; }

  void begin(Millis now);  // reboot: everyone Unknown, every phone "disconnected since now"

  // Phone link events. first_login = first login on this connection.
  void on_login(uint8_t key, const PhoneContext& ctx, bool first_login, Millis now);
  void on_disconnect(uint8_t key, Millis now);  // its last link went away
  void on_rssi(uint8_t key, float dbm, bool at_door, Millis now);
  void forget(uint8_t key, Millis now);  // key revoked or replaced

  // Door events from the estimator.
  void on_door_opened(Millis now);
  void on_door_closed(Millis close_at, Millis now);

  void set_side(uint8_t key, Side side, SideReason reason, Millis now);
  void tick(Millis now);

  // Wizard step 4: measure the blocking drop at the next close while this phone is outside at the door.
  void measure_block(uint8_t key) { block_key_ = key; block_armed_ = true; }
  bool take_block_result(uint8_t& db);

  // Side changes since the last call, one per call (for the event log and notifications).
  SideChange take_change();

  Side side(uint8_t key) const { return key < kKeys ? k_[key].side : Side::Unknown; }
  SideReason reason(uint8_t key) const { return key < kKeys ? k_[key].reason : SideReason::Boot; }
  bool pending(uint8_t key) const { return key < kKeys && k_[key].pending; }

 private:
  struct Sample {
    Millis t;
    float dbm;
  };
  struct Track {
    Side side = Side::Unknown;
    SideReason reason = SideReason::Boot;
    bool connected = false;
    Millis down_at = 0;
    // signal history
    Sample hist[kHistory] = {};
    uint8_t head = 0;
    uint8_t count = 0;
    // current door episode
    bool in_episode = false;
    bool at_door_seen = false;
    float peak = -127.f;
    // evaluation after a close
    bool pending = false;
    Millis close_at = 0;
    bool have_pre = false;
    float pre = 0.f;
    float post_sum = 0.f;
    uint16_t post_n = 0;
    uint16_t follow_n = 0;
    uint16_t follow_above = 0;
    uint16_t follow_outside = 0;  // at or below "outside at the door, door closed" levels
    uint16_t follow_far = 0;
    uint16_t early_n = 0;      // follow samples within settle_ms of the close...
    uint16_t early_above = 0;  // ...and how many of those were at inside levels
    bool faded = false;
    bool stationary = false;
    // latest signed context
    bool have_steps = false;
    uint16_t secs_since_step = 0xFFFF;
    Millis ctx_at = 0;
  };

  void decide(uint8_t key, Track& t, bool final, Millis now);
  void continuity(uint8_t key, Track& t, const PhoneContext& ctx, Millis now);
  void record_door_event(Millis now);
  bool door_event_within(uint32_t age_ms, Millis now) const;  // any door event younger than age_ms
  float average(const Track& t, Millis from, Millis to, uint16_t& n) const;
  void change(uint8_t key, Side side, SideReason reason);

  SideConfig cfg_;
  DoorCalibration default_cal_;
  const DoorCalibration* cal_ = &default_cal_;
  Track k_[kKeys];
  Millis door_events_[kDoorEvents] = {};
  uint8_t door_head_ = 0;
  uint8_t door_count_ = 0;
  bool door_lost_ = false;  // older door events were overwritten...
  Millis door_lost_at_ = 0; // ...the newest of which happened here
  Millis boot_at_ = 0;
  bool door_open_ = false;
  // pending side changes
  SideChange changes_[kKeys];
  uint8_t change_n_ = 0;
  // calibration of the blocking drop
  bool block_armed_ = false;
  uint8_t block_key_ = 0;
  bool block_ready_ = false;
  uint8_t block_db_ = 0;
};

}  // namespace slock
