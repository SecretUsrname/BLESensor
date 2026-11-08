// SLock v0 board layer: wires the portable core to the XIAO nRF52840 Sense.
//
// Compiles for xiaoblesense_adafruit (Adafruit nRF52 core); not yet run on hardware.
// Current scope is BENCH BRING-UP over USB serial, so the motor, sensors and calibration can be
// proven before any BLE work:
//   c = calibrate (turn the bolt to LOCKED by hand first)
//   u = unlock    l = lock    s = status
//
// TODO, in order:
//   1. Persist Calibration and KeyStore to flash (InternalFS / LittleFS).
//   2. BLE GATT service from slock/protocol.h (Bluefruit), feeding Session and PresenceTracker.
//   3. P-256 Verifier (micro-ecc + SHA-256) and the hardware TRNG for hal::Random.
//   4. IMU (LSM6DS3TR-C on the Sense) -> IntentDetector -> decide().
//   5. Low-power: sleep between ticks, wake on BLE / IMU / encoder.

#include <Adafruit_INA219.h>
#include <Arduino.h>
#include <Wire.h>

#include "board_config.h"
#include "slock/drive.h"
#include "slock/lock_controller.h"

using namespace slock;

// ---------------------------------------------------------------- HAL implementations

class BoardMotor : public hal::Motor {
 public:
  void begin() {
    pinMode(board::kMotorIn1, OUTPUT);
    pinMode(board::kMotorIn2, OUTPUT);
    brake();
  }
  void drive(int16_t duty) override {
    int pwm = constrain(abs(duty), 0, 1000) * 255 / 1000;
    if (duty > 0) {
      analogWrite(board::kMotorIn1, pwm);
      analogWrite(board::kMotorIn2, 0);
    } else if (duty < 0) {
      analogWrite(board::kMotorIn1, 0);
      analogWrite(board::kMotorIn2, pwm);
    } else {
      brake();
    }
  }
  void brake() override {
    analogWrite(board::kMotorIn1, 255);
    analogWrite(board::kMotorIn2, 255);
  }
};

volatile int32_t g_enc_count = 0;
volatile uint8_t g_enc_prev = 0;

void encoder_isr() {
  // Quadrature decode via a transition table: +1 / -1 / 0 for each (prev, now) pair.
  static const int8_t kTable[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  uint8_t now = static_cast<uint8_t>((digitalRead(board::kEncA) << 1) | digitalRead(board::kEncB));
  int8_t step = kTable[(g_enc_prev << 2) | now];
  g_enc_count += board::kEncoderInvert ? -step : step;
  g_enc_prev = now;
}

class BoardEncoder : public hal::Encoder {
 public:
  void begin() {
    pinMode(board::kEncA, INPUT_PULLUP);
    pinMode(board::kEncB, INPUT_PULLUP);
    g_enc_prev = static_cast<uint8_t>((digitalRead(board::kEncA) << 1) | digitalRead(board::kEncB));
    attachInterrupt(digitalPinToInterrupt(board::kEncA), encoder_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(board::kEncB), encoder_isr, CHANGE);
  }
  int32_t count() override {
    noInterrupts();
    int32_t c = g_enc_count;
    interrupts();
    return c;
  }
};

class BoardHubSensor : public hal::HubSensor {
 public:
  void read(int16_t& a, int16_t& b) override {
    a = static_cast<int16_t>(analogRead(board::kHallA) - board::kAdcMid);
    b = static_cast<int16_t>(analogRead(board::kHallB) - board::kAdcMid);
  }
};

class BoardCurrent : public hal::CurrentSense {
 public:
  bool begin() { return ina_.begin(); }
  uint16_t milliamps() override {
    float ma = fabsf(ina_.getCurrent_mA());
    return static_cast<uint16_t>(ma > 65535.f ? 65535.f : ma);
  }

 private:
  Adafruit_INA219 ina_{board::kIna219Addr};
};

class BoardBattery : public hal::Battery {
 public:
  uint16_t millivolts() override {
    float pin_mv = analogRead(board::kBattery) * board::kAdcRefMv / ((1 << board::kAdcBits) - 1);
    return static_cast<uint16_t>(pin_mv * board::kBatteryDivider);
  }
};

class BoardDoor : public hal::DoorSensor {
 public:
  void begin() { pinMode(board::kDoorReed, INPUT_PULLUP); }
  bool closed() override { return digitalRead(board::kDoorReed) == LOW; }
};

// ---------------------------------------------------------------- wiring

BoardMotor motor;
BoardEncoder encoder;
BoardHubSensor hub;
BoardCurrent current;
BoardBattery battery;
BoardDoor door;

DriveConfig make_drive_config() {
  DriveConfig c;
  c.nominal_cpd = board::kNominalCpd;
  c.slot_span_deg = 102.f;  // must match mech/params.scad: travel + 2 * deadband
  return c;
}

DriveController drive(motor, encoder, hub, current, make_drive_config());
LockController lock(drive, battery, board::kHasDoorSensor ? &door : nullptr, LockConfig{});

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
    Serial.printf("  cal: unlock_dir=%d cpd=%.2f (nominal %.2f) locked=%.1f unlocked=%.1f span=%ld\n",
                  cal.unlock_dir, cal.cpd, board::kNominalCpd, cal.hub_locked, cal.hub_unlocked,
                  static_cast<long>(cal.stall_span));
  }
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(board::kAdcBits);
  motor.begin();
  encoder.begin();
  door.begin();
  Wire.begin();
  if (!current.begin()) Serial.println("WARNING: INA219 not found; stall detection uses the encoder only");

  // TODO(1): load the saved Calibration here and drive.set_calibration(cal).
  lock.begin(millis());
  Serial.println("SLock bench firmware. c=calibrate (bolt LOCKED first)  u=unlock  l=lock  s=status");
}

void loop() {
  Millis now = millis();
  lock.tick(now);

  static uint32_t last_changes = 0;
  if (lock.change_count() != last_changes) {
    last_changes = lock.change_count();
    print_status();
    // TODO(1): after a successful calibration, save drive.calibration() to flash.
  }

  if (Serial.available()) {
    switch (Serial.read()) {
      case 'c': Serial.printf("calibrate -> %d\n", static_cast<int>(lock.request_calibration(now))); break;
      case 'u': Serial.printf("unlock -> %d\n", static_cast<int>(lock.request_unlock(now))); break;
      case 'l': Serial.printf("lock -> %d\n", static_cast<int>(lock.request_lock(now))); break;
      case 's': print_status(); break;
      default: break;
    }
  }
  delay(1);  // the drive needs a tick every 1-5 ms while moving
}
