#pragma once
// A swinging door with the lock's gyro on it: angle, a biased and noisy gyro, and accelerometer
// impacts. Used by the door estimator tests and the App scenarios.

#include <cmath>
#include <cstdint>

struct DoorSim {
  float angle = 0.f;      // true angle, deg (0 = closed)
  float target = 0.f;
  float speed_dps = 90.f;
  float bias_dps = 0.7f;  // gyro zero-rate offset
  float bias_drift_dps_per_min = 0.f;
  float noise_dps = 0.15f;
  bool slam = false;      // hit the frame with an impact when reaching 0
  float impact_mg = 0.f;  // set on the sample the door hits the frame
  float knock_mg = 0.f;   // one-off vibration with no rotation (e.g. knocking)
  float forced_dps = 0.f; // rotation forced on a door (e.g. tamper on a locked door)
  uint32_t rng = 12345;

  float noise() {
    rng = rng * 1664525u + 1013904223u;
    return (static_cast<float>(rng >> 8) / 16777216.f * 2.f - 1.f) * noise_dps;
  }

  void open(float to = 80.f, float dps = 90.f) {
    target = to;
    speed_dps = dps;
  }
  void close(bool with_slam, float dps = 90.f) {
    target = 0.f;
    speed_dps = dps;
    slam = with_slam;
  }

  // Advance dt_ms; returns the gyro rate (deg/s) and fills the accelerometer deviation.
  float step(uint32_t dt_ms, float& accel_mg) {
    float dt = static_cast<float>(dt_ms) / 1000.f;
    float before = angle;
    float d = target - angle;
    float max = speed_dps * dt;
    if (std::fabs(d) <= max) angle = target;
    else angle += d > 0 ? max : -max;
    angle += forced_dps * dt;
    target += forced_dps * dt;  // forced rotation moves where the door rests too
    accel_mg = 0.f;
    if (before > 0.f && angle <= 0.f && slam) {
      accel_mg = 900.f;
      slam = false;
    }
    if (knock_mg > 0.f) {
      accel_mg = knock_mg;
      knock_mg = 0.f;
    }
    bias_dps += bias_drift_dps_per_min * dt / 60.f;
    return (angle - before) / dt + bias_dps + noise();
  }
};
