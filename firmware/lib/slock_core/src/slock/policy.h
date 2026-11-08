#pragma once
// When to unlock automatically. Kept as a pure function so every rule is unit-testable.
//
// Mode A (ApproachOnly):        authenticated + genuine arrival + phone Close            -> unlock
// Mode B (ApproachAndIntent):   authenticated + genuine arrival + phone Near or Close
//                               + handle intent detected                                -> unlock
// Both modes also require the bolt to be Locked (nothing to do otherwise).
// The caller must call PresenceTracker::consume_arrival() after acting on Unlock.

#include "presence.h"

namespace slock {

enum class UnlockMode : uint8_t { ApproachOnly, ApproachAndIntent };

struct PolicyInputs {
  bool authenticated = false;
  bool arrival = false;   // PresenceTracker::arrival_active()
  Zone zone = Zone::Gone;
  bool intent = false;    // IntentDetector fired this tick
  bool locked = false;
};

enum class Decision : uint8_t { None, Unlock };

inline Decision decide(UnlockMode mode, const PolicyInputs& in) {
  if (!in.authenticated || !in.arrival || !in.locked) return Decision::None;
  switch (mode) {
    case UnlockMode::ApproachOnly:
      return in.zone == Zone::Close ? Decision::Unlock : Decision::None;
    case UnlockMode::ApproachAndIntent:
      return (in.intent && (in.zone == Zone::Near || in.zone == Zone::Close)) ? Decision::Unlock
                                                                              : Decision::None;
  }
  return Decision::None;
}

}  // namespace slock
