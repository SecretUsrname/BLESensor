#include "check.h"
#include "slock/intent.h"
#include "slock/policy.h"
#include "slock/presence.h"

using namespace slock;

TEST(arrival_needs_a_real_absence) {
  PresenceConfig cfg;
  PresenceTracker t(cfg);
  t.begin(0);

  t.on_connect(10000);  // 10 s after boot: lock just rebooted, phone was probably here
  CHECK(!t.arrival_active(10000));

  t.on_disconnect(20000);
  t.on_connect(30000);  // brief BLE drop while the phone is inside
  CHECK(!t.arrival_active(30000));

  t.on_disconnect(40000);
  t.on_connect(40000 + 60000);  // away a full minute, then back
  CHECK(t.arrival_active(100000));
  CHECK(t.arrival_active(100000 + 29999));
  CHECK(!t.arrival_active(100000 + 30000));  // window over
}

TEST(away_time_is_adjustable) {
  PresenceConfig cfg;
  PresenceTracker t(cfg);
  t.begin(0);
  t.set_min_away_ms(10000);
  CHECK(t.min_away_ms() == 10000);
  t.on_connect(9999);
  CHECK(!t.arrival_active(9999));  // 9.999 s since boot: not long enough
  t.on_disconnect(20000);
  t.on_connect(30000);  // away exactly 10 s
  CHECK(t.arrival_active(30000));
}

TEST(phone_sitting_inside_never_arms) {
  PresenceConfig cfg;
  PresenceTracker t(cfg);
  t.begin(0);
  t.on_connect(5000);
  for (Millis now = 5000; now < 3600000; now += 1000) t.on_rssi(-50, now);  // right by the door for an hour
  CHECK(t.zone() == Zone::Close);
  CHECK(!t.arrival_active(3600000));
}

TEST(arrival_consumed_and_cleared_on_disconnect) {
  PresenceConfig cfg;
  PresenceTracker t(cfg);
  t.begin(0);
  t.on_connect(70000);
  CHECK(t.arrival_active(70000));
  t.consume_arrival();
  CHECK(!t.arrival_active(70001));

  t.on_disconnect(80000);
  t.on_connect(200000);
  CHECK(t.arrival_active(200000));
  t.on_disconnect(200500);
  CHECK(!t.arrival_active(200500));
}

TEST(zones_use_hysteresis) {
  PresenceConfig cfg;
  cfg.ema_alpha = 1.f;  // no smoothing, to test thresholds exactly
  PresenceTracker t(cfg);
  t.begin(0);
  t.on_connect(0);
  t.on_rssi(-90, 0);
  CHECK(t.zone() == Zone::Far);
  t.on_rssi(-78, 0);
  CHECK(t.zone() == Zone::Near);
  t.on_rssi(-81, 0);
  CHECK(t.zone() == Zone::Near);  // within hysteresis
  t.on_rssi(-83, 0);
  CHECK(t.zone() == Zone::Far);
  t.on_rssi(-60, 0);
  CHECK(t.zone() == Zone::Close);
  t.on_rssi(-68, 0);
  CHECK(t.zone() == Zone::Close);  // within hysteresis
  t.on_rssi(-70, 0);
  CHECK(t.zone() == Zone::Near);
  t.on_disconnect(1);
  CHECK(t.zone() == Zone::Gone);
}

TEST(policy_rules) {
  PolicyInputs in;
  in.authenticated = true;
  in.arrival = true;
  in.locked = true;
  in.zone = Zone::Near;

  CHECK(decide(UnlockMode::ApproachOnly, in) == Decision::None);  // needs Close
  in.zone = Zone::Close;
  CHECK(decide(UnlockMode::ApproachOnly, in) == Decision::Unlock);

  in.zone = Zone::Near;
  CHECK(decide(UnlockMode::ApproachAndIntent, in) == Decision::None);  // needs intent
  in.intent = true;
  CHECK(decide(UnlockMode::ApproachAndIntent, in) == Decision::Unlock);
  in.zone = Zone::Far;
  CHECK(decide(UnlockMode::ApproachAndIntent, in) == Decision::None);

  in.zone = Zone::Close;
  PolicyInputs x = in;
  x.authenticated = false;
  CHECK(decide(UnlockMode::ApproachAndIntent, x) == Decision::None);
  x = in;
  x.arrival = false;
  CHECK(decide(UnlockMode::ApproachAndIntent, x) == Decision::None);
  x = in;
  x.locked = false;
  CHECK(decide(UnlockMode::ApproachAndIntent, x) == Decision::None);
}

TEST(intent_detects_bursts_not_drift) {
  IntentConfig cfg;
  IntentDetector d(cfg);
  Millis now = 0;
  int events = 0;

  for (int i = 0; i < 400; ++i, now += 5) events += d.on_sample(0, 0, 1000, now);  // still door
  CHECK(events == 0);

  // Slow tilt: gravity direction changes but its magnitude doesn't.
  for (int i = 0; i < 400; ++i, now += 5) {
    float a = static_cast<float>(i) * 0.001f;
    events += d.on_sample(static_cast<int16_t>(1000 * std::sin(a)), 0,
                          static_cast<int16_t>(1000 * std::cos(a)), now);
  }
  CHECK(events == 0);

  events += d.on_sample(0, 0, 1300, now);  // one isolated knock
  now += 5;
  for (int i = 0; i < 100; ++i, now += 5) events += d.on_sample(0, 0, 1000, now);
  CHECK(events == 0);

  for (int i = 0; i < 4; ++i, now += 5) events += d.on_sample(0, 150, 1150, now);  // handle burst
  CHECK(events == 1);

  now += 400;
  for (int i = 0; i < 4; ++i, now += 5) events += d.on_sample(0, 150, 1150, now);
  CHECK(events == 1);  // refractory

  now += 2000;
  for (int i = 0; i < 4; ++i, now += 5) events += d.on_sample(0, 150, 1150, now);
  CHECK(events == 2);
}
