#pragma once
// The lock's own record of what happened (docs/v1-framework.md section 11). About 100 events in
// RAM; phones page through it with the ReadLog command. Times are lock uptime in milliseconds:
// the status packet carries the current uptime so a phone can convert them to wall-clock time.

#include "types.h"

namespace slock {

enum class EventType : uint8_t {
  Boot = 1,
  AutoUnlock = 2,     // key = phone
  AppUnlock = 3,      // key = phone
  AppLock = 4,        // key = phone
  AutoRelock = 5,     // a = 0 after a trusted close, 1 because the door wasn't opened
  BoltTurned = 6,     // by the knob or the key; a = 1 now unlocked, 0 now locked
  SideChanged = 7,    // key = phone, a = Side, b = SideReason
  Tamper = 8,         // the door rotated while locked
  FalseLock = 9,      // the bolt was thrown while the door was open
  Jam = 10,
  Enrolled = 11,      // key = new key
  Revoked = 12,       // key = revoked key, b = revoking phone
  LoginFailed = 13,   // a = AuthResult
  DoorCalibrated = 14,  // a = wizard step, b = value (dBm or dB)
};

#pragma pack(push, 1)
struct Event {
  uint32_t seq = 0;
  uint32_t t_ms = 0;
  uint8_t type = 0;
  int8_t key = -1;
  uint8_t a = 0;
  uint8_t b = 0;
};
static_assert(sizeof(Event) == 12, "Event is a wire format");
#pragma pack(pop)

class EventLog {
 public:
  static constexpr uint16_t kCapacity = 100;

  void add(EventType type, Millis now, int key = -1, uint8_t a = 0, uint8_t b = 0);
  // Copies up to max events with seq >= from_seq (oldest still held first). Returns the count.
  uint16_t read(uint32_t from_seq, Event* out, uint16_t max) const;
  uint32_t next_seq() const { return next_; }
  uint16_t size() const { return count_; }
  const Event* latest() const;

 private:
  Event ev_[kCapacity];
  uint16_t head_ = 0;  // where the next event goes
  uint16_t count_ = 0;
  uint32_t next_ = 1;
};

}  // namespace slock
