#include "side.h"

namespace slock {

namespace {
// Signed difference a - b on the wrapping millisecond clock.
int32_t diff(Millis a, Millis b) { return static_cast<int32_t>(a - b); }
}  // namespace

void SideTracker::begin(Millis now) {
  boot_at_ = now;
  for (Track& t : k_) {
    t = Track{};
    t.down_at = now;
  }
  door_count_ = 0;
  door_head_ = 0;
  door_lost_ = false;
  door_open_ = false;
  change_n_ = 0;
  block_armed_ = false;
  block_ready_ = false;
}

void SideTracker::change(uint8_t key, Side side, SideReason reason) {
  Track& t = k_[key];
  bool user = reason == SideReason::User;
  if (t.side == side && !user) return;  // no change (a user confirmation is always reported)
  t.side = side;
  t.reason = reason;
  SideChange c;
  c.changed = true;
  c.key = key;
  c.side = side;
  c.reason = reason;
  if (change_n_ == kKeys) {  // queue full: drop the oldest
    for (uint8_t i = 1; i < kKeys; ++i) changes_[i - 1] = changes_[i];
    --change_n_;
  }
  changes_[change_n_++] = c;
}

SideChange SideTracker::take_change() {
  if (change_n_ == 0) return SideChange{};
  SideChange c = changes_[0];
  for (uint8_t i = 1; i < change_n_; ++i) changes_[i - 1] = changes_[i];
  --change_n_;
  return c;
}

void SideTracker::record_door_event(Millis now) {
  if (door_count_ == kDoorEvents) {
    door_lost_ = true;
    door_lost_at_ = door_events_[door_head_];  // the oldest one is overwritten
  } else {
    ++door_count_;
  }
  door_events_[door_head_] = now;
  door_head_ = static_cast<uint8_t>((door_head_ + 1) % kDoorEvents);
}

bool SideTracker::door_event_within(uint32_t age_ms, Millis now) const {
  for (uint8_t i = 0; i < door_count_; ++i) {
    if (since(now, door_events_[i]) < age_ms) return true;
  }
  return door_lost_ && since(now, door_lost_at_) < age_ms;
}

float SideTracker::average(const Track& t, Millis from, Millis to, uint16_t& n) const {
  float sum = 0.f;
  n = 0;
  for (uint8_t i = 0; i < t.count; ++i) {
    const Sample& s = t.hist[i];
    if (diff(s.t, from) >= 0 && diff(to, s.t) >= 0) {
      sum += s.dbm;
      ++n;
    }
  }
  return n > 0 ? sum / n : 0.f;
}

void SideTracker::continuity(uint8_t key, Track& t, const PhoneContext& ctx, Millis now) {
  const uint32_t age_down = since(now, t.down_at);
  const bool diary = ctx.provided && ctx.has_home && ctx.version >= 3;
  // Minutes are rounded down by the phone, so the real age is up to a minute more.
  const bool left = diary && ctx.minutes_since_exit != kNoMinutes &&
                    (static_cast<uint32_t>(ctx.minutes_since_exit) + 1u) * 60000u <= age_down;
  if (left && ctx.inside_home) {
    // It left the home area after the lock last saw it: it was outside when it came back. Door
    // events around or after that return are unexplained (that could be it walking in).
    uint32_t window = cfg_.diary_margin_ms;
    if (ctx.minutes_since_entry != kNoMinutes) {
      window += (static_cast<uint32_t>(ctx.minutes_since_entry) + 1u) * 60000u;
    } else {
      window = age_down;  // inside, but no entry time: can't place the return
    }
    if (window > age_down) window = age_down;
    change(key, door_event_within(window, now) ? Side::Unknown : Side::Outside, SideReason::Continuity);
    return;
  }
  if (door_event_within(age_down, now)) change(key, Side::Unknown, SideReason::Continuity);
}

void SideTracker::on_login(uint8_t key, const PhoneContext& ctx, bool first_login, Millis now) {
  if (key >= kKeys) return;
  Track& t = k_[key];
  bool was_connected = t.connected;
  t.connected = true;
  t.have_steps = ctx.provided && ctx.has_steps;
  t.secs_since_step = ctx.seconds_since_step;
  t.ctx_at = now;
  if (first_login && !was_connected) continuity(key, t, ctx, now);
  if (ctx.provided && ctx.has_home && !ctx.inside_home) {  // away from home: Outside, whatever happened
    t.pending = false;
    change(key, Side::Outside, SideReason::Away);
  }
  if (door_open_ && !t.in_episode) {
    t.in_episode = true;
    t.at_door_seen = false;
    t.peak = -127.f;
  }
}

void SideTracker::on_disconnect(uint8_t key, Millis now) {
  if (key >= kKeys) return;
  Track& t = k_[key];
  t.connected = false;
  t.down_at = now;
  t.in_episode = false;
  t.count = 0;
  t.head = 0;
  if (t.pending) {
    t.faded = true;  // went out of range right after the door closed
    decide(key, t, true, now);
  }
}

void SideTracker::forget(uint8_t key, Millis now) {
  if (key >= kKeys) return;
  k_[key] = Track{};
  k_[key].down_at = now;
  change(key, Side::Unknown, SideReason::Revoked);
}

void SideTracker::on_rssi(uint8_t key, float dbm, bool at_door, Millis now) {
  if (key >= kKeys) return;
  Track& t = k_[key];
  t.hist[t.head] = Sample{now, dbm};
  t.head = static_cast<uint8_t>((t.head + 1) % kHistory);
  if (t.count < kHistory) ++t.count;

  if (door_open_) {
    if (!t.in_episode) {
      t.in_episode = true;
      t.at_door_seen = false;
      t.peak = -127.f;
    }
    if (dbm > t.peak) t.peak = dbm;
    if (at_door) t.at_door_seen = true;
  }

  if (t.pending) {
    int32_t age = diff(now, t.close_at);
    if (age >= static_cast<int32_t>(cfg_.post_from_ms)) {
      if (age <= static_cast<int32_t>(cfg_.post_to_ms)) {
        t.post_sum += dbm;
        ++t.post_n;
      }
      ++t.follow_n;
      bool above = dbm >= static_cast<float>(cal_->outside_at_door + 4);
      if (above) ++t.follow_above;
      if (age <= static_cast<int32_t>(cfg_.settle_ms)) {
        ++t.early_n;
        if (above) ++t.early_above;
      }
      if (dbm <= static_cast<float>(cal_->outside_at_door + 1)) ++t.follow_outside;
      if (dbm <= static_cast<float>(cal_->outside_at_door - 8)) ++t.follow_far;
    }
  }
}

void SideTracker::on_door_opened(Millis now) {
  record_door_event(now);
  for (uint8_t i = 0; i < kKeys; ++i) {
    Track& t = k_[i];
    if (t.pending) decide(i, t, true, now);  // reopened before the decision: use what we have
    if (t.connected) {
      t.in_episode = true;
      t.at_door_seen = false;
      t.peak = -127.f;
    }
  }
  door_open_ = true;
}

void SideTracker::on_door_closed(Millis close_at, Millis now) {
  door_open_ = false;
  record_door_event(now);
  for (uint8_t i = 0; i < kKeys; ++i) {
    Track& t = k_[i];
    if (!t.in_episode) continue;
    t.in_episode = false;
    if (!t.at_door_seen || !t.connected) continue;  // not at the door: this transit wasn't theirs

    t.pending = true;
    t.close_at = close_at;
    uint16_t n = 0;
    t.pre = average(t, close_at - cfg_.pre_ms, close_at - cfg_.pre_gap_ms, n);
    t.have_pre = n > 0;
    t.post_sum = 0.f;
    t.post_n = 0;
    t.follow_n = 0;
    t.follow_above = 0;
    t.follow_outside = 0;
    t.follow_far = 0;
    t.early_n = 0;
    t.early_above = 0;
    t.faded = false;
    // Samples already received after the close moment (the close is reported once the door settles).
    for (uint8_t j = 0; j < t.count; ++j) {
      const Sample& s = t.hist[j];
      int32_t age = diff(s.t, close_at);
      if (age < static_cast<int32_t>(cfg_.post_from_ms)) continue;
      if (age <= static_cast<int32_t>(cfg_.post_to_ms)) {
        t.post_sum += s.dbm;
        ++t.post_n;
      }
      ++t.follow_n;
      bool above = s.dbm >= static_cast<float>(cal_->outside_at_door + 4);
      if (above) ++t.follow_above;
      if (age <= static_cast<int32_t>(cfg_.settle_ms)) {
        ++t.early_n;
        if (above) ++t.early_above;
      }
      if (s.dbm <= static_cast<float>(cal_->outside_at_door + 1)) ++t.follow_outside;
      if (s.dbm <= static_cast<float>(cal_->outside_at_door - 8)) ++t.follow_far;
    }
    uint32_t still_s = t.secs_since_step;
    if (t.have_steps && still_s != 0xFFFF) still_s += since(close_at, t.ctx_at) / 1000u;
    t.stationary = t.have_steps && t.secs_since_step != 0xFFFF && still_s >= cfg_.stationary_s;
  }
}

void SideTracker::decide(uint8_t key, Track& t, bool final, Millis now) {
  const bool settled_window_done = since(now, t.close_at) >= cfg_.settle_ms;
  const DoorCalibration& c = *cal_;
  const float block = c.block_db < 3 ? 3.f : static_cast<float>(c.block_db);
  int out = 0, in = 0;

  // 1. The signal step at the close moment (primary).
  if (t.have_pre && t.post_n > 0) {
    float step = t.post_sum / t.post_n - t.pre;
    if (step <= -0.6f * block) out += 2;
    else if (step >= -0.25f * block) in += 2;
  }
  // 2. It stayed at "outside at the door" levels the whole time the door was open.
  const float outside_open = static_cast<float>(c.outside_at_door) + block;
  if (t.peak <= outside_open + 2.f && outside_open + 2.f < static_cast<float>(c.inside_at_lock) - 2.f) out += 1;
  // 3. What it does next: inside levels, or outside levels / fading down the hallway (primary).
  //    Levels in between (a far room inside, or just outside) say nothing.
  if (t.follow_n >= cfg_.min_follow_samples) {
    // A phone outside a closed door can't reach inside levels, so mostly-inside levels right after
    // the close count even if it then goes somewhere far (a bedroom).
    bool settled_inside = t.early_n >= cfg_.min_follow_samples && t.early_above * 10u >= t.early_n * 6u;
    if (settled_inside || t.follow_above * 10u >= t.follow_n * 6u) in += 2;
    else if (t.follow_above == 0 && t.follow_far * 2u >= t.follow_n) out += 2;
    else if (t.follow_above == 0 && t.follow_outside * 10u >= t.follow_n * 8u) out += 1;
  }
  if (t.faded && t.follow_above == 0) out += 2;

  Side result;
  if (out >= 4 && in == 0) {
    result = Side::Outside;
  } else if (out == 0 && (in >= 4 || (in >= 2 && settled_window_done))) {
    result = Side::Inside;  // the safe answer may come early: nothing points outside
  } else if (!final) {
    return;  // keep watching
  } else if (in >= 2 && in > out) {
    result = Side::Inside;  // the safe answer: Inside and Unknown both block auto-unlock
  } else if (in > 0 && out > 0) {
    result = Side::Unknown;  // mixed
  } else if (t.side == Side::Outside && in == 0 && (out > 0 || t.stationary)) {
    result = Side::Outside;  // weak but consistent with where it was (tie-breaker)
  } else if (t.side == Side::Inside && out == 0) {
    result = Side::Inside;
  } else {
    result = Side::Unknown;
  }
  t.pending = false;
  change(key, result, SideReason::Transit);
}

void SideTracker::tick(Millis now) {
  for (uint8_t i = 0; i < kKeys; ++i) {
    Track& t = k_[i];
    if (!t.pending) continue;
    uint32_t age = since(now, t.close_at);
    if (block_armed_ && block_key_ == i && age > cfg_.post_to_ms) {
      block_armed_ = false;
      if (t.have_pre && t.post_n > 0) {
        float drop = t.pre - t.post_sum / t.post_n;
        if (drop >= 3.f) {
          block_db_ = static_cast<uint8_t>(drop > 30.f ? 30.f : drop + 0.5f);
          block_ready_ = true;
        }
      }
    }
    if (age >= cfg_.follow_ms) decide(i, t, true, now);
    else if (age > cfg_.post_to_ms) decide(i, t, false, now);
  }
}

bool SideTracker::take_block_result(uint8_t& db) {
  if (!block_ready_) return false;
  block_ready_ = false;
  db = block_db_;
  return true;
}

void SideTracker::set_side(uint8_t key, Side side, SideReason reason, Millis now) {
  (void)now;
  if (key >= kKeys) return;
  k_[key].pending = false;
  change(key, side, reason);
}

}  // namespace slock
