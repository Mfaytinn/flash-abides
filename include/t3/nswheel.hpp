// The kernel's event queue for crowded runs whose latencies stay within a few
// microseconds: a timing wheel of one slot per nanosecond, the 4-ary heap
// behind it for the rest. Same keys as EventHeap (sim.cpp's make_key()), same
// order of pops.
//
// EventWheel (eventwheel.hpp) sizes its slots so that a slot seldom holds more
// than a key. With a few hundred events in flight and latencies of half a
// microsecond that already means slots of 1 ns, and from there on every further
// event in flight lands in a slot that is taken. But a slot of 1 ns is one
// instant: its keys differ in their low halves only (sender, recipient, message
// id). This wheel keeps just those, up to eight to a slot in ascending order:
// 64 bytes, two 256-bit registers. A push puts its half in place with one
// compare of the register against it and two blends, with no branch on what the
// slot holds; a pop takes the first half and moves the others down a place. A
// key's time is its slot's: the next front's is the popped key's time plus the
// distance to its slot. Keys are packed from the front, so a slot of up to four
// keys has the second register empty and push and pop work on the first alone;
// only a slot of five to eight keys takes the slower path across the two (one
// predicted branch tells which).
//
// The stored halves are biased by 2^63, so that the register's signed compare
// orders them as the unsigned values they are, and an empty place holds the
// largest of them: the low half with every bit set (sender and recipient 65,535
// with the last message id), which is therefore never filed in a slot. That
// key, a key beyond the window (the market close, a long latency draw) and a
// ninth key of one instant go to the heap; top() is the smaller of the wheel's
// front and the heap's top, kept up to date by every push and pop, so the merge
// is exact for any mix. The window is [now, now + slots): advance(now) must
// follow every move of the kernel's clock, and every push is at or after it,
// which the kernel guarantees (a wake-up in the past throws, a message adds a
// latency >= 0).
//
// Which slot is next is read from a bitmap of the slots and, when the popped
// slot's word has nothing further, from one summary word of the bitmap's words:
// at most 4,096 slots.
//
// Selected by sim.cpp's queue_kind() when EventWheel would use 1 ns slots.
#pragma once

#include <immintrin.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "t3/eventheap.hpp"

namespace t3 {

class NsWheel {
public:
  static constexpr size_t kCap = 8;  // keys per slot: two 256-bit registers
  static constexpr size_t kHalf = 4; // keys a register holds
  static constexpr size_t kMinSlots = 64,
                          kMaxSlots = 4096; // the summary is one 64-bit word
  static constexpr u128 kNone =
      EventHeap::kPad; // no key reaches it: an empty queue

  // `slots` slots of 1 ns: a power of two in [64, 4096].
  void init(size_t slots) {
    if (slots < kMinSlots || slots > kMaxSlots || (slots & (slots - 1)))
      throw std::invalid_argument(
          "NsWheel: slots must be a power of two in [64, 4096]");
    slots_ = slots;
    mask_ = slots - 1;
    // assign() writes every entry, so the storage is faulted in here, before
    // the window.
    slot_.assign(slots, Slot{{kEmpty, kEmpty, kEmpty, kEmpty, kEmpty, kEmpty,
                              kEmpty, kEmpty}});
    bits_.assign(slots >> 6, 0);
    summary_ = 0;
    far_.reserve(64); // its top() must be readable (kNone) while it is empty
    n_wheel_ = 0;
    cursor_ = 0;
    front_ = top_ = far_top_ = kNone;
    front_slot_ = 0;
  }
  // Capacity for n heap entries (the wheel's storage is written by init()).
  void reserve(size_t n) { far_.reserve(n < 64 ? 64 : n); }
  // The kernel's clock moved to `now` (never backwards).
  void advance(uint64_t now) { cursor_ = now; }

  bool empty() const { return top_ == kNone; }
  size_t size() const { return n_wheel_ + far_.size(); }
  u128 top() const { return top_; }

  void push(u128 k) {
    const uint64_t t = static_cast<uint64_t>(k >> 64);
    const uint64_t low = static_cast<uint64_t>(k);
    const size_t s = t & mask_;
    Slot &sl = slot_[s];
    // Beyond the window, a slot that holds eight keys, or the low half of an
    // empty place: the heap.
    if (t - cursor_ >= slots_ || sl.k[kCap - 1] != kEmpty ||
        low == ~uint64_t{0}) [[unlikely]] {

      push_far(k);
      return;
    }

    // The keys above k each move up a place (the last place is empty, so none
    // is lost) and k takes the first place that moved; an empty place is above
    // every key. Equal keys are indistinguishable, so it does not matter that k
    // goes behind its equals. Keys are packed from the front, so while the
    // first register has a place left the second is empty.
    const __m256i key = _mm256_set1_epi64x(static_cast<int64_t>(low ^ kBias));
    const __m256i lo =
        _mm256_load_si256(reinterpret_cast<const __m256i *>(sl.k));
    const __m256i above = _mm256_cmpgt_epi64(lo, key);
    if (sl.k[kHalf - 1] == kEmpty) [[likely]] {
      const __m256i moved =
          _mm256_blendv_epi8(lo, _mm256_permute4x64_epi64(lo, 0b10010000),
                             above); // from [lo0, lo0, lo1, lo2]
      const __m256i above_before =
          _mm256_blend_epi32(_mm256_permute4x64_epi64(above, 0b10010000),
                             _mm256_setzero_si256(), 0b00000011);
      _mm256_store_si256(
          reinterpret_cast<__m256i *>(sl.k),
          _mm256_blendv_epi8(moved, key,
                             _mm256_andnot_si256(above_before, above)));
    } else {
      insert_into_both(sl, lo, above, key);
    }
    bits_[s >> 6] |= uint64_t{1} << (s & 63);
    summary_ |= uint64_t{1} << (s >> 6);
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
      const uint64_t emptied = sl.k[1] == kEmpty;
      // [k1, k2, k3, empty]: with one key in the slot that is four empty places
      // again. With a key in the second register that one moves up into the
      // first's last place.
      const __m256i v =
          _mm256_load_si256(reinterpret_cast<const __m256i *>(sl.k));
      if (sl.k[kHalf] == kEmpty) [[likely]] {
        _mm256_store_si256(
            reinterpret_cast<__m256i *>(sl.k),
            _mm256_blend_epi32(_mm256_permute4x64_epi64(v, 0b11111001),
                               _mm256_set1_epi64x(kEmpty), 0b11000000));
      } else {
        remove_first_of_both(sl, v);
      }
      const size_t wi = s >> 6;
      const uint64_t w = bits_[wi] & ~(emptied << (s & 63));
      bits_[wi] = w;
      summary_ &= ~(static_cast<uint64_t>(w == 0) << wi);
      --n_wheel_;
      // The popped key was the wheel's smallest, so every key left is due in
      // this slot's nanosecond or later: the front is in the next marked slot
      // in cyclic order, which is this one if it kept a key. A burst's next key
      // is a few slots on, in the same word of the bitmap; the search beyond it
      // is out of line.
      const uint64_t m = w >> (s & 63);
      if (m) [[likely]] {
        const size_t ahead = static_cast<size_t>(std::countr_zero(m));
        set_front(s + ahead, static_cast<uint64_t>(k >> 64) + ahead);
      } else {
        front_in_another_word(s, static_cast<uint64_t>(k >> 64));
      }
    } else {
      pop_far();
    }
    top_ = far_top_ < front_ ? far_top_ : front_;
    return k;
  }

  unsigned shift() const { return 0; }
  size_t slots() const { return slots_; }

private:
  static constexpr uint64_t kBias = uint64_t{1}
                                    << 63; // stored halves compare as signed
  static constexpr int64_t kEmpty =
      INT64_MAX; // the low half ~0, biased: above every key
  struct alignas(64) Slot {
    int64_t k[kCap]; // biased low halves, ascending; empty places last
  };
  static_assert(sizeof(Slot) == 64 && kCap == 2 * kHalf);

  // Both registers hold keys, or the first is full: the keys above `key` move
  // up a place across the join, the second register's first place taking the
  // first register's last.
  [[gnu::always_inline]] static inline void
  insert_into_both(Slot &sl, __m256i lo, __m256i above_lo, __m256i key) {
    const __m256i hi =
        _mm256_load_si256(reinterpret_cast<const __m256i *>(sl.k + kHalf));
    const __m256i above_hi = _mm256_cmpgt_epi64(hi, key);
    const __m256i up_lo =
        _mm256_permute4x64_epi64(lo, 0b10010000); // [lo0, lo0, lo1, lo2]
    const __m256i up_hi = _mm256_alignr_epi8(
        hi, _mm256_permute2x128_si256(lo, hi, 0x21), 8); // [lo3, hi0, hi1, hi2]
    const __m256i before_lo =
        _mm256_blend_epi32(_mm256_permute4x64_epi64(above_lo, 0b10010000),
                           _mm256_setzero_si256(), 0b00000011);
    const __m256i before_hi = _mm256_alignr_epi8(
        above_hi, _mm256_permute2x128_si256(above_lo, above_hi, 0x21),
        8); // [a_lo3, a_hi0, a_hi1, a_hi2]
    const __m256i moved_lo = _mm256_blendv_epi8(lo, up_lo, above_lo);
    const __m256i moved_hi = _mm256_blendv_epi8(hi, up_hi, above_hi);
    _mm256_store_si256(
        reinterpret_cast<__m256i *>(sl.k),
        _mm256_blendv_epi8(moved_lo, key,
                           _mm256_andnot_si256(before_lo, above_lo)));
    _mm256_store_si256(
        reinterpret_cast<__m256i *>(sl.k + kHalf),
        _mm256_blendv_epi8(moved_hi, key,
                           _mm256_andnot_si256(before_hi, above_hi)));
  }
  // The slot's first key leaves with the second register in use: [lo1, lo2,
  // lo3, hi0] and [hi1, hi2, hi3, empty].
  [[gnu::always_inline]] static inline void remove_first_of_both(Slot &sl,
                                                                 __m256i lo) {
    const __m256i hi =
        _mm256_load_si256(reinterpret_cast<const __m256i *>(sl.k + kHalf));
    _mm256_store_si256(
        reinterpret_cast<__m256i *>(sl.k),
        _mm256_alignr_epi8(_mm256_permute2x128_si256(lo, hi, 0x21), lo, 8));
    _mm256_store_si256(
        reinterpret_cast<__m256i *>(sl.k + kHalf),
        _mm256_alignr_epi8(
            _mm256_permute2x128_si256(hi, _mm256_set1_epi64x(kEmpty), 0x21), hi,
            8));
  }

  // The front is the first key of slot `s`, whose nanosecond is `t`.
  [[gnu::always_inline]] inline void set_front(size_t s, uint64_t t) {
    front_ = (static_cast<u128>(t) << 64) |
             (static_cast<uint64_t>(slot_[s].k[0]) ^ kBias);
    front_slot_ = static_cast<uint32_t>(s);
  }
  // Nothing is left in the word of slot `s` (the popped slot, of time `t`) from
  // `s` on: the front is in the first marked slot of the next marked word in
  // cyclic order, which is that word again, below `s`, when no other is marked.
  // Or the wheel is empty.
  [[gnu::noinline]] void front_in_another_word(size_t s, uint64_t t) {
    if (summary_ == 0) {
      front_ = kNone;
      return;
    }
    const size_t wi = s >> 6;
    const uint64_t later =
        (summary_ >> wi) >> 1; // the words after wi (two shifts: wi may be 63)
    const size_t x = later
                         ? wi + 1 + static_cast<size_t>(std::countr_zero(later))
                         : static_cast<size_t>(std::countr_zero(summary_));
    const size_t s2 =
        (x << 6) + static_cast<size_t>(std::countr_zero(bits_[x]));
    set_front(s2, t + ((s2 - s) & mask_));
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
  uint64_t cursor_ = 0; // the kernel's clock: the window starts at its slot
  size_t slots_ = 0, mask_ = 0;
  uint64_t summary_ = 0; // bit w: bits_[w] is not zero
  std::vector<Slot> slot_;
  std::vector<uint64_t> bits_; // per 64 slots: which hold a key
  size_t n_wheel_ = 0;
  EventHeap far_; // beyond the window, and spilled from full slots
};

} // namespace t3
