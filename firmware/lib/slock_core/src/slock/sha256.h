#pragma once
// SHA-256 (FIPS 180-4), for the P-256 signature check: the phone signs SHA-256(message). Portable
// and small; tested against the standard vectors on the PC.

#include "types.h"

namespace slock {

constexpr size_t kSha256Len = 32;

class Sha256 {
 public:
  Sha256() { reset(); }
  void reset();
  void update(const uint8_t* data, size_t len);
  void finish(uint8_t out[kSha256Len]);

 private:
  void block(const uint8_t* p);
  uint32_t h_[8];
  uint8_t buf_[64];
  size_t used_ = 0;
  uint64_t total_ = 0;
};

inline void sha256(const uint8_t* data, size_t len, uint8_t out[kSha256Len]) {
  Sha256 s;
  s.update(data, len);
  s.finish(out);
}

}  // namespace slock
