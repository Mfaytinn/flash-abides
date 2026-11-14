// A busy recipient's waiting messages, smallest (sender << 32 | message id)
// first.
//
// ABIDES requeues a message for a busy agent at the time the agent is free, and
// messages due at the same time leave in (sender, id) order; this queue holds
// one recipient's requeued messages in that order.
//
// Starts as a sorted array with a moving front. A push with at least kMin
// entries spanning at most kGap values each moves them to a bitmap window of
// kWindow consecutive values. This check runs at power-of-two queue sizes.
// Window mode lasts until the queue empties; values outside its range go to
// the array, and the smaller front leaves first.
//
// use_window(false) keeps the queue as a sorted array.
#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace t3 {

class alignas(64) DeferQueue {
public:
  static constexpr uint64_t kWindow =
      16384; // values the window spans (a power of two): 2 KiB of bits
  static constexpr size_t kMin =
      4; // entries in the array from which the window is considered
  static constexpr uint64_t kGap =
      16; // and taken into use if they span no more values each

  // The array's storage, for the kernel's reserve-and-fault-in helper; holds no
  // entries yet.
  std::vector<uint64_t> &storage() { return v_; }
  // Whether this queue may move to the window when it grows long (it may unless
  // told not to). Call it on an empty queue.
  void use_window(bool yes) { windows_ = yes; }

  // `sid` is not in the queue.
  void push(uint64_t sid) {
    if (n_win_ !=
        0) { // the window is in use: it takes whatever is within its width
      const uint64_t lo = lo_ < sid ? lo_ : sid, hi = hi_ > sid ? hi_ : sid;
      if (hi - lo < kWindow) [[likely]] {
        mark(sid);
        ++n_win_;
        lo_ = lo;
        hi_ = hi;

        return;
      }
    } else if (const size_t n = v_.size() - head_;
               windows_ && n >= kMin && (n & (n - 1)) == 0) {
      if (start_window(sid, n))
        return;
    }
    push_array(sid);
  }
  uint64_t front() const {
    if (n_win_ == 0)
      return v_[head_];
    return head_ != v_.size() && v_[head_] < lo_ ? v_[head_] : lo_;
  }
  void pop_front() {
    if (n_win_ != 0 && (head_ == v_.size() || lo_ < v_[head_])) {
      // The window's lowest entry leaves; the next is the next set bit, at most
      // kWindow bits on. From the second word on the search may have come round
      // the ring: every bit it meets there is of a later entry, the earlier
      // ones of that word having left.
      uint64_t w = lo_ >> 6;
      uint64_t &word = bits_[w & (kWords - 1)];
      word &= ~(uint64_t{1} << (lo_ & 63));
      if (--n_win_ == 0)
        return;
      uint64_t m = word & (~uint64_t{0} << (lo_ & 63));
      while (!m)
        m = bits_[++w & (kWords - 1)];
      lo_ = (w << 6) + static_cast<uint64_t>(std::countr_zero(m));
      return;
    }
    if (++head_ == v_.size()) { // the array is empty: start again at the front
      v_.clear();
      head_ = 0;
    }
  }
  bool empty() const { return n_win_ == 0 && head_ == v_.size(); }
  size_t size() const { return n_win_ + (v_.size() - head_); }
  size_t capacity() const { return v_.capacity(); }

private:
  static constexpr size_t kWords = kWindow / 64;

  void mark(uint64_t sid) {
    bits_[(sid >> 6) & (kWords - 1)] |= uint64_t{1} << (sid & 63);
  }

  // The array's n entries and sid, from the smallest to the largest: if they
  // are close together, all of them move to the window (true).
  [[gnu::noinline]] bool start_window(uint64_t sid, size_t n) {
    const uint64_t lo = v_[head_] < sid ? v_[head_] : sid,
                   hi = v_.back() > sid ? v_.back() : sid;
    if (hi - lo > kGap * n || hi - lo >= kWindow)
      return false;
    if (!bits_) [[unlikely]]
      bits_.reset(new uint64_t[kWords]()); // this recipient's first use of it
    for (size_t i = head_; i < v_.size(); ++i)
      mark(v_[i]);
    mark(sid);
    v_.clear();
    head_ = 0;
    n_win_ = static_cast<uint32_t>(n + 1);
    lo_ = lo;
    hi_ = hi;

    return true;
  }

  void push_array(uint64_t sid) {

    // Out of room: reclaim the entries already taken if they are at least half
    // of the storage, else let the vector grow.
    if (v_.size() == v_.capacity() && head_ > 0 && head_ >= v_.size() / 2) {
      v_.erase(v_.begin(), v_.begin() + static_cast<std::ptrdiff_t>(head_));
      head_ = 0;
    }
    if (v_.size() == head_ ||
        sid > v_.back()) { // the usual case for the latest message
      v_.push_back(sid);
      return;
    }
    // Find the first entry above sid by branchless bisection. The array is not
    // empty and the last entry of [base, base + len) stays above sid at every
    // step, so at one entry it is the answer.
    const uint64_t *const b = v_.data() + head_;
    size_t base = 0, len = v_.size() - head_;
    while (len > 1) {
      const size_t half = len / 2;
      base += b[base + half - 1] <= sid ? half : 0;
      len -= half;
    }

    v_.insert(v_.begin() + static_cast<std::ptrdiff_t>(head_ + base), sid);
  }

  std::vector<uint64_t> v_; // [head_, size) ascending
  size_t head_ = 0;
  uint32_t n_win_ = 0;  // entries in the window
  bool windows_ = true; // the queue may use the window
  uint64_t lo_ = 0,
           hi_ = 0; // the smallest and the largest of the window's entries
  std::unique_ptr<uint64_t[]> bits_; // null, or kWords words: bit (sid &
                                     // (kWindow - 1)) for a waiting sid
};

static_assert(sizeof(DeferQueue) == 64); // one cache line a recipient

} // namespace t3
