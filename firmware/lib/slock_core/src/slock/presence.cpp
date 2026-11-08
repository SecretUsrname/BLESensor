#include "presence.h"

namespace slock {

void PresenceTracker::begin(Millis now) {
  connected_ = false;
  disconnected_at_ = now;
  zone_ = Zone::Gone;
  arrival_ = false;
}

void PresenceTracker::on_connect(Millis now) {
  if (!connected_ && since(now, disconnected_at_) >= cfg_.min_away_ms) {
    arrival_ = true;
    arrival_at_ = now;
  }
  connected_ = true;
  have_rssi_ = false;
  zone_ = Zone::Far;
}

void PresenceTracker::on_disconnect(Millis now) {
  connected_ = false;
  disconnected_at_ = now;
  zone_ = Zone::Gone;
  arrival_ = false;
}

void PresenceTracker::on_rssi(int8_t dbm, Millis now) {
  (void)now;
  if (!connected_) return;
  if (!have_rssi_) {
    rssi_ = dbm;
    have_rssi_ = true;
  } else {
    rssi_ += cfg_.ema_alpha * (static_cast<float>(dbm) - rssi_);
  }

  const float near_in = cfg_.near_dbm;
  const float near_out = cfg_.near_dbm - cfg_.hysteresis_db;
  const float close_in = cfg_.close_dbm;
  const float close_out = cfg_.close_dbm - cfg_.hysteresis_db;

  switch (zone_) {
    case Zone::Gone:
    case Zone::Far:
      if (rssi_ >= close_in) zone_ = Zone::Close;
      else if (rssi_ >= near_in) zone_ = Zone::Near;
      else zone_ = Zone::Far;
      break;
    case Zone::Near:
      if (rssi_ >= close_in) zone_ = Zone::Close;
      else if (rssi_ < near_out) zone_ = Zone::Far;
      break;
    case Zone::Close:
      if (rssi_ < near_out) zone_ = Zone::Far;
      else if (rssi_ < close_out) zone_ = Zone::Near;
      break;
  }
}

bool PresenceTracker::arrival_active(Millis now) const {
  return connected_ && arrival_ && since(now, arrival_at_) < cfg_.arrival_window_ms;
}

uint32_t PresenceTracker::arrival_remaining_ms(Millis now) const {
  return arrival_active(now) ? cfg_.arrival_window_ms - since(now, arrival_at_) : 0;
}

}  // namespace slock
