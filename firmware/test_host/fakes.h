#pragma once
// Shared fakes for host tests.

#include <cstring>

#include "slock/hal.h"
#include "slock/session.h"

namespace fakes {

// Stand-in for ECDSA: sig[i] = msg[i % len] ^ pubkey[i]. Only the protocol logic is under test.
inline void fake_sign(const uint8_t* pub, const uint8_t* msg, size_t len, uint8_t* sig) {
  for (size_t i = 0; i < slock::kSigLen; ++i) sig[i] = msg[i % len] ^ pub[i];
}

struct FakeVerifier : slock::Verifier {
  bool verify(const uint8_t* pub, const uint8_t* msg, size_t len, const uint8_t* sig) override {
    uint8_t expect[slock::kSigLen];
    fake_sign(pub, msg, len, expect);
    return std::memcmp(expect, sig, slock::kSigLen) == 0;
  }
};

struct CountingRng : slock::hal::Random {
  uint8_t next = 1;
  void fill(uint8_t* buf, size_t len) override {
    for (size_t i = 0; i < len; ++i) buf[i] = next++;
  }
};

struct FakeBattery : slock::hal::Battery {
  uint16_t mv = 6000;
  uint16_t millivolts() override { return mv; }
};

struct FakeDoor : slock::hal::DoorSensor {
  bool is_closed = true;
  bool closed() override { return is_closed; }
};

}  // namespace fakes
