#include "session.h"

#include <cstring>

namespace slock {

int KeyStore::add(const uint8_t* pubkey) {
  for (uint8_t i = 0; i < kCapacity; ++i) {
    if (!keys_[i].used) {
      keys_[i].used = true;
      std::memcpy(keys_[i].pubkey, pubkey, kPubKeyLen);
      return i;
    }
  }
  return -1;
}

bool KeyStore::remove(uint8_t id) {
  if (id >= kCapacity || !keys_[id].used) return false;
  keys_[id] = KeyRecord{};
  return true;
}

const KeyRecord* KeyStore::get(uint8_t id) const {
  if (id >= kCapacity || !keys_[id].used) return nullptr;
  return &keys_[id];
}

uint8_t KeyStore::count() const {
  uint8_t n = 0;
  for (const auto& k : keys_) {
    if (k.used) ++n;
  }
  return n;
}

void Session::bind(const KeyStore* keys, Verifier* verifier, hal::Random* rng, const uint8_t* lock_id,
                   const SessionConfig& cfg, AuthGuard* shared_guard) {
  keys_ = keys;
  verifier_ = verifier;
  rng_ = rng;
  std::memcpy(lock_id_, lock_id, kLockIdLen);
  cfg_ = cfg;
  guard_ = shared_guard;
}

void Session::build_message(const uint8_t* lock_id, const uint8_t* nonce, uint8_t* out) {
  std::memcpy(out, "SLK1", 4);
  std::memcpy(out + 4, lock_id, kLockIdLen);
  std::memcpy(out + 4 + kLockIdLen, nonce, kNonceLen);
}

void Session::build_message_v2(const uint8_t* lock_id, const uint8_t* nonce, const uint8_t* ctx, uint8_t* out) {
  std::memcpy(out, "SLK2", 4);
  std::memcpy(out + 4, lock_id, kLockIdLen);
  std::memcpy(out + 4 + kLockIdLen, nonce, kNonceLen);
  std::memcpy(out + 4 + kLockIdLen + kNonceLen, ctx, kContextLen);
}

PhoneContext Session::decode_context(const uint8_t* ctx) {
  PhoneContext c;
  c.provided = true;
  c.has_home = (ctx[0] & 1) != 0;
  c.inside_home = (ctx[0] & 2) != 0;
  c.minutes_since_entry = ctx[1];
  return c;
}

const uint8_t* Session::new_challenge() {
  rng_->fill(nonce_, kNonceLen);
  have_nonce_ = true;
  auth_ = false;
  key_id_ = -1;
  return nonce_;
}

void Session::fail(Millis now) {
  AuthGuard& g = guard();
  if (g.failures < 255) ++g.failures;
  if (g.failures >= cfg_.max_failures) g.locked_at = now;
}

AuthResult Session::check(uint8_t key_id, const uint8_t* msg, size_t len, const uint8_t* sig, Millis now) {
  AuthGuard& g = guard();
  if (g.failures >= cfg_.max_failures) {
    if (since(now, g.locked_at) < cfg_.lockout_ms) return AuthResult::LockedOut;
    g.failures = 0;
  }
  const KeyRecord* key = keys_->get(key_id);
  if (key == nullptr) {
    fail(now);
    return AuthResult::UnknownKey;
  }
  if (!verifier_->verify(key->pubkey, msg, len, sig)) {
    fail(now);
    return AuthResult::BadSignature;
  }
  g.failures = 0;
  auth_ = true;
  auth_at_ = now;
  key_id_ = key_id;
  return AuthResult::Ok;
}

AuthResult Session::respond(uint8_t key_id, const uint8_t* sig, Millis now) {
  if (guard().failures >= cfg_.max_failures && since(now, guard().locked_at) < cfg_.lockout_ms) {
    return AuthResult::LockedOut;
  }
  if (!have_nonce_) return AuthResult::NoChallenge;
  have_nonce_ = false;  // single use, whatever the outcome
  uint8_t msg[kAuthMsgLen];
  build_message(lock_id_, nonce_, msg);
  AuthResult r = check(key_id, msg, kAuthMsgLen, sig, now);
  if (r == AuthResult::Ok) context_ = PhoneContext{};
  return r;
}

AuthResult Session::respond_with_context(uint8_t key_id, const uint8_t* ctx, const uint8_t* sig, Millis now) {
  if (guard().failures >= cfg_.max_failures && since(now, guard().locked_at) < cfg_.lockout_ms) {
    return AuthResult::LockedOut;
  }
  if (!have_nonce_) return AuthResult::NoChallenge;
  have_nonce_ = false;
  uint8_t msg[kAuthMsgLenV2];
  build_message_v2(lock_id_, nonce_, ctx, msg);
  AuthResult r = check(key_id, msg, kAuthMsgLenV2, sig, now);
  if (r == AuthResult::Ok) context_ = decode_context(ctx);
  return r;
}

bool Session::authenticated(Millis now) const {
  return auth_ && since(now, auth_at_) < cfg_.session_ms;
}

void Session::end() {
  auth_ = false;
  have_nonce_ = false;
  key_id_ = -1;
  context_ = PhoneContext{};
  std::memset(nonce_, 0, sizeof(nonce_));
}

}  // namespace slock
