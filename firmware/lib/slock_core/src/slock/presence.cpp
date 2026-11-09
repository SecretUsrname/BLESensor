#include "presence.h"

namespace slock {

void ZoneTracker::reset() {
  have_ = false;
  rssi_ = -127.f;
  fast_ = -127.f;
  zone_ = Zone::Far;
  at_door_ = false;
}

void ZoneTracker::clear() {
  have_ = false;
  rssi_ = -127.f;
  fast_ = -127.f;
  zone_ = Zone::Gone;
  at_door_ = false;
}

void ZoneTracker::on_rssi(int8_t dbm, int8_t at_door_dbm) {
  if (cfg_ == nullptr || zone_ == Zone::Gone) return;
  if (!have_) {
    rssi_ = dbm;
    fast_ = dbm;
    have_ = true;
  } else {
    rssi_ += cfg_->ema_alpha * (static_cast<float>(dbm) - rssi_);
    fast_ += cfg_->fast_alpha * (static_cast<float>(dbm) - fast_);
  }
  if (fast_ >= at_door_dbm) at_door_ = true;
  else if (fast_ < static_cast<float>(at_door_dbm - cfg_->at_door_hysteresis_db)) at_door_ = false;

  const float near_in = cfg_->near_dbm;
  const float near_out = cfg_->near_dbm - cfg_->hysteresis_db;
  const float close_in = cfg_->close_dbm;
  const float close_out = cfg_->close_dbm - cfg_->hysteresis_db;

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

}  // namespace slock
