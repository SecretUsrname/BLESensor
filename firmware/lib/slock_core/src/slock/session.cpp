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

Session::Session(const KeyStore& keys, Verifier& verifier, hal::Random& rng, const uint8_t* lock_id,
                 const SessionConfig& cfg)
    : keys_(keys), verifier_(verifier), rng_(rng), cfg_(cfg) {
  std::memcpy(lock_id_, lock_id, kLockIdLen);
}

void Session::build_message(const uint8_t* lock_id, const uint8_t* nonce, uint8_t* out) {
  std::memcpy(out, "SLK1", 4);
  std::memcpy(out + 4, lock_id, kLockIdLen);
  std::memcpy(out + 4 + kLockIdLen, nonce, kNonceLen);
}

const uint8_t* Session::new_challenge() {
  rng_.fill(nonce_, kNonceLen);
  have_nonce_ = true;
  auth_ = false;
  key_id_ = -1;
  return nonce_;
}

void Session::fail(Millis now) {
  if (failures_ < 255) ++failures_;
  if (failures_ >= cfg_.max_failures) locked_at_ = now;
}

AuthResult Session::respond(uint8_t key_id, const uint8_t* sig, Millis now) {
  if (failures_ >= cfg_.max_failures) {
    if (since(now, locked_at_) < cfg_.lockout_ms) return AuthResult::LockedOut;
    failures_ = 0;
  }
  if (!have_nonce_) return AuthResult::NoChallenge;
  have_nonce_ = false;  // single use, whatever the outcome

  const KeyRecord* key = keys_.get(key_id);
  if (key == nullptr) {
    fail(now);
    return AuthResult::UnknownKey;
  }
  uint8_t msg[kAuthMsgLen];
  build_message(lock_id_, nonce_, msg);
  if (!verifier_.verify(key->pubkey, msg, kAuthMsgLen, sig)) {
    fail(now);
    return AuthResult::BadSignature;
  }
  failures_ = 0;
  auth_ = true;
  auth_at_ = now;
  key_id_ = key_id;
  return AuthResult::Ok;
}

bool Session::authenticated(Millis now) const {
  return auth_ && since(now, auth_at_) < cfg_.session_ms;
}

void Session::end() {
  auth_ = false;
  have_nonce_ = false;
  key_id_ = -1;
  std::memset(nonce_, 0, sizeof(nonce_));
}

}  // namespace slock
