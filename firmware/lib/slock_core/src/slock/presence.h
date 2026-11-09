#pragma once
// Proximity from BLE signal strength (RSSI), per connection.
//
// RSSI says roughly how close a phone is, never which side of the door it is on (the side comes
// from door events: side.h). Two filters: a slow one for the Far/Near/Close zones, and a faster
// one for "at the door" (about 1 m, calibrated per door) and for the side evidence at door closes.

#include "types.h"

namespace slock {

enum class Zone : uint8_t { Gone, Far, Near, Close };

struct PresenceConfig {
  float ema_alpha = 0.3f;     // slow smoothing for zones (0..1, higher = less smoothing)
  float fast_alpha = 0.6f;    // faster smoothing at close range
  int8_t near_dbm = -78;      // [TUNE with recorded traces] enter Near at or above this
  int8_t close_dbm = -65;     // [TUNE] enter Close at or above this
  uint8_t hysteresis_db = 4;  // leave a zone only when this far below its threshold
  uint8_t at_door_hysteresis_db = 3;
};

class ZoneTracker {
 public:
  explicit ZoneTracker(const PresenceConfig* cfg = nullptr) : cfg_(cfg) {}
  void bind(const PresenceConfig* cfg) { cfg_ = cfg; }

  void reset();  // connected, no reading yet: Far
  void clear();  // disconnected: Gone
  // at_door_dbm = the calibrated "about 1 m" level for this door.
  void on_rssi(int8_t dbm, int8_t at_door_dbm);

  Zone zone() const { return zone_; }
  float rssi() const { return rssi_; }       // slow
  float fast_rssi() const { return fast_; }  // fast
  bool at_door() const { return at_door_; }

 private:
  const PresenceConfig* cfg_;
  bool have_ = false;
  float rssi_ = -127.f;
  float fast_ = -127.f;
  Zone zone_ = Zone::Gone;
  bool at_door_ = false;
};

}  // namespace slock
