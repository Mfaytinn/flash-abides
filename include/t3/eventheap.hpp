// The kernel's event queue: a 4-ary min-heap of event keys.
//
// A key is the ABIDES priority (t, sender, recipient, message_id) packed into
// 128 bits by sim.cpp's make_key(), so one unsigned compare orders two events.
//
// Slots past the last entry hold kPad, which no key reaches (t is far below
// 2^64 - 1), so every node has four children to compare and a node at the edge
// of the heap needs no special case.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace t3 {

using u128 = unsigned __int128;

class EventHeap {
public:
  static constexpr size_t kArity = 4;
  static constexpr u128 kPad = ~u128{0};

  // Index (0..3) of the smallest of k[0..3]; among equal keys the lowest index,
  // the order the heap has always resolved them in. No branch may depend on
  // the keys: which child wins is close to random, so such a branch would
  // mispredict.
  //
  // Two pairs side by side, then their winners: the last compare waits for one
  // compare before it, where a scan over the four waits for two. Each pair
  // keeps its smaller key in a register; reading k[] again through the winning
  // index would put a load back on the chain. The left operand wins every tie,
  // which is the lowest index.
  [[gnu::always_inline]] static inline size_t min4(const u128 *k) {
    const bool r01 = k[1] < k[0];
    const bool r23 = k[3] < k[2];
    const u128 m01 = r01 ? k[1] : k[0];
    const u128 m23 = r23 ? k[3] : k[2];
    // The empty asm keeps both pairs' indices in registers before the last
    // compare, preventing their computation from moving behind a branch.
    size_t i01 = r01, i23 = size_t{2} + r23;
    asm("" : "+r"(i01), "+r"(i23));
    return m23 < m01 ? i23 : i01;
  }

  // Capacity for n entries, written once so its pages are faulted in now.
  void reserve(size_t n) {
    if (n > cap_)
      grow(n);
  }
  bool empty() const { return n_ == 0; }
  size_t size() const { return n_; }

  u128 top() const { return h_[0]; }
  void push(u128 k) {
    if (n_ == cap_) [[unlikely]]
      grow(cap_ ? 2 * cap_ : 64);
    u128 *h = h_.data();
    size_t i = n_++;
    while (i > 0) {
      const size_t p = (i - 1) / kArity;
      if (!(k < h[p]))
        break;
      h[i] = h[p];
      i = p;
    }
    h[i] = k;
  }
  // Inlined into the event loop, its one caller.
  [[gnu::always_inline]] inline u128 pop() {
    static_assert(kArity == 4, "min4() compares four children");
    u128 *h = h_.data();
    const u128 top = h[0];
    const size_t n = --n_;
    const u128 last = h[n];
    h[n] = kPad; // the freed slot rejoins the padding
    if (n == 0)
      return top;
    size_t i = 0;
    for (;;) {
      const size_t c = kArity * i + 1;
      if (c >= n)
        break;
      const size_t best =
          c + min4(h + c); // children past n are padding and never win
      const u128 bv = h[best];
      if (!(bv < last))
        break;
      h[i] = bv;
      i = best;
    }
    h[i] = last;
    return top;
  }

private:
  // Room for `cap` entries plus the padding a node at index cap - 1 would
  // compare.
  [[gnu::noinline, gnu::cold]] void grow(size_t cap) {
    h_.resize(cap + kArity, kPad);
    cap_ = cap;
  }

  std::vector<u128> h_; // entries [0, n_), then kPad to the end
  size_t n_ = 0, cap_ = 0;
};

} // namespace t3
