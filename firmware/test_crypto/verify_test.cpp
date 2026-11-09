// The board's signature check (BoardVerifier in src/board_hal.h: SHA-256 from the core, then
// micro-ecc's uECC_verify on secp256r1) against signatures made by a standard library.
#include <cstdio>

#include "slock/sha256.h"
#include "uECC.h"
#include "vectors.h"

int main() {
  int failures = 0, n = 0;
  for (const Vector& v : kVectors) {
    uint8_t h[slock::kSha256Len];
    slock::sha256(v.msg, sizeof(v.msg), h);
    int ok = uECC_verify(v.pub, h, sizeof(h), v.sig, uECC_secp256r1()) == 1 ? 1 : 0;
    if (ok != v.valid) {
      std::printf("FAIL vector %d: expected %s\n", n, v.valid ? "valid" : "invalid");
      ++failures;
    }
    ++n;
  }
  std::printf("%d signature vectors, %d failed\n", n, failures);
  return failures == 0 ? 0 : 1;
}
