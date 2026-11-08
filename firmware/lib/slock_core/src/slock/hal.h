#pragma once
// Hardware abstraction. The board layer (src/) implements these for the XIAO nRF52840;
// the host tests implement them with a simulated mechanism.

#include "types.h"

namespace slock {
namespace hal {

class Motor {
 public:
  virtual ~Motor() = default;
  // duty in [-1000, 1000]. The sign is the raw motor direction, not lock/unlock:
  // positive duty must make Encoder::count() increase (flip it in the board layer if not).
  virtual void drive(int16_t duty) = 0;
  virtual void brake() = 0;
};

class Encoder {
 public:
  virtual ~Encoder() = default;
  virtual int32_t count() = 0;
};

// Two linear hall channels 90 degrees apart around the hub magnet, centred so 0 = no field.
class HubSensor {
 public:
  virtual ~HubSensor() = default;
  virtual void read(int16_t& a, int16_t& b) = 0;
};

class CurrentSense {
 public:
  virtual ~CurrentSense() = default;
  virtual uint16_t milliamps() = 0;
};

class Battery {
 public:
  virtual ~Battery() = default;
  virtual uint16_t millivolts() = 0;
};

// Optional door-closed sensor (reed switch on the frame).
class DoorSensor {
 public:
  virtual ~DoorSensor() = default;
  virtual bool closed() = 0;
};

// Must be a cryptographic RNG on the device (nRF52840 has a hardware TRNG).
class Random {
 public:
  virtual ~Random() = default;
  virtual void fill(uint8_t* buf, size_t len) = 0;
};

}  // namespace hal
}  // namespace slock
