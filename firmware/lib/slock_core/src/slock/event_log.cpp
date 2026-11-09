#include "event_log.h"

namespace slock {

void EventLog::add(EventType type, Millis now, int key, uint8_t a, uint8_t b) {
  Event& e = ev_[head_];
  e.seq = next_++;
  e.t_ms = now;
  e.type = static_cast<uint8_t>(type);
  e.key = static_cast<int8_t>(key);
  e.a = a;
  e.b = b;
  head_ = static_cast<uint16_t>((head_ + 1) % kCapacity);
  if (count_ < kCapacity) ++count_;
}

uint16_t EventLog::read(uint32_t from_seq, Event* out, uint16_t max) const {
  uint16_t n = 0;
  uint16_t start = static_cast<uint16_t>((head_ + kCapacity - count_) % kCapacity);  // oldest
  for (uint16_t i = 0; i < count_ && n < max; ++i) {
    const Event& e = ev_[(start + i) % kCapacity];
    if (e.seq >= from_seq) out[n++] = e;
  }
  return n;
}

const Event* EventLog::latest() const {
  if (count_ == 0) return nullptr;
  return &ev_[(head_ + kCapacity - 1) % kCapacity];
}

}  // namespace slock
