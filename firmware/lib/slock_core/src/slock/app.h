#pragma once
// Application glue: connects BLE events, authentication, presence, the unlock policy and the
// lock state machine. The board's BLE layer and the PC digital twin both drive this same class,
// so what the twin shows is what the firmware does.

#include "lock_controller.h"
#include "policy.h"
#include "presence.h"
#include "protocol.h"
#include "session.h"

namespace slock {

struct AppConfig {
  UnlockMode mode = UnlockMode::ApproachAndIntent;
  uint32_t intent_hold_ms = 1500;  // a handle touch stays valid this long
};

enum class CmdResult : uint8_t { Ok, NotAuthenticated, BadCommand, Busy, NotReady, AlreadyThere, LowBattery };

class App {
 public:
  App(LockController& lock, Session& session, PresenceTracker& presence, KeyStore& keys,
      hal::Battery& battery, const AppConfig& cfg);

  void begin(Millis now);
  void tick(Millis now);  // call every 1-5 ms

  // BLE link events
  const uint8_t* on_connect(Millis now);  // returns the challenge nonce to publish
  void on_disconnect(Millis now);
  AuthResult on_auth_response(const uint8_t* data, size_t len, Millis now);  // key_id | sig(64)
  CmdResult on_command(const uint8_t* data, size_t len, Millis now);        // opcode | args
  void on_rssi(int8_t dbm, Millis now);
  int on_enroll(const uint8_t* pubkey, size_t len, Millis now);  // new key id, or -1

  // Physical inputs
  void on_intent(Millis now);                       // IntentDetector fired
  void open_enrollment(Millis now, uint32_t ms);    // e.g. after a knob gesture on the lock

  bool enrollment_open(Millis now) const;
  bool connected() const { return connected_; }
  UnlockMode mode() const { return cfg_.mode; }
  void set_mode(UnlockMode m) { cfg_.mode = m; }
  uint32_t auto_unlocks() const { return auto_unlocks_; }
  protocol::StatusPacket status(Millis now) const;

 private:
  LockController& lock_;
  Session& session_;
  PresenceTracker& presence_;
  KeyStore& keys_;
  hal::Battery& battery_;
  AppConfig cfg_;
  bool connected_ = false;
  bool intent_pending_ = false;
  Millis intent_at_ = 0;
  bool enroll_open_ = false;
  Millis enroll_at_ = 0;
  uint32_t enroll_ms_ = 0;
  uint32_t auto_unlocks_ = 0;
  CmdResult last_cmd_ = CmdResult::Ok;
  AuthResult last_auth_ = AuthResult::NoChallenge;
  int last_enrolled_ = -1;
};

}  // namespace slock
