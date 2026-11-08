#pragma once
// Simulated mechanism for host tests: motor -> gear with arc slot -> hub lug -> bolt.
//
// Positions are in bolt degrees: u = 0 is locked, u = travel is unlocked. The lug can sit anywhere
// in [g, g + slot_span]. The gear's lower slot end pushes the bolt toward unlocked; the upper end
// pushes it toward locked. When the bolt hits an end (or a jam) the gear stalls.

#include <algorithm>
#include <cmath>

#include "slock/hal.h"

struct SimPlant : slock::hal::Motor,
                  slock::hal::Encoder,
                  slock::hal::HubSensor,
                  slock::hal::CurrentSense {
  float travel = 90.f;
  float slot_span = 102.f;
  float g = -30.f;              // gear position
  float u = 0.f;                // bolt position
  float jam_at = 1e9f;          // the motor can't push the bolt past this toward unlocked
  int plant_dir = +1;           // +1: positive duty turns the gear toward unlocked
  float sensor_offset = 20.f;   // hub sensor angle (deg) at u = 0
  float sensor_sign = 1.f;
  float cpd = 38.9f;            // encoder counts per gear degree (N20 1:150, 7 PPR x4, 40:12)
  float free_dps = 360.f;       // gear speed at full duty, unloaded
  double enc = 0;
  int16_t duty = 0;
  bool stalled = false;
  bool pushing = false;

  // --- HAL ---
  void drive(int16_t d) override { duty = d; }
  void brake() override { duty = 0; }
  int32_t count() override { return static_cast<int32_t>(std::lround(enc)); }
  void read(int16_t& a, int16_t& b) override {
    float ang = (sensor_offset + sensor_sign * u) * 3.14159265f / 180.f;
    a = static_cast<int16_t>(std::lround(1000.f * std::cos(ang)));
    b = static_cast<int16_t>(std::lround(1000.f * std::sin(ang)));
  }
  uint16_t milliamps() override {
    if (duty == 0) return 0;
    return stalled ? 900 : (pushing ? 300 : 90);
  }

  // --- physics ---
  void step(uint32_t ms) {
    for (uint32_t i = 0; i < ms; ++i) step1();
  }

  void step1() {
    stalled = pushing = false;
    if (duty == 0) return;
    float dg = static_cast<float>(plant_dir) * (duty / 1000.f) * free_dps / 1000.f;  // per ms
    float gn = g + dg;
    if (dg > 0 && u <= gn) {
      pushing = true;
      gn = g + dg * 0.6f;  // slower under load
      float limit = std::min(travel, jam_at);
      if (gn >= limit) {
        gn = limit;
        stalled = true;
      }
      u = std::max(u, gn);
    } else if (dg < 0 && u >= gn + slot_span) {
      pushing = true;
      gn = g + dg * 0.6f;
      if (gn + slot_span <= 0.f) {
        gn = -slot_span;
        stalled = true;
      }
      u = std::min(u, gn + slot_span);
    }
    enc += static_cast<double>(gn - g) * plant_dir * cpd;
    g = gn;
  }

  // Someone turns the knob or the key. Only possible within the slot's free range.
  bool turn_by_hand(float target) {
    float lo = std::max(0.f, g);
    float hi = std::min(travel, g + slot_span);
    u = std::min(std::max(target, lo), hi);
    return std::fabs(u - target) < 1e-3f;
  }

  void power_cycle() { enc = 0; duty = 0; }  // the encoder count is lost; the gear stays put
};
