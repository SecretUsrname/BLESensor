#pragma once
// Pin map and hardware constants for the v0 bench prototype (XIAO nRF52840 Sense).
// [UNVERIFIED] values must be checked against the parts actually bought.

#include <Arduino.h>

namespace board {

// DRV8871 motor driver: PWM one input, hold the other low; both high = brake.
constexpr uint8_t kMotorIn1 = D9;
constexpr uint8_t kMotorIn2 = D10;

// N20 hall encoder, channels A/B (UART pins are free: USB serial is used for logs).
constexpr uint8_t kEncA = D6;
constexpr uint8_t kEncB = D7;
constexpr bool kEncoderInvert = false;  // flip if counts fall when the motor is driven positive

// DRV5055 linear halls (ratiometric: 0 field = VCC/2).
constexpr uint8_t kHallA = A0;
constexpr uint8_t kHallB = A1;
constexpr int kAdcBits = 12;
constexpr int kAdcMid = 2048;

// Battery: 4xAA through a 100k / 47k divider (6.4 V max -> 2.05 V at the pin).
constexpr uint8_t kBattery = A2;
constexpr float kBatteryDivider = (100.f + 47.f) / 47.f;  // [UNVERIFIED] match the resistors fitted
constexpr float kAdcRefMv = 3600.f;                       // nRF52 default analog reference

// Optional reed switch to GND on the door frame (closed door = magnet near = LOW). Validation only:
// ground truth while testing the gyro door estimator.
constexpr bool kHasDoorSensor = false;
constexpr uint8_t kDoorReed = D8;

// INA219 current sensor on I2C (D4 = SDA, D5 = SCL), default address.
constexpr uint8_t kIna219Addr = 0x40;

// Gearmotors on hand (all N20 with a hall encoder). Calibration auto-detects which one is fitted
// from the measured encoder counts per output-gear degree:
// 7 PPR magnetic encoder x 4 (quadrature) x gearbox ratio x spur ratio (40/12) / 360.
constexpr float kGearboxRatios[] = {100.f, 150.f, 300.f};  // reported as motor 1, 2, 3
constexpr int kMotorCount = 3;
constexpr float motor_cpd(int i) { return 7.f * 4.f * kGearboxRatios[i] * (40.f / 12.f) / 360.f; }

}  // namespace board
