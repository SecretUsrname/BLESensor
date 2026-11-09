#include "check.h"
#include "slock/intent.h"
#include "slock/policy.h"
#include "slock/presence.h"

using namespace slock;

TEST(zones_use_hysteresis) {
  PresenceConfig cfg;
  cfg.ema_alpha = 1.f;  // no smoothing, to test thresholds exactly
  ZoneTracker z(&cfg);
  CHECK(z.zone() == Zone::Gone);
  z.on_rssi(-50);
  CHECK(z.zone() == Zone::Gone);  // ignored while disconnected
  z.reset();
  CHECK(z.zone() == Zone::Far);
  z.on_rssi(-90);
  CHECK(z.zone() == Zone::Far);
  z.on_rssi(-78);
  CHECK(z.zone() == Zone::Near);
  z.on_rssi(-81);
  CHECK(z.zone() == Zone::Near);  // within hysteresis
  z.on_rssi(-83);
  CHECK(z.zone() == Zone::Far);
  z.on_rssi(-60);
  CHECK(z.zone() == Zone::Close);
  z.on_rssi(-68);
  CHECK(z.zone() == Zone::Close);  // within hysteresis
  z.on_rssi(-70);
  CHECK(z.zone() == Zone::Near);
  z.clear();
  CHECK(z.zone() == Zone::Gone);
}

TEST(rssi_is_smoothed) {
  PresenceConfig cfg;  // alpha 0.3
  ZoneTracker z(&cfg);
  z.reset();
  z.on_rssi(-60);
  CHECK_NEAR(z.rssi(), -60, 1e-4);  // first reading taken as-is
  z.on_rssi(-90);
  CHECK_NEAR(z.rssi(), -69, 1e-4);  // moves 30% of the way
}

TEST(policy_rules) {
  PolicyInputs ok;
  ok.authenticated = ok.armed = ok.at_door = ok.context_ok = ok.locked = true;
  CHECK(decide(UnlockMode::ApproachOnly, ok) == Decision::Unlock);
  CHECK(decide(UnlockMode::ApproachAndIntent, ok) == Decision::None);  // needs a handle touch
  PolicyInputs touched = ok;
  touched.intent = true;
  CHECK(decide(UnlockMode::ApproachAndIntent, touched) == Decision::Unlock);

  bool PolicyInputs::*required[] = {&PolicyInputs::authenticated, &PolicyInputs::armed, &PolicyInputs::at_door,
                                    &PolicyInputs::context_ok, &PolicyInputs::locked};
  for (auto field : required) {
    PolicyInputs x = touched;
    x.*field = false;
    CHECK(decide(UnlockMode::ApproachAndIntent, x) == Decision::None);
    CHECK(decide(UnlockMode::ApproachOnly, x) == Decision::None);
  }
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
