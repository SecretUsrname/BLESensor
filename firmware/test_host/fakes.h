#pragma once
// Shared fakes for host tests.

#include <cstring>
#include <map>
#include <vector>

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

// In-memory stand-in for flash.
struct MemStorage : slock::hal::Storage {
  std::map<uint16_t, std::vector<uint8_t>> rec;
  bool load(uint16_t id, void* buf, size_t len) override {
    auto it = rec.find(id);
    if (it == rec.end() || it->second.size() != len) return false;
    std::memcpy(buf, it->second.data(), len);
    return true;
  }
  bool save(uint16_t id, const void* buf, size_t len) override {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    rec[id] = std::vector<uint8_t>(p, p + len);
    return true;
  }
};

}  // namespace fakes
