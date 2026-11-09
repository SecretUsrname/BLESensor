#pragma once
// Phone presence from the BLE connection, and the "genuine arrival" signal that arms the lock.
//
// The key rule: a phone that is already near the door (e.g. inside the flat) must never arm the
// lock. Only a phone that was AWAY (disconnected for at least min_away_ms) and then connects
// counts as an arrival, and the arrival is only good for arrival_window_ms.

#include "types.h"

namespace slock {

enum class Zone : uint8_t { Gone, Far, Near, Close };

struct PresenceConfig {
  float ema_alpha = 0.3f;         // RSSI smoothing (0..1, higher = less smoothing)
  int8_t near_dbm = -78;          // [TUNE with recorded traces] enter Near at or above this
  int8_t close_dbm = -65;         // [TUNE] enter Close at or above this
  uint8_t hysteresis_db = 4;      // leave a zone only when this far below its threshold
  uint32_t min_away_ms = 60000;   // must have been disconnected this long for a connect to count
  uint32_t arrival_window_ms = 30000;
};

class PresenceTracker {
 public:
  explicit PresenceTracker(const PresenceConfig& cfg) : cfg_(cfg) {}

  void begin(Millis now);  // boot counts as "disconnected since now"
  void on_connect(Millis now);
  void on_disconnect(Millis now);
  void on_rssi(int8_t dbm, Millis now);

  Zone zone() const { return zone_; }
  float rssi() const { return rssi_; }
  bool arrival_active(Millis now) const;
  uint32_t arrival_remaining_ms(Millis now) const;
  bool connected() const { return connected_; }

  // How long the phone must have been disconnected for a reconnect to count as an arrival.
  // 60 s by default; shorter values make testing quicker but let brief dropouts at home count.
  void set_min_away_ms(uint32_t ms) { cfg_.min_away_ms = ms; }
  uint32_t min_away_ms() const { return cfg_.min_away_ms; }
  void consume_arrival() { arrival_ = false; }  // after an unlock, so it fires once per arrival

 private:
  PresenceConfig cfg_;
  bool connected_ = false;
  Millis disconnected_at_ = 0;
  bool have_rssi_ = false;
  float rssi_ = -127.f;
  Zone zone_ = Zone::Gone;
  bool arrival_ = false;
  Millis arrival_at_ = 0;
};

}  // namespace slock
