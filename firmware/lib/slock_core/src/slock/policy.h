#pragma once
// When to unlock automatically (docs/v1-framework.md section 8). A pure function so every rule is
// unit-testable. For one connected phone, ALL of these must hold:
//   authenticated  logged in with an enrolled key (fresh, single-use challenge)
//   outside        the lock believes this phone is Outside (measured or set by the user)
//   at_door        within about 1 m (calibrated)
//   home_ok        the phone signs "inside the home area" (strict mode refuses phones that don't say)
//   carried        the phone's signed step count rose within the last ~30 s
//   intent         the handle was just turned
//   locked         the bolt is locked

#include <cstdint>

namespace slock {

struct PolicyInputs {
  bool authenticated = false;
  bool outside = false;
  bool at_door = false;
  bool home_ok = false;
  bool carried = false;
  bool intent = false;
  bool locked = false;
};

enum class Decision : uint8_t { None, Unlock };

// Everything except the handle turn and the bolt: "this phone would unlock on a handle turn".
inline bool ready(const PolicyInputs& in) {
  return in.authenticated && in.outside && in.at_door && in.home_ok && in.carried;
}

inline Decision decide(const PolicyInputs& in) {
  return ready(in) && in.intent && in.locked ? Decision::Unlock : Decision::None;
}

}  // namespace slock
