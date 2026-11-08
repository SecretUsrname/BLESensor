#pragma once
// Basic types shared by the portable core. No Arduino or nRF headers here: everything in
// lib/slock_core builds and runs on a PC for unit tests.

#include <cstddef>
#include <cstdint>

namespace slock {

using Millis = uint32_t;

// Wrap-safe elapsed time for a 32-bit millisecond clock (rolls over every ~49 days).
inline uint32_t since(Millis now, Millis then) { return now - then; }

}  // namespace slock
