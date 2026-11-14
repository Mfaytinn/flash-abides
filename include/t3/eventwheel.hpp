// The kernel's event queue for runs with a few dozen to a few hundred events in
// flight: a timing wheel of two ordered keys per time slot, the 4-ary heap
// behind it for the rest. Same keys as EventHeap (sim.cpp's make_key()), same
// order of pops.
//
// Events are filed by due time into one of `slots` time slots of 2^shift ns.
// The next event is the first key of the first non-empty slot.
//
// A slot is two keys in ascending order, 32 bytes, and the slots are sized so
// that a key mostly has its slot to itself (sim.cpp, line_wheel_geometry()). A
// push reads the slot's first key and writes the pair in order, with no branch
// on what was there (an empty place holds kNone, which is above every key); a
// pop takes the first key and moves the second up. Nothing is compared on the
// way out.
//
// Events due beyond the window (the market close, a long latency draw) and
// events that find two keys in their slot go to the heap. top() is the smaller
// of the wheel's front and the heap's top, kept up to date by every push and
// pop, so the merge is exact for any mix: a key that spilled may well be
// smaller than the two that stayed. The window is [now, now + slots * 2^shift):
// advance(now) must follow every move of the kernel's clock, and every push is
// at or after it, which the kernel guarantees (a wake-up in the past throws, a
// message adds a latency >= 0).
//
// A run that would need slots of 1 ns here uses NsWheel (nswheel.hpp), which
// holds four keys of one instant in the same 32 bytes. (The wheel of large
// populations, eventqueue.hpp, keeps 32 sorted keys per slot, 512 bytes: with
// thousands of events in flight slots fill.)
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "t3/eventheap.hpp"

namespace t3 {

class EventWheel {
public:
  static constexpr size_t kCap = 2; // keys per slot
  static constexpr size_t kMinSlots = 64, kMaxSlots = size_t{1} << 16;
  static constexpr u128 kNone =
      EventHeap::kPad; // no key reaches it: an empty place, an empty queue

  // `slots` time slots (a power of two in [64, 65536]) of 2^shift ns.
  void init(unsigned shift, size_t slots) {
    if (slots < kMinSlots || slots > kMaxSlots || (slots & (slots - 1)))
      throw std::invalid_argument(
          "EventWheel: slots must be a power of two in [64, 65536]");
    if (shift > 60)
      throw std::invalid_argument("EventWheel: shift too large");
    shift_ = shift;
    slots_ = slots;
    mask_ = slots - 1;
    // assign() writes every entry, so the storage is faulted in here, before
    // the window.
    slot_.assign(slots, Slot{{kNone, kNone}});
    bits_.assign(slots >> 6, 0);
    far_.reserve(64); // its top() must be readable (kNone) while it is empty
    n_wheel_ = 0;
    cursor_ = 0;
    front_ = top_ = far_top_ = kNone;
    front_slot_ = 0;
  }
  // Capacity for n heap entries (the wheel's storage is written by init()).
  void reserve(size_t n) { far_.reserve(n < 64 ? 64 : n); }
  // The kernel's clock moved to `now` (never backwards).
  void advance(uint64_t now) { cursor_ = now >> shift_; }

  bool empty() const { return top_ == kNone; }
  size_t size() const { return n_wheel_ + far_.size(); }
  u128 top() const { return top_; }

  void push(u128 k) {
    const uint64_t slot = static_cast<uint64_t>(k >> 64) >> shift_;
    const size_t s = slot & mask_;
    Slot &sl = slot_[s];
    const u128 first = sl.k[0], second = sl.k[1];
    // Beyond the window, or the slot holds two keys: the heap.
    if (slot - cursor_ >= slots_ || second != kNone) [[unlikely]] {

      push_far(k);
      return;
    }

    // The pair in order. An empty slot's first place holds kNone, so k goes
    // first and kNone second; equal keys are indistinguishable, so either may
    // go first.
    const bool before = k < first;
    sl.k[0] = before ? k : first;
    sl.k[1] = before ? first : k;
    bits_[s >> 6] |= uint64_t{1} << (s & 63);
    ++n_wheel_;
    if (k < front_) {
      front_ = k;
      front_slot_ = static_cast<uint32_t>(s);
      if (k < top_)
        top_ = k;
    }
  }

  u128 pop() {
    const u128 k = top_;
    if (k == front_) [[likely]] { // equal keys are indistinguishable: either
                                  // source may give one up
      const size_t s = front_slot_;
      Slot &sl = slot_[s];
      const u128 next = sl.k[1];
      sl.k[0] = next; // kNone, twice, when the slot held one key
      sl.k[1] = kNone;
      const uint64_t emptied = next == kNone;
      uint64_t &word = bits_[s >> 6];
      const uint64_t w = word & ~(emptied << (s & 63));
      word = w;
      --n_wheel_;
      // The popped key was the wheel's smallest, so every key left is due in
      // this slot's time or later: the front is the first key of the next
      // marked slot in cyclic order, which is this one if it kept its second
      // key. A burst's next key is a few slots on, in the same word of the
      // bitmap; the search beyond it is out of line.
      const uint64_t m = w >> (s & 63);
      if (m) [[likely]] {
        const size_t s2 = s + static_cast<size_t>(std::countr_zero(m));
        front_ = slot_[s2].k[0];
        front_slot_ = static_cast<uint32_t>(s2);
      } else {
        front_in_another_word(s >> 6);
      }
    } else {
      pop_far();
    }
    top_ = far_top_ < front_ ? far_top_ : front_;
    return k;
  }

  unsigned shift() const { return shift_; }
  size_t slots() const { return slots_; }

private:
  struct alignas(32) Slot {
    u128 k[kCap]; // ascending; an empty place holds kNone
  };
  static_assert(sizeof(Slot) == 32 && kCap == 2);

  // Nothing is left in word `w` of the bitmap from the popped slot on: the
  // front is the first key of the first marked slot of a later word in cyclic
  // order, coming round to `w` itself (its slots below the popped one) last. Or
  // the wheel is empty.
  [[gnu::noinline]] void front_in_another_word(size_t w) {
    if (n_wheel_ == 0) {
      front_ = kNone;
      return;
    }
    const size_t words = slots_ >> 6;
    for (size_t i = 1; i <= words; ++i) {
      const size_t x = (w + i) & (words - 1);
      if (bits_[x]) {
        const size_t s =
            (x << 6) + static_cast<size_t>(std::countr_zero(bits_[x]));
        front_ = slot_[s].k[0];
        front_slot_ = static_cast<uint32_t>(s);
        return;
      }
    }
    throw std::logic_error("EventWheel: keys counted but no slot marked");
  }
  // Out of line: one push in a few hundred, and the heap's walks are long.
  [[gnu::noinline]] void push_far(u128 k) {
    far_.push(k);
    far_top_ = far_.top();
    if (k < top_)
      top_ = k;
  }
  [[gnu::noinline]] void pop_far() {
    far_.pop();
    far_top_ = far_.top(); // kNone when that was its last key
  }

  u128 top_ =
      kNone; // the smaller of front_ and far_top_; kNone: the queue is empty
  u128 front_ =
      kNone; // the wheel's smallest key, the first of slot front_slot_
  u128 far_top_ = kNone; // the heap's top
  uint32_t front_slot_ = 0;
  unsigned shift_ = 0;
  uint64_t cursor_ = 0; // the slot of the kernel's clock
  size_t slots_ = 0, mask_ = 0;
  std::vector<Slot> slot_;
  std::vector<uint64_t> bits_; // per 64 slots: which hold a key
  size_t n_wheel_ = 0;
  EventHeap far_; // beyond the window, and spilled from full slots
};

} // namespace t3
