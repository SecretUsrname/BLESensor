// SHA-256 against the FIPS 180-2 / NIST example vectors.

#include <cstring>
#include <string>

#include "check.h"
#include "slock/sha256.h"

using namespace slock;

namespace {
std::string hex(const uint8_t* d, size_t n) {
  static const char* k = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    s += k[d[i] >> 4];
    s += k[d[i] & 15];
  }
  return s;
}
std::string digest(const std::string& m) {
  uint8_t out[kSha256Len];
  sha256(reinterpret_cast<const uint8_t*>(m.data()), m.size(), out);
  return hex(out, kSha256Len);
}
}  // namespace

TEST(sha256_standard_vectors) {
  CHECK(digest("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(digest("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(digest(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(sha256_streaming_matches_one_shot) {
  std::string m = "SLK3 lock id, nonce and an 11-byte context: 39 bytes or so";
  Sha256 s;
  for (char c : m) s.update(reinterpret_cast<const uint8_t*>(&c), 1);  // byte by byte
  uint8_t a[kSha256Len], b[kSha256Len];
  s.finish(a);
  sha256(reinterpret_cast<const uint8_t*>(m.data()), m.size(), b);
  CHECK(std::memcmp(a, b, kSha256Len) == 0);
}
