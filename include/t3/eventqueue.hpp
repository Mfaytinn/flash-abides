// The kernel's event queue for large populations: a timing wheel for the events
// due soon, the 4-ary heap behind it for the rest. Same keys as EventHeap
// (sim.cpp's make_key()), same order of pops.
//
// The wheel files each event by its due time into one of `slots` time slots
// of 2^shift ns and pops from the first non-empty slot. Entries within a slot
// stay sorted by the full key, so
// equal-slot events leave in exactly the heap's order. Events due beyond the
// window (the market close, a long latency draw) and events that find their
// slot full go to the heap; top() and pop() take the smaller of the wheel's
// front and the heap's top, so the merge is exact for any mix. The window is
// [now, now + slots * 2^shift): advance(now) must follow every move of the
// kernel's clock, and every push is at or after it, which the kernel guarantees
// already (a wake-up in the past throws, a message adds a non-negative
// latency).
//
// Selected by sim.cpp's queue_kind() for large populations.
#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "t3/eventheap.hpp"

namespace t3 {

class EventQueue {
public:
  static constexpr size_t kCap =
      32; // entries per slot (a power of two); a full slot spills to the heap
  static constexpr size_t kMinSlots = 64, kMaxSlots = size_t{1} << 16;

  // `slots` time slots (a power of two, at least 64; 0 leaves the heap on its
  // own) of 2^shift ns.
  void init(unsigned shift, size_t slots) {
    if (slots &&
        (slots < kMinSlots || slots > kMaxSlots || (slots & (slots - 1))))
      throw std::invalid_argument(
          "EventQueue: slots must be 0 or a power of two in [64, 65536]");
    if (shift > 60)
      throw std::invalid_argument("EventQueue: shift too large");
    shift_ = shift;
    slots_ = slots;
    mask_ = slots ? slots - 1 : 0;
    // assign() writes every entry, so the storage is faulted in here, before
    // the window.
    keys_.assign(slots * kCap, 0);
    head_.assign(slots, 0);
    count_.assign(slots, 0);
    bits_.assign(slots >> 6, 0);
    n_wheel_ = 0;
    have_front_ = false;
    cursor_ = 0;
    wheel_pushes_ = far_pushes_ = spills_ = occupancy_sum_ = 0;
  }
  // Capacity for n heap entries (the wheel's storage is written by init()).
  void reserve(size_t n) { heap_.reserve(n); }
  // The kernel's clock moved to `now` (never backwards).
  void advance(uint64_t now) { cursor_ = now >> shift_; }

  bool empty() const { return !have_front_ && heap_.empty(); }
  size_t size() const { return n_wheel_ + heap_.size(); }
  u128 top() const {
    if (!have_front_)
      return heap_.top();
    if (heap_.empty())
      return front_;
    const u128 h = heap_.top();
    return h < front_ ? h : front_;
  }

  void push(u128 k) {
    const uint64_t slot = static_cast<uint64_t>(k >> 64) >> shift_;
    if (slot - cursor_ >= slots_)
        [[unlikely]] { // beyond the window (or no wheel): the heap
      heap_.push(k);
      ++far_pushes_;
      return;
    }
    const size_t s = slot & mask_;
    const unsigned c = count_[s];
    if (c == kCap) [[unlikely]] {
      heap_.push(k);
      ++spills_;
      return;
    }
    u128 *keys = keys_.data() + s * kCap;
    const unsigned h = head_[s];
    // The entries of a slot sit at keys[(h + i) & kCapMask], i < c, in
    // ascending key order. p: where the new key goes (after every key below
    // it), found from the back, where the later of two messages usually lands.
    unsigned p = c;
    while (p > 0 && k < keys[(h + p - 1) & kCapMask])
      --p;
    if (p == c) {
      keys[(h + c) & kCapMask] = k;
    } else if (p ==
               0) { // due before everything in the slot: it lands at the front
      head_[s] = (h - 1) & kCapMask;
      keys[head_[s]] = k;
    } else if (p <= c / 2) { // move the shorter side: the front part one step
                             // forward
      const unsigned nh = (h - 1) & kCapMask;
      head_[s] = nh;
      for (unsigned i = 0; i < p; ++i)
        keys[(nh + i) & kCapMask] = keys[(nh + i + 1) & kCapMask];
      keys[(nh + p) & kCapMask] = k;
    } else { // the back part one step back
      for (unsigned i = c; i > p; --i)
        keys[(h + i) & kCapMask] = keys[(h + i - 1) & kCapMask];
      keys[(h + p) & kCapMask] = k;
    }
    if (c == 0)
      bits_[s >> 6] |= uint64_t{1} << (s & 63);
    count_[s] = static_cast<uint8_t>(c + 1);
    ++n_wheel_;
    ++wheel_pushes_;
    occupancy_sum_ += c;
    if (!have_front_ || k < front_) {
      front_ = k;
      front_slot_ = s;
      have_front_ = true;
    }
  }

  u128 pop() {
    if (!have_front_ || (!heap_.empty() && heap_.top() < front_))
      return heap_.pop();
    const size_t s = front_slot_;
    const unsigned h = head_[s];
    const u128 k = keys_[s * kCap + h];
    head_[s] = static_cast<uint8_t>((h + 1) & kCapMask);
    --n_wheel_;
    const unsigned c = count_[s] - 1;
    count_[s] = static_cast<uint8_t>(c);
    if (c) {
      front_ = keys_[s * kCap + head_[s]];
      return k;
    }
    bits_[s >> 6] &= ~(uint64_t{1} << (s & 63));
    // The popped key was the wheel's smallest, so every remaining entry is due
    // in this slot's window or later: the next non-empty slot in cyclic order
    // from here is the new front.
    find_front_from(s);
    return k;
  }

  // Diagnostics.
  size_t wheel_pushes() const { return wheel_pushes_; }
  size_t far_pushes() const {
    return far_pushes_;
  } // beyond the window: pushed to the heap
  size_t spills() const { return spills_; } // slot full: pushed to the heap
  // Mean entries a wheel push found in its slot (its insertion walks over part
  // of them).
  double mean_occupancy() const {
    return wheel_pushes_ ? double(occupancy_sum_) / double(wheel_pushes_) : 0.0;
  }
  unsigned shift() const { return shift_; }
  size_t slots() const { return slots_; }

private:
  static constexpr unsigned kCapMask = kCap - 1;
  static_assert((kCap & (kCap - 1)) == 0 && kCap <= 128,
                "kCap: a power of two that fits count_");

  // Sets the front to the first non-empty slot at or after `pos` in cyclic
  // order, or clears it.
  void find_front_from(size_t pos) {
    const size_t words = slots_ >> 6;
    const size_t w0 = pos >> 6;
    for (size_t i = 0; i <= words; ++i) {
      const size_t w = (w0 + i) & (words - 1);
      uint64_t m = bits_[w];
      if (i == 0)
        m &= ~uint64_t{0} << (pos & 63);
      if (i == words)
        m &= ~(~uint64_t{0} << (pos & 63));
      if (m) {
        const size_t s = (w << 6) + static_cast<size_t>(std::countr_zero(m));
        front_ = keys_[s * kCap + head_[s]];
        front_slot_ = s;
        have_front_ = true;
        return;
      }
    }
    have_front_ = false;
  }

  unsigned shift_ = 0;
  size_t slots_ = 0, mask_ = 0;
  uint64_t cursor_ = 0;        // the slot of the kernel's clock
  std::vector<u128> keys_;     // slots_ * kCap
  std::vector<uint8_t> head_;  // per slot: index of its first entry
  std::vector<uint8_t> count_; // per slot: entries
  std::vector<uint64_t> bits_; // per 64 slots: which are non-empty
  size_t n_wheel_ = 0;
  bool have_front_ = false;
  u128 front_ = 0; // the wheel's smallest key, in slot front_slot_
  size_t front_slot_ = 0;
  EventHeap heap_;
  size_t wheel_pushes_ = 0, far_pushes_ = 0, spills_ = 0, occupancy_sum_ = 0;
};

} // namespace t3
