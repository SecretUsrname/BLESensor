// SLock v1 board firmware for the XIAO nRF52840 Sense (Adafruit nRF52 core).
//
// Wires the portable core (lib/slock_core: slock::App and everything under it, the same code the
// host tests and the digital twin run) to the real hardware:
//   * motor, encoder, hub halls, INA219, battery           -> board_hal.h
//   * flash (LittleFS), hardware RNG, P-256 check (micro-ecc) -> board_hal.h
//   * LSM6DS3TR-C gyro/accelerometer at ~100 Hz          -> App::on_imu (door) + IntentDetector
//   * Bluetooth, up to 3 phones                          -> ble_link.cpp
//
// Not yet run on hardware. USB serial bench console (115200):
//   c calibrate the motor (turn the bolt to LOCKED by hand first)   u unlock   l lock
//   s status   e open the pairing window (60 s)   i toggle IMU print (find the hinge axis)
//   g print the lock's event log   v verify-time of the last login   x FACTORY RESET (keys, settings)
// Pairing without a computer: turn the knob 3 times within 10 s.
//
// TODO (after first bring-up): low-power (sleep between ticks, gyro only while moving, IMU
// wake-on-motion interrupt), CryptoCell (CC310) P-256 for faster logins, signed firmware updates.

#include <Arduino.h>

#include "ble_link.h"
#include "board_config.h"
#include "board_hal.h"
#include "slock/app.h"
#include "slock/intent.h"

using namespace slock;
using board::BleLink;

// ---------------------------------------------------------------- wiring

board::BoardMotor motor;
board::BoardEncoder encoder;
board::BoardHubSensor hub;
board::BoardCurrent current;
board::BoardBattery battery;
board::BoardDoor reed;  // validation only
board::BoardRandom rng;
board::BoardStorage storage;
board::BoardVerifier verifier;
board::Imu imu;
BleLink ble;

DriveConfig make_drive_config() {
  DriveConfig c;
  for (int i = 0; i < board::kMotorCount && i < DriveConfig::kMaxMotors; ++i) c.motor_cpd[i] = board::motor_cpd(i);
  c.slot_span_deg = 102.f;  // must match mech/params.scad: travel + 2 * deadband
  return c;
}

DriveController drive(motor, encoder, hub, current, make_drive_config());
LockController lock(drive, battery, LockConfig{});
KeyStore keys;
IntentDetector intent{IntentConfig{}};
uint8_t lock_id[kLockIdLen];
App* app = nullptr;

bool imu_print = false;

// The lock id is part of every signed login, so a signature for one lock can't open another.
// Derived from the chip's factory-programmed device id.
void make_lock_id(uint8_t* out) {
  uint32_t a = NRF_FICR->DEVICEID[0], b = NRF_FICR->DEVICEID[1];
  for (int i = 0; i < 4; ++i) {
    out[i] = static_cast<uint8_t>(a >> (8 * i));
    out[4 + i] = static_cast<uint8_t>(b >> (8 * i));
  }
}

const char* state_name(LockState s) {
  switch (s) {
    case LockState::Uncalibrated: return "uncalibrated";
    case LockState::Calibrating: return "calibrating";
    case LockState::Homing: return "homing";
    case LockState::NeedsHoming: return "needs-homing (turn bolt to an end)";
    case LockState::Locked: return "locked";
    case LockState::Unlocked: return "unlocked";
    case LockState::Between: return "between";
    case LockState::Unlocking: return "unlocking";
    case LockState::Locking: return "locking";
    case LockState::Jammed: return "JAMMED";
  }
  return "?";
}

void print_status() {
  const Calibration& cal = drive.calibration();
  Serial.printf("state=%s drive=%d hub=%.1fdeg progress=%.2f enc=%ld batt=%umV cur=%umA\n",
                state_name(lock.state()), static_cast<int>(lock.last_drive_status()), drive.hub_angle(),
                drive.hub_progress(), static_cast<long>(encoder.count()), battery.millivolts(),
                current.milliamps());
  if (cal.valid) {
    float ratio = cal.motor > 0 ? board::kGearboxRatios[cal.motor - 1] : 0.f;
    Serial.printf("  cal: unlock_dir=%d cpd=%.2f motor=1:%.0f locked=%.1f unlocked=%.1f span=%ld\n",
                  cal.unlock_dir, cal.cpd, ratio, cal.hub_locked, cal.hub_unlocked,
                  static_cast<long>(cal.stall_span));
  }
  const DoorEstimator& d = app->door();
  static const char* kDoor[] = {"unsure", "closed", "open"};
  Serial.printf("  door: %s%s angle=%.1f bias=%.2fdps%s  reed=%s  phones=%u keys=%u imu=%s\n",
                kDoor[static_cast<int>(d.state())], d.trusted() ? " (trusted)" : "", d.angle(), d.bias(),
                d.tamper() ? " TAMPER" : "", board::kHasDoorSensor ? (reed.closed() ? "closed" : "open") : "-",
                ble.connections(), keys.count(), imu.ok() ? "ok" : "MISSING");
}

void print_log() {
  Event ev[EventLog::kCapacity];
  uint16_t n = app->log().read(0, ev, EventLog::kCapacity);
  for (uint16_t i = 0; i < n; ++i) {
    Serial.printf("  #%lu t=%lus type=%u key=%d a=%u b=%u\n", static_cast<unsigned long>(ev[i].seq),
                  static_cast<unsigned long>(ev[i].t_ms / 1000), ev[i].type, ev[i].key, ev[i].a, ev[i].b);
  }
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(board::kAdcBits);
  motor.begin();
  encoder.begin();
  reed.begin();
  Wire.begin();
  if (!current.begin()) Serial.println("WARNING: INA219 not found; stall detection uses the encoder only");
  if (!imu.begin()) Serial.println("WARNING: IMU not found: no door state, no handle detection");
  if (!storage.begin()) Serial.println("WARNING: flash file system unavailable: nothing will be saved");

  make_lock_id(lock_id);
  static App app_instance(lock, keys, verifier, rng, battery, lock_id, AppConfig{}, &storage);
  app = &app_instance;
  ble.begin(*app, lock_id);  // starts the SoftDevice (the RNG needs it), then advertising
  BleLink::lock();
  app->begin(millis());  // loads keys, settings and the motor calibration, then homes
  BleLink::unlock();

  Serial.println("SLock v1. c=calibrate (bolt LOCKED first) u=unlock l=lock s=status e=pair i=imu g=log x=reset");
}

void handle_serial(Millis now) {
  if (!Serial.available()) return;
  int c = Serial.read();
  BleLink::lock();
  switch (c) {
    case 'c': Serial.printf("calibrate -> %d\n", static_cast<int>(lock.request_calibration(now))); break;
    case 'u': Serial.printf("unlock -> %d\n", static_cast<int>(lock.request_unlock(now))); break;
    case 'l': Serial.printf("lock -> %d\n", static_cast<int>(lock.request_lock(now))); break;
    case 's': print_status(); break;
    case 'e':
      app->open_enrollment(now, 60000);
      Serial.println("pairing window open for 60 s");
      break;
    case 'i': imu_print = !imu_print; break;
    case 'g': print_log(); break;
    case 'v': Serial.printf("last signature check took %lu ms\n", static_cast<unsigned long>(verifier.last_ms())); break;
    case 'x':
      storage.wipe();
      Serial.println("FACTORY RESET: flash wiped. Restarting.");
      delay(100);
      NVIC_SystemReset();
      break;
    default: break;
  }
  BleLink::unlock();
}

void loop() {
  Millis now = millis();
  BleLink::lock();
  app->tick(now);
  BleLink::unlock();

  // IMU at ~100 Hz: hinge-axis rotation for the door estimator, acceleration for handle turns.
  static Millis last_imu = 0;
  if (now - last_imu >= 10) {
    last_imu = now;
    board::Imu::Sample s;
    if (imu.read(s)) {
      float rate = board::kDoorGyroSign * s.gyro_dps[board::kDoorGyroAxis];
      float a = sqrtf(s.accel_mg[0] * s.accel_mg[0] + s.accel_mg[1] * s.accel_mg[1] + s.accel_mg[2] * s.accel_mg[2]);
      bool turned = intent.on_sample(static_cast<int16_t>(s.accel_mg[0]), static_cast<int16_t>(s.accel_mg[1]),
                                     static_cast<int16_t>(s.accel_mg[2]), now);
      BleLink::lock();
      app->on_imu(rate, fabsf(a - 1000.f), now);
      if (turned) app->on_intent(now);
      BleLink::unlock();
      if (imu_print) {
        Serial.printf("gyro %7.2f %7.2f %7.2f dps  accel %6.0f %6.0f %6.0f mg%s\n", s.gyro_dps[0], s.gyro_dps[1],
                      s.gyro_dps[2], s.accel_mg[0], s.accel_mg[1], s.accel_mg[2], turned ? "  HANDLE" : "");
      }
    }
  }

  ble.poll(now);

  static uint32_t last_changes = 0;
  if (lock.change_count() != last_changes) {
    last_changes = lock.change_count();
    BleLink::lock();
    print_status();
    BleLink::unlock();
  }

  handle_serial(now);
  delay(1);  // the drive needs a tick every 1-5 ms while moving
}
