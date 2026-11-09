#pragma once
// Hardware abstraction for the XIAO nRF52840 Sense: implements slock::hal (and the core's Verifier)
// on the real pins, flash, radio RNG and IMU.

#include <Adafruit_INA219.h>
#include <Adafruit_LittleFS.h>
#include <Arduino.h>
#include <InternalFileSystem.h>
#include <Wire.h>
#include <bluefruit.h>
#include <uECC.h>

#include "board_config.h"
#include "slock/hal.h"
#include "slock/session.h"
#include "slock/sha256.h"

namespace board {

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

inline volatile int32_t g_enc_count = 0;
inline volatile uint8_t g_enc_prev = 0;

inline void encoder_isr() {
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

// Optional reed switch, ONLY to validate the gyro door estimator on the bench (ground truth for
// logs). The lock itself never uses it.
class BoardDoor {
 public:
  void begin() { pinMode(board::kDoorReed, INPUT_PULLUP); }
  bool closed() { return digitalRead(board::kDoorReed) == LOW; }
};

// Cryptographic randomness from the nRF52840's hardware RNG, through the SoftDevice (Bluefruit
// owns the peripheral once it has started).
class BoardRandom : public hal::Random {
 public:
  void fill(uint8_t* buf, size_t len) override {
    while (len > 0) {
      uint8_t avail = 0;
      sd_rand_application_bytes_available_get(&avail);
      if (avail == 0) {
        delay(1);
        continue;
      }
      uint8_t n = static_cast<uint8_t>(avail < len ? avail : len);
      sd_rand_application_vector_get(buf, n);
      buf += n;
      len -= n;
    }
  }
};

// Records in the internal flash file system (LittleFS), one small file per record id.
class BoardStorage : public hal::Storage {
 public:
  bool begin() { return InternalFS.begin(); }
  bool load(uint16_t id, void* buf, size_t len) override {
    using namespace Adafruit_LittleFS_Namespace;
    char path[16];
    snprintf(path, sizeof(path), "/slock_%u", id);
    File f(InternalFS);
    if (!f.open(path, FILE_O_READ)) return false;
    bool ok = f.size() == len && f.read(buf, len) == static_cast<int>(len);
    f.close();
    return ok;
  }
  bool save(uint16_t id, const void* buf, size_t len) override {
    using namespace Adafruit_LittleFS_Namespace;
    char path[16];
    snprintf(path, sizeof(path), "/slock_%u", id);
    InternalFS.remove(path);  // FILE_O_WRITE appends
    File f(InternalFS);
    if (!f.open(path, FILE_O_WRITE)) return false;
    bool ok = f.write(static_cast<const uint8_t*>(buf), len) == len;
    f.close();
    return ok;
  }
  void wipe() { InternalFS.format(); }  // factory reset
};

// The phone signs SHA-256(message) with its P-256 key; micro-ecc checks it. The public key is x|y
// (64 bytes) and the signature r|s (64 bytes), the formats micro-ecc uses directly.
class BoardVerifier : public Verifier {
 public:
  bool verify(const uint8_t* pub, const uint8_t* msg, size_t len, const uint8_t* sig) override {
    uint8_t h[kSha256Len];
    sha256(msg, len, h);
    last_ms_ = millis();
    bool ok = uECC_verify(pub, h, sizeof(h), sig, uECC_secp256r1()) == 1;
    last_ms_ = millis() - last_ms_;
    return ok;
  }
  uint32_t last_ms() const { return last_ms_; }  // how long the last check took (bench info)

 private:
  uint32_t last_ms_ = 0;
};

// LSM6DS3TR-C: register-level, polled at about 100 Hz.
class Imu {
 public:
  struct Sample {
    float gyro_dps[3];
    float accel_mg[3];
  };

  bool begin() {
    pinMode(PIN_LSM6DS3TR_C_POWER, OUTPUT);
    digitalWrite(PIN_LSM6DS3TR_C_POWER, HIGH);
    delay(20);
    Wire1.begin();
    Wire1.setClock(400000);
    if (read8(0x0F) != 0x6A) return false;  // WHO_AM_I
    write8(0x12, 0x44);  // CTRL3_C: block data update, register auto-increment
    write8(0x10, 0x40);  // CTRL1_XL: 104 Hz, +-2 g
    write8(0x11, 0x40);  // CTRL2_G: 104 Hz, +-250 dps
    ok_ = true;
    return true;
  }

  bool read(Sample& s) {
    if (!ok_) return false;
    uint8_t b[12];
    Wire1.beginTransmission(kImuAddr);
    Wire1.write(0x22);  // OUTX_L_G: gyro x, y, z then accel x, y, z
    if (Wire1.endTransmission(false) != 0) return false;
    if (Wire1.requestFrom(kImuAddr, static_cast<uint8_t>(12)) != 12) return false;
    for (uint8_t& v : b) v = static_cast<uint8_t>(Wire1.read());
    for (int i = 0; i < 3; ++i) {
      s.gyro_dps[i] = static_cast<int16_t>(b[2 * i] | (b[2 * i + 1] << 8)) * 0.00875f;     // 8.75 mdps/LSB
      s.accel_mg[i] = static_cast<int16_t>(b[6 + 2 * i] | (b[7 + 2 * i] << 8)) * 0.061f;  // 0.061 mg/LSB
    }
    return true;
  }

  bool ok() const { return ok_; }

 private:
  uint8_t read8(uint8_t reg) {
    Wire1.beginTransmission(kImuAddr);
    Wire1.write(reg);
    Wire1.endTransmission(false);
    Wire1.requestFrom(kImuAddr, static_cast<uint8_t>(1));
    return Wire1.available() ? static_cast<uint8_t>(Wire1.read()) : 0;
  }
  void write8(uint8_t reg, uint8_t v) {
    Wire1.beginTransmission(kImuAddr);
    Wire1.write(reg);
    Wire1.write(v);
    Wire1.endTransmission();
  }
  bool ok_ = false;
};

}  // namespace board
