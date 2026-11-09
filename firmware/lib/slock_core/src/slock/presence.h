#pragma once
// Proximity from BLE signal strength (RSSI), per connection.
//
// RSSI can only say "roughly how close", never "which side of the door" (see docs). It is used to
// decide WHEN an armed phone is at the door; WHETHER it is a genuine arrival is decided by the
// App (time away + the phone's signed home context).

#include "types.h"

namespace slock {

enum class Zone : uint8_t { Gone, Far, Near, Close };

struct PresenceConfig {
  float ema_alpha = 0.3f;           // RSSI smoothing (0..1, higher = less smoothing)
  int8_t near_dbm = -78;            // [TUNE with recorded traces] enter Near at or above this
  int8_t close_dbm = -65;           // [TUNE] enter Close at or above this
  uint8_t hysteresis_db = 4;        // leave a zone only when this far below its threshold
  uint32_t min_away_ms = 60000;     // a phone must have been gone this long for its next login to be an arrival
  uint32_t armed_ms = 300000;       // an arrival stays armed this long while the phone stays connected
  uint32_t door_window_ms = 30000;  // once an armed phone reaches Close, it may unlock for this long (while Close)
};

// Smoothed RSSI and zone with hysteresis for one connection.
class ZoneTracker {
 public:
  explicit ZoneTracker(const PresenceConfig* cfg = nullptr) : cfg_(cfg) {}
  void bind(const PresenceConfig* cfg) { cfg_ = cfg; }

  void reset();  // connected, no reading yet: Far
  void clear();  // disconnected: Gone
  void on_rssi(int8_t dbm);

  Zone zone() const { return zone_; }
  float rssi() const { return rssi_; }

 private:
  const PresenceConfig* cfg_;
  bool have_ = false;
  float rssi_ = -127.f;
  Zone zone_ = Zone::Gone;
};

}  // namespace slock
