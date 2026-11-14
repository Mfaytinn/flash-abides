// A market maker's open orders (TradingAgent.orders), in the order they were
// placed.
//
// The order of the list is part of the simulation: a market maker's act sends
// one CancelOrder per open order, in list order. Orders are placed with
// ascending ids (the kernel's order counter), and the 2 * depth_levels orders
// of one quote take consecutive ids.
//
// The acknowledgements of a quote's cancels come back while the next quote's
// orders are already at the back, oldest first but a little out of order (the
// latency jitter puts one about 15 places from where order would have it at 32
// quote levels). A std::vector erased in place moved every later entry on each
// acknowledgement, work that grows with the square of the quote depth. Here a
// removed order is only marked, so no entry ever moves between two reclaims,
// and an order sits exactly as many slots after the oldest open one as their
// ids differ, unless ids of other agents lie in between: that slot is looked at
// first, and only if it holds another id is the range before it bisected.
// Marked entries are reclaimed when the front passes them or the storage is
// full.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace t3 {

struct OpenOrder {
  int64_t id;
  int64_t price;
  int64_t qty;
  uint8_t side;
  uint8_t gone = 0; // OpenOrders: removed, its slot not reclaimed yet
};

class OpenOrders {
public:
  // The storage, for the kernel's reserve-and-fault-in helper; holds no orders
  // yet.
  std::vector<OpenOrder> &storage() { return v_; }

  // `o.id` is larger than every id pushed before.
  void push(const OpenOrder &o) {
    // Out of room: reclaim the removed entries if they are at least half of the
    // storage (so an entry is moved at most once per time it could be), else
    // let the vector grow.
    if (v_.size() == v_.capacity() &&
        v_.size() - live_ >= (v_.size() + 1) / 2 && v_.size() > live_)
      reclaim();
    v_.push_back(o);
    v_.back().gone = 0;
    ++live_;
  }

  // OrderExecuted for `qty`: the order leaves the list when that fills it, else
  // its quantity drops. An id that is not in the list is ignored.
  void executed(int64_t id, int64_t qty) {
    const size_t k = find(id);
    if (k == npos)
      return;
    if (qty >= v_[k].qty)
      remove(k);
    else
      v_[k].qty -= qty;
  }

  // OrderCancelled: the order leaves the list. An id that is not in the list is
  // ignored.
  void cancelled(int64_t id) {
    const size_t k = find(id);
    if (k != npos)
      remove(k);
  }

  // Calls f(order) for every open order, oldest first.
  template <class F> void for_each(F f) const {
    for (size_t i = head_; i < v_.size(); ++i)
      if (!v_[i].gone)
        f(v_[i]);
  }

  size_t size() const { return live_; }
  size_t capacity() const { return v_.capacity(); }

private:
  static constexpr size_t npos = ~size_t{0};

  // The slot of open order `id`, or npos. Ids ascend along v_ (removed entries
  // keep theirs), so the order is at most id - v_[head_].id slots after the
  // oldest open one.
  size_t find(int64_t id) const {
    if (live_ == 0)
      return npos;
    const int64_t d = id - v_[head_].id;
    if (d < 0)
      return npos;
    size_t hi = v_.size() - 1 -
                head_; // offset from head_ of the last slot it can be in
    if (static_cast<uint64_t>(d) < hi)
      hi = static_cast<size_t>(d);
    const OpenOrder *b = v_.data() + head_;
    if (b[hi].id != id) {
      // Ids of other agents lie between: the first slot in [0, hi) whose id is
      // not below `id`.
      size_t base = 0, len = hi;
      while (len > 1) {
        const size_t half = len / 2;
        base += b[base + half - 1].id < id ? half : 0; // a select, not a jump
        len -= half;
      }
      if (len == 1) {
        base += b[base].id < id ? 1 : 0;
      }
      hi = base;
      if (b[hi].id != id)
        return npos;
    }
    return b[hi].gone ? npos : head_ + hi;
  }

  void remove(size_t k) {
    v_[k].gone = 1;
    if (--live_ == 0) { // empty: start again at the front
      v_.clear();
      head_ = 0;
    } else if (k == head_) { // the front moves to the oldest order still open
      do
        ++head_;
      while (v_[head_].gone);
    }
  }

  // Drops every removed entry; the open ones keep their order.
  void reclaim() {
    size_t w = 0;
    for (size_t i = head_; i < v_.size(); ++i)
      if (!v_[i].gone)
        v_[w++] = v_[i];
    v_.resize(w);
    head_ = 0;
  }

  std::vector<OpenOrder>
      v_; // ascending id; [head_, size) holds the open orders and removed ones
  size_t head_ = 0; // the oldest open order (when live_ > 0)
  size_t live_ = 0;
};

} // namespace t3
