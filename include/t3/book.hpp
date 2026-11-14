// The exchange's limit order book.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace t3 {

// ---------------------------------------------------------------------------------------------
// Order book: per side, price levels sorted with the best level at the back,
// and FIFO lists of resting orders. Resting orders live in a small pool of
// nodes, not in an array indexed by order id.
//
// Nearly all activity is at the best few levels, so a search looks at those
// first, one by one. Beyond those, a level is first looked for where it would
// sit if every tick between it and the best were quoted; gaps can only put it
// nearer the best. The remaining range is bisected without a data-dependent
// branch.
//
// A cancel finds its order through a small table of the most recently rested
// ids. ABIDES's OrderBook.cancel_order walks the level's FIFO, and so does this
// book when the table no longer holds the id.
//
// Empty levels deep inside the book remain available for reuse. Only the
// best level and those within kEagerShift are removed immediately, so the best
// always has orders. Stale levels are compacted at the kMinStale and
// kStaleToLive thresholds.
// ---------------------------------------------------------------------------------------------
constexpr uint32_t kNone = 0xFFFFFFFFu;

struct Level {
  int64_t price;
  int64_t total;
  uint32_t head; // pool slots
  uint32_t tail;
};

struct OrderNode {
  int64_t price;
  int64_t qty;
  uint32_t prev; // pool slots
  uint32_t next;
  uint32_t id; // order id
  uint16_t agent;
  uint8_t side;
  uint8_t pad;
};
static_assert(sizeof(OrderNode) == 32);

class Book {
public:
  static constexpr size_t npos = ~size_t{0};

  std::vector<Level> lv[2]; // [0] bids ascending, [1] asks descending: best =
                            // back(), a level with orders
  std::vector<OrderNode> pool; // resting orders, by slot
  std::vector<uint32_t> free;  // free slots, reused last-in first-out

  // The levels a search checks one by one, from the best, before it bisects the
  // rest.
  static constexpr size_t kNearLevels = 4;
  // A level that empties this close to the best is removed at once (at most
  // this many levels move); a level deeper in is left where it is, empty.
  static constexpr size_t kEagerShift = 4;
  // Empty levels are dropped together when there are more than kMinStale of
  // them and more than kStaleToLive times as many as the levels that hold
  // orders.
  static constexpr uint32_t kMinStale = 32;
  static constexpr uint32_t kStaleToLive = 2;
  // The id table of a book the kernel sets up: ids ascend, so the orders
  // resting from the last `id_slots` placed are found directly, and an older
  // order's entry has been overwritten (its cancel walks). A book that was
  // never given a table has one entry and walks for every cancel.
  static constexpr uint32_t kMinIdSlots = 1u << 10, kMaxIdSlots = 1u << 14;

  Book() : ids_(1, IdSlot{kNone, kNone}) {}

  // Capacity for `slots` resting orders, written once so its pages are faulted
  // in now, and an id table of `id_slots` entries (rounded up to a power of
  // two). Call it on an empty book.
  void reserve(size_t slots, uint32_t id_slots) {
    pool.resize(std::max(slots, pool.size()));
    pool.clear();
    free.resize(std::max(slots, free.size()));
    free.clear();
    uint32_t n = 1;
    while (n < id_slots && n < (1u << 30))
      n <<= 1;
    ids_.assign(n, IdSlot{kNone, kNone});
    id_mask_ = n - 1;
  }

  bool empty(uint8_t side) const { return lv[side].empty(); }
  // Levels of `side` kept in lv[side] with no orders (tests and diagnostics).
  size_t empty_levels(uint8_t side) const { return stale_[side]; }
  Level &best(uint8_t side) { return lv[side].back(); }

  // position order: bids ascending, asks descending
  static bool worse(uint8_t side, int64_t a, int64_t b) {
    return side == 0 ? a < b : a > b;
  }

  // Index of the level with `price`; npos if there is none. (A level that holds
  // no orders may be found: look at its head.)
  size_t find(uint8_t side, int64_t price) const {
    const std::vector<Level> &v = lv[side];
    size_t i = v.size();
    for (size_t near = std::min(i, kNearLevels); near > 0; --near) {
      --i;
      if (v[i].price == price)
        return i;
    }
    const size_t at = first_not_worse(side, price, i);
    return at < i && v[at].price == price ? at : npos;
  }

  // Slot of the resting order `id` in level li of `side`, or kNone. Order ids
  // are unique, so an id the table places in another level is not in this one.
  uint32_t find_order(uint8_t side, size_t li, uint32_t id) const {
    const IdSlot e = ids_[id & id_mask_];
    if (e.id == id) {
      const OrderNode &n = pool[e.slot];
      return n.side == side && n.price == lv[side][li].price ? e.slot : kNone;
    }
    for (uint32_t k = lv[side][li].head; k != kNone; k = pool[k].next) {
      if (pool[k].id == id)
        return k;
    }
    return kNone;
  }

  void append(uint8_t side, int64_t price, uint32_t id, int64_t qty,
              uint16_t agent) {
    std::vector<Level> &v = lv[side];

    // The common append: the price equals the best level's or beats it (a new
    // best level at the back). Found from the back pointer, with no size() and
    // no scan of the near levels; the generic code below reaches the same state
    // for these prices.
    if (!v.empty() && !worse(side, price, v.back().price)) [[likely]] {
      if (v.back().price != price)
        v.push_back(Level{price, 0, kNone, kNone});
      else if (v.back().head == kNone)
        --stale_[side]; // a level left empty takes an order again
      link_tail(v.back(), side, price, id, qty, agent);
      return;
    }

    // The position after every level that is not better than `price`: past the
    // near levels one by one, then by bisection.
    size_t i = v.size();
    size_t near = std::min(i, kNearLevels);
    while (near > 0 && worse(side, price, v[i - 1].price)) {
      --i;
      --near;
    }
    if (near == 0 && i > 0 && worse(side, price, v[i - 1].price)) {
      const size_t at = first_not_worse(side, price, i);
      i = at < i && v[at].price == price ? at + 1 : at;
    }
    if (i == 0 || v[i - 1].price != price) {
      v.insert(v.begin() + static_cast<std::ptrdiff_t>(i),
               Level{price, 0, kNone, kNone});
      ++i;
    } else if (v[i - 1].head == kNone) {
      --stale_[side]; // a level left empty takes an order again
    }
    link_tail(v[i - 1], side, price, id, qty, agent);
  }

  // Takes a pool slot for the order and links it at the tail of level L.
  [[gnu::always_inline]] void link_tail(Level &L, uint8_t side, int64_t price,
                                        uint32_t id, int64_t qty,
                                        uint16_t agent) {
    uint32_t k;
    if (!free.empty()) {
      k = free.back();
      free.pop_back();
    } else {
      k = static_cast<uint32_t>(pool.size());
      pool.emplace_back();
    }
    OrderNode &n = pool[k];
    n.price = price;
    n.qty = qty;
    n.prev = L.tail;
    n.next = kNone;
    n.id = id;
    n.agent = agent;
    n.side = side;
    n.pad = 0;
    if (L.tail == kNone)
      L.head = k;
    else
      pool[L.tail].next = k;
    L.tail = k;
    L.total += qty;
    ids_[id & id_mask_] = IdSlot{id, k};
  }

  // unlink_at for the order in slot k that heads the best level of `side` (a
  // full fill): the level is known (the back), the order has no predecessor,
  // and a level that empties is the last one, so the size() division, the side
  // lookup and the predecessor tests all go.
  void unlink_best_head(uint8_t side, uint32_t k) {
    std::vector<Level> &v = lv[side];
    Level &L = v.back();
    const OrderNode &n = pool[k];
    const uint32_t next = n.next;
    L.head = next;
    if (next == kNone)
      L.tail = kNone;
    else
      pool[next].prev = kNone;
    L.total -= n.qty;
    IdSlot &e = ids_[n.id & id_mask_];
    if (e.id == n.id)
      e = IdSlot{kNone, kNone}; // the slot is about to be reused
    free.push_back(k);
    if (next == kNone) {
      v.pop_back();
      // levels left empty that the best end has now reached go with it
      while (!v.empty() && v.back().head == kNone) {
        v.pop_back();
        --stale_[side];
      }
    }
  }

  // Unlinks the order in slot k from level index `li` of its side and frees the
  // slot. A level that empties is removed if it is the best or within
  // kEagerShift of it; otherwise it stays, empty, for the next order at its
  // price.
  void unlink_at(uint32_t k, size_t li) {
    OrderNode &n = pool[k];
    std::vector<Level> &v = lv[n.side];
    Level &L = v[li];
    if (n.prev == kNone)
      L.head = n.next;
    else
      pool[n.prev].next = n.next;
    if (n.next == kNone)
      L.tail = n.prev;
    else
      pool[n.next].prev = n.prev;
    L.total -= n.qty;
    IdSlot &e = ids_[n.id & id_mask_];
    if (e.id == n.id)
      e = IdSlot{kNone, kNone}; // the slot is about to be reused
    free.push_back(k);
    if (L.head == kNone) {
      const size_t after = v.size() - li - 1; // the levels better than it
      if (after == 0) {
        v.pop_back();
        // levels left empty that the best end has now reached go with it
        while (!v.empty() && v.back().head == kNone) {
          v.pop_back();
          --stale_[n.side];
        }
      } else if (after <= kEagerShift) {
        v.erase(v.begin() + static_cast<std::ptrdiff_t>(li));
      } else if (++stale_[n.side] > kMinStale &&
                 stale_[n.side] > kStaleToLive * (v.size() - stale_[n.side])) {
        compact(n.side);
      }
    }
  }

private:
  // Drops every level with no orders from lv[side]; the others keep their
  // order.
  void compact(uint8_t side) {
    std::vector<Level> &v = lv[side];
    size_t w = 0;
    for (size_t r = 0; r < v.size(); ++r)
      if (v[r].head != kNone)
        v[w++] = v[r];
    v.resize(w);
    stale_[side] = 0;
  }
  // The first index in v[0, n) whose level is not worse than `price`, n if all
  // of them are worse. (Position order: every level is worse than the next.)
  size_t first_not_worse(uint8_t side, int64_t price, size_t n) const {
    const std::vector<Level> &v = lv[side];
    if (n == 0)
      return 0;
    // Ticks from `price` up to the best of these levels. With every tick in
    // between quoted, the level sits that many places before it; with gaps,
    // nearer.
    const int64_t d =
        side == 0 ? v[n - 1].price - price : price - v[n - 1].price;
    if (d < 0)
      return n; // better than all of them
    size_t base =
        static_cast<uint64_t>(d) < n ? n - 1 - static_cast<size_t>(d) : 0;
    if (v[base].price == price)
      return base;
    size_t len = n - base; // the answer is in [base, base + len]
    while (len > 1) {
      const size_t half = len / 2;
      base += worse(side, v[base + half - 1].price, price)
                  ? half
                  : 0; // a select, not a jump
      len -= half;
    }
    return base + (worse(side, v[base].price, price) ? 1 : 0);
  }

  struct IdSlot {
    uint32_t id; // kNone: empty (no order has that id: a cancel's id is at most
                 // 0xFFFFFFFE)
    uint32_t slot; // where that order rests in `pool`
  };
  uint32_t stale_[2] = {0, 0}; // levels in lv[side] that hold no orders
  std::vector<IdSlot>
      ids_; // by id & id_mask_; holds an id only while that order rests
  uint32_t id_mask_ = 0;
};

} // namespace t3
