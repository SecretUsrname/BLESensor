#pragma once
// When to unlock automatically. A pure function so every rule is unit-testable.
//
// For one connected phone:
//   authenticated  logged in with an enrolled key
//   armed          its login counted as a genuine ARRIVAL (it had been gone long enough), and
//                  the arrival hasn't been used or expired
//   at_door        it reached the Close zone while armed, recently, and is still Close
//   context_ok     what the phone signed about its home geofence doesn't contradict an arrival
//   locked         the bolt is locked (nothing to do otherwise)
// Mode A (ApproachOnly) unlocks on all of the above. Mode B (ApproachAndIntent) also needs a
// handle touch. The caller disarms the phone after acting on Unlock.

#include <cstdint>

namespace slock {

enum class UnlockMode : uint8_t { ApproachOnly, ApproachAndIntent };

struct PolicyInputs {
  bool authenticated = false;
  bool armed = false;
  bool at_door = false;
  bool context_ok = false;
  bool intent = false;
  bool locked = false;
};

enum class Decision : uint8_t { None, Unlock };

inline Decision decide(UnlockMode mode, const PolicyInputs& in) {
  if (!in.authenticated || !in.armed || !in.at_door || !in.context_ok || !in.locked) return Decision::None;
  if (mode == UnlockMode::ApproachAndIntent && !in.intent) return Decision::None;
  return Decision::Unlock;
}

}  // namespace slock
