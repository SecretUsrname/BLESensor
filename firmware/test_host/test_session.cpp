#include <cstring>

#include "check.h"
#include "fakes.h"
#include "slock/session.h"

using namespace slock;
using fakes::CountingRng;
using fakes::fake_sign;
using fakes::FakeVerifier;

namespace {

const uint8_t kLockId[kLockIdLen] = {1, 2, 3, 4, 5, 6, 7, 8};

struct Rig {
  KeyStore keys;
  FakeVerifier ver;
  CountingRng rng;
  Session s{keys, ver, rng, kLockId, SessionConfig{}};
  uint8_t pub[kPubKeyLen];
  int id = -1;
  Rig() {
    for (size_t i = 0; i < kPubKeyLen; ++i) pub[i] = static_cast<uint8_t>(0xA0 + i);
    id = keys.add(pub);
  }
  void sign(const uint8_t* nonce, uint8_t* sig, const uint8_t* lock_id = kLockId) {
    uint8_t msg[kAuthMsgLen];
    Session::build_message(lock_id, nonce, msg);
    fake_sign(pub, msg, kAuthMsgLen, sig);
  }
};

}  // namespace

TEST(auth_happy_path) {
  Rig r;
  CHECK(r.id == 0);
  uint8_t sig[kSigLen];
  r.sign(r.s.new_challenge(), sig);
  CHECK(r.s.respond(0, sig, 1000) == AuthResult::Ok);
  CHECK(r.s.authenticated(1000));
  CHECK(r.s.key_id() == 0);
}

TEST(auth_rejects_unknown_key_and_bad_signature) {
  Rig r;
  uint8_t sig[kSigLen];
  r.sign(r.s.new_challenge(), sig);
  CHECK(r.s.respond(5, sig, 0) == AuthResult::UnknownKey);

  r.sign(r.s.new_challenge(), sig);
  sig[10] ^= 1;
  CHECK(r.s.respond(0, sig, 0) == AuthResult::BadSignature);
  CHECK(!r.s.authenticated(0));
}

TEST(auth_signature_is_bound_to_this_lock) {
  Rig r;
  const uint8_t other_lock[kLockIdLen] = {9, 9, 9, 9, 9, 9, 9, 9};
  uint8_t sig[kSigLen];
  r.sign(r.s.new_challenge(), sig, other_lock);
  CHECK(r.s.respond(0, sig, 0) == AuthResult::BadSignature);
}

TEST(auth_nonce_is_single_use) {
  Rig r;
  uint8_t sig[kSigLen];
  r.sign(r.s.new_challenge(), sig);
  CHECK(r.s.respond(0, sig, 0) == AuthResult::Ok);
  CHECK(r.s.respond(0, sig, 1) == AuthResult::NoChallenge);  // replaying the same answer

  // A recorded answer is useless on the next connection: the nonce is different.
  r.s.end();
  r.s.new_challenge();
  CHECK(r.s.respond(0, sig, 2) == AuthResult::BadSignature);
}

TEST(auth_lockout_after_repeated_failures) {
  Rig r;
  uint8_t bad[kSigLen] = {};
  for (int i = 0; i < 5; ++i) {
    r.s.new_challenge();
    CHECK(r.s.respond(0, bad, 100) == AuthResult::BadSignature);
  }
  uint8_t sig[kSigLen];
  r.sign(r.s.new_challenge(), sig);
  CHECK(r.s.respond(0, sig, 200) == AuthResult::LockedOut);  // even a correct answer

  r.sign(r.s.new_challenge(), sig);
  CHECK(r.s.respond(0, sig, 100 + 60000) == AuthResult::Ok);  // lockout over
}

TEST(auth_session_expires_and_ends_on_disconnect) {
  Rig r;
  uint8_t sig[kSigLen];
  r.sign(r.s.new_challenge(), sig);
  CHECK(r.s.respond(0, sig, 0) == AuthResult::Ok);
  CHECK(r.s.authenticated(119999));
  CHECK(!r.s.authenticated(120000));

  r.sign(r.s.new_challenge(), sig);
  CHECK(r.s.respond(0, sig, 0) == AuthResult::Ok);
  r.s.end();
  CHECK(!r.s.authenticated(1));
}

TEST(keystore_add_remove_capacity) {
  KeyStore ks;
  uint8_t pub[kPubKeyLen] = {};
  for (int i = 0; i < KeyStore::kCapacity; ++i) CHECK(ks.add(pub) == i);
  CHECK(ks.add(pub) == -1);
  CHECK(ks.remove(3));
  CHECK(!ks.remove(3));
  CHECK(ks.get(3) == nullptr);
  CHECK(ks.add(pub) == 3);
  CHECK(ks.count() == KeyStore::kCapacity);
}
