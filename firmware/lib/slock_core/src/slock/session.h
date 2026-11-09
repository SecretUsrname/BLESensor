#pragma once
// Phone authentication: challenge-response with per-phone P-256 keys.
//
//   lock  -> phone : lock_id | nonce (16 random bytes, new on every connection, single use)
//   phone -> lock  : v1: key_id | sig over "SLK1" | lock_id | nonce
//                    v2: key_id | ctx(2) | sig over "SLK2" | lock_id | nonce | ctx(2)
//
// v2 adds the phone's HOME CONTEXT, signed together with the challenge so it can't be altered or
// replayed: ctx[0] bit0 = phone has a home geofence set up (and permission to monitor it),
//           ctx[0] bit1 = phone is inside the home geofence now,
//           ctx[1]      = minutes since the phone last ENTERED the geofence (255 = unknown).
// A relay attacker can forward radio packets but can't make your phone believe it is home.
//
// P-256 because the iPhone Secure Enclave signs with it; the check sits behind Verifier
// (device: micro-ecc + SHA-256, TODO in the board layer; twin: Python cryptography; tests: a fake).

#include "hal.h"

namespace slock {

constexpr size_t kNonceLen = 16;
constexpr size_t kLockIdLen = 8;
constexpr size_t kPubKeyLen = 64;  // uncompressed P-256 point without the 0x04 prefix
constexpr size_t kSigLen = 64;     // r | s
constexpr size_t kContextLen = 2;
constexpr size_t kAuthMsgLen = 4 + kLockIdLen + kNonceLen;                  // v1
constexpr size_t kAuthMsgLenV2 = 4 + kLockIdLen + kNonceLen + kContextLen;  // v2

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
  uint32_t session_ms = 120000;  // a login is good for this long (the App renews it on live links)
  uint8_t max_failures = 5;
  uint32_t lockout_ms = 60000;   // refuse all attempts for this long after max_failures
};

enum class AuthResult : uint8_t { Ok, NoChallenge, UnknownKey, BadSignature, LockedOut };

// What the phone said about itself, signed with its login.
struct PhoneContext {
  bool provided = false;           // the phone used the v2 login
  bool has_home = false;
  bool inside_home = false;
  uint8_t minutes_since_entry = 255;
};

// Failure counter shared by all connections, so extra connections don't buy extra guesses.
struct AuthGuard {
  uint8_t failures = 0;
  Millis locked_at = 0;
};

class Session {
 public:
  Session() = default;
  Session(const KeyStore& keys, Verifier& verifier, hal::Random& rng, const uint8_t* lock_id,
          const SessionConfig& cfg) {
    bind(&keys, &verifier, &rng, lock_id, cfg, nullptr);
  }
  void bind(const KeyStore* keys, Verifier* verifier, hal::Random* rng, const uint8_t* lock_id,
            const SessionConfig& cfg, AuthGuard* shared_guard);

  const uint8_t* new_challenge();  // call on every new connection (and to renew a login)
  AuthResult respond(uint8_t key_id, const uint8_t* sig, Millis now);  // v1
  AuthResult respond_with_context(uint8_t key_id, const uint8_t* ctx, const uint8_t* sig, Millis now);  // v2
  bool authenticated(Millis now) const;
  int key_id() const { return auth_ ? key_id_ : -1; }
  const PhoneContext& context() const { return context_; }
  void end();  // call on disconnect

  static void build_message(const uint8_t* lock_id, const uint8_t* nonce, uint8_t* out);
  static void build_message_v2(const uint8_t* lock_id, const uint8_t* nonce, const uint8_t* ctx, uint8_t* out);
  static PhoneContext decode_context(const uint8_t* ctx);

 private:
  AuthResult check(uint8_t key_id, const uint8_t* msg, size_t len, const uint8_t* sig, Millis now);
  void fail(Millis now);
  AuthGuard& guard() { return guard_ != nullptr ? *guard_ : own_guard_; }

  const KeyStore* keys_ = nullptr;
  Verifier* verifier_ = nullptr;
  hal::Random* rng_ = nullptr;
  uint8_t lock_id_[kLockIdLen] = {};
  SessionConfig cfg_;
  AuthGuard own_guard_;
  AuthGuard* guard_ = nullptr;
  uint8_t nonce_[kNonceLen] = {};
  bool have_nonce_ = false;
  bool auth_ = false;
  Millis auth_at_ = 0;
  int key_id_ = -1;
  PhoneContext context_;
};

}  // namespace slock
