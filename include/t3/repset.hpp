// The representatives of deferred deliveries.
//
// A message that arrives while its recipient is busy waits in that recipient's
// queue, and the recipient has one representative: the key (busy-until time,
// sender, recipient, message id) at which the first waiting message is due. The
// kernel takes whichever is smallest of the event heap's top, the wakeup lanes'
// best and the smallest representative.
//
// Representatives form an indexed binary heap; a delivery re-keys the top
// in place.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "t3/eventheap.hpp"

namespace t3 {

class RepSet {
public:
  static constexpr u128 kNoKey =
      ~u128{0}; // no key reaches it: the kernel's times are far below 2^64 - 1

  void reserve(size_t recipients) {
    pos_.assign(recipients, kAbsent);
    e_.reserve(64);
  }
  bool empty() const { return e_.empty(); }
  size_t size() const { return e_.size(); }
  // The smallest key, kNoKey when the set is empty, and its recipient.
  u128 top_key() const { return top_; }
  uint32_t top() const { return e_[0].r; }
  bool has(uint32_t r) const { return pos_[r] != kAbsent; }

  // Recipient r's representative is `key`: added, or moved if r already has
  // one.
  void set(uint32_t r, u128 key) {
    uint32_t i = pos_[r];
    if (i == kAbsent) {
      i = static_cast<uint32_t>(e_.size());
      e_.push_back(Entry{key, r});
      up(i, Entry{key, r});
    } else if (key < e_[i].key) {
      up(i, Entry{key, r});
    } else {
      down(i, Entry{key, r});
    }
    top_ = e_[0].key;
  }
  // The top recipient's representative becomes `key`, which is not smaller than
  // its old one.
  void replace_top(u128 key) {
    down(0, Entry{key, e_[0].r});
    top_ = e_[0].key;
  }
  // The top recipient has no message waiting any more.
  void erase_top() {
    pos_[e_[0].r] = kAbsent;
    const Entry last = e_.back();
    e_.pop_back();
    if (e_.empty()) {
      top_ = kNoKey;
      return;
    }
    down(0, last);
    top_ = e_[0].key;
  }

private:
  static constexpr uint32_t kAbsent = 0xFFFFFFFFu;
  struct Entry {
    u128 key;
    uint32_t r;
  };

  void up(uint32_t i, Entry x) {
    while (i > 0) {
      const uint32_t p = (i - 1) / 2;
      if (!(x.key < e_[p].key))
        break;
      e_[i] = e_[p];
      pos_[e_[i].r] = i;
      i = p;
    }
    e_[i] = x;
    pos_[x.r] = i;
  }
  // Bottom-up sift. The key being placed is nearly always one of the largest in
  // the set (a delivery re-keys the top to a time just after "now"), so it ends
  // at or near a leaf. Walk the smaller-child path to a leaf with no compare
  // against x and a branch-free choice of child (one compare per level instead
  // of two, and the 50/50 choice is a select, not a branch the CPU mispredicts
  // every other level), then climb back up while x is smaller than the parent.
  // x lands below the last path key smaller than it, as in the classic
  // sift-down, so the heap, and every pop, is the same.
  void down(uint32_t i, Entry x) {
    const uint32_t n = static_cast<uint32_t>(e_.size());
    Entry *const e = e_.data();
    uint32_t *const pos = pos_.data();
    uint32_t hole = i;
    for (;;) {
      uint32_t c = 2 * hole + 1;
      if (c >= n)
        break;
      const uint32_t c1 =
          c + 1 < n ? c + 1 : c; // no sibling: compare c with itself
      c += static_cast<uint32_t>(e[c1].key < e[c].key) & (c1 != c);
      e[hole] = e[c];
      pos[e[hole].r] = hole;
      hole = c;
    }
    while (hole > i) {
      const uint32_t p = (hole - 1) / 2;
      if (!(x.key < e[p].key))
        break;
      e[hole] = e[p];
      pos[e[hole].r] = hole;
      hole = p;
    }
    e[hole] = x;
    pos[x.r] = hole;
  }

  std::vector<Entry> e_;      // binary min-heap on key
  std::vector<uint32_t> pos_; // recipient -> index in e_, or kAbsent
  u128 top_ = kNoKey;
};

} // namespace t3
