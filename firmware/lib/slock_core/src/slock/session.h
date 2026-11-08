#pragma once
// Phone authentication: challenge-response with per-phone P-256 keys.
//
//   lock  -> phone : nonce (16 random bytes, new on every connection, single use)
//   phone -> lock  : key_id, ECDSA-P256 signature over  "SLK1" | lock_id (8) | nonce (16)
//
// P-256 because that is what the iPhone Secure Enclave signs with, so the private key never
// leaves the phone. The signature check itself sits behind Verifier: the device uses micro-ecc +
// SHA-256 (TODO in the board layer); tests use a fake.
//
// The BLE link must also be bonded with LE Secure Connections. A later version should add a
// per-command MAC with a session key; see docs/04-ble-protocol.md (to be written).

#include "hal.h"

namespace slock {

constexpr size_t kNonceLen = 16;
constexpr size_t kLockIdLen = 8;
constexpr size_t kPubKeyLen = 64;  // uncompressed P-256 point without the 0x04 prefix
constexpr size_t kSigLen = 64;     // r | s
constexpr size_t kAuthMsgLen = 4 + kLockIdLen + kNonceLen;

class Verifier {
 public:
  virtual ~Verifier() = default;
  virtual bool verify(const uint8_t* pubkey, const uint8_t* msg, size_t len, const uint8_t* sig) = 0;
};

struct KeyRecord {
  bool used = false;
  uint8_t pubkey[kPubKeyLen] = {};
};

class KeyStore {
 public:
  static constexpr uint8_t kCapacity = 8;
  int add(const uint8_t* pubkey);  // returns the new key id, or -1 if full
  bool remove(uint8_t id);
  const KeyRecord* get(uint8_t id) const;
  uint8_t count() const;

 private:
  KeyRecord keys_[kCapacity];
};

struct SessionConfig {
  uint32_t session_ms = 120000;  // an authentication is good for this long within one connection
  uint8_t max_failures = 5;
  uint32_t lockout_ms = 60000;   // refuse all attempts for this long after max_failures
};

enum class AuthResult : uint8_t { Ok, NoChallenge, UnknownKey, BadSignature, LockedOut };

class Session {
 public:
  Session(const KeyStore& keys, Verifier& verifier, hal::Random& rng, const uint8_t* lock_id,
          const SessionConfig& cfg);

  const uint8_t* new_challenge();  // call on every new connection
  AuthResult respond(uint8_t key_id, const uint8_t* sig, Millis now);
  bool authenticated(Millis now) const;
  int key_id() const { return auth_ ? key_id_ : -1; }
  void end();                      // call on disconnect

  static void build_message(const uint8_t* lock_id, const uint8_t* nonce, uint8_t* out);

 private:
  void fail(Millis now);

  const KeyStore& keys_;
  Verifier& verifier_;
  hal::Random& rng_;
  uint8_t lock_id_[kLockIdLen];
  SessionConfig cfg_;
  uint8_t nonce_[kNonceLen] = {};
  bool have_nonce_ = false;
  bool auth_ = false;
  Millis auth_at_ = 0;
  int key_id_ = -1;
  uint8_t failures_ = 0;  // survives reconnects on purpose
  Millis locked_at_ = 0;
};

}  // namespace slock
