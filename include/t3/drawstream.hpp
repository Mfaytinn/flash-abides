// NoiseTrader draws on a producer thread.
//
// A NoiseTrader act draws, in this order and unconditionally, size =
// round(normal(mean, std)) (at least 1), buy = randint(0, 2) and offset =
// randint(0, offset_ticks + 1) from the agent's own RandomState, which nothing
// else touches. Each agent's draw sequence is therefore a function of its seed
// alone; only how many acts consume it depends on the market. As with the
// latency stream, each agent's consumer side draws inline from the agent's own
// RNG until it reaches a block boundary the producer has already passed, then
// reads the producer's ring for good. Both sides run the same code from the
// same state, so the output never depends on thread timing.
//
// The ring is stored draw number by draw number, the agents' entries for one
// number side by side. The agents of a burst act one after another in the order
// their messages arrive, all on the same draw number, so their reads are
// neighbours (four to a cache line); an agent's successive draws are n entries
// apart (ring_step()).
#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>
#include <exception>
#include <memory>
#include <thread>
#include <vector>

#include "t3/arena.hpp"
#include "t3/parallel.hpp"
#include "t3/rng.hpp"

namespace t3 {

// Each draw is 16 bytes. The buy flag uses the top bit of the offset, which
// is never negative.
struct NoiseDraw {
  int64_t size;
  uint64_t offset_buy; // offset | buy << 63
  int64_t offset() const {
    return static_cast<int64_t>(offset_buy & ~(uint64_t{1} << 63));
  }
  bool buy() const { return (offset_buy >> 63) != 0; }
};
static_assert(sizeof(NoiseDraw) == 16);

struct NoiseParams {
  double mean = 0, std = 0;
  int64_t offset_ticks = 0;
};

// agents.py NoiseTrader: the three draws of one act, in order.
inline NoiseDraw draw_noise(LegacyRandom &rng, const NoiseParams &p) {
  NoiseDraw d;
  int64_t size = static_cast<int64_t>(py_round(rng.normal(p.mean, p.std)));
  if (size < 1)
    size = 1;
  d.size = size;
  const bool buy = rng.randint(0, 2) != 0;
  const int64_t offset =
      rng.randint(0, p.offset_ticks + 1); // non-negative: bit 63 is free
  d.offset_buy =
      static_cast<uint64_t>(offset) | (static_cast<uint64_t>(buy) << 63);
  return d;
}

class NoiseStreams {
public:
  static constexpr uint64_t kBlock = 64; // draws per block
  static constexpr uint64_t kBlocks = 8; // ring slots per agent

  // Where draw number `draw` of agent `slot` lies in a ring for n agents: the
  // ring is stored draw number by draw number, the n agents' entries for one
  // number side by side.
  static size_t ring_index(size_t n, size_t slot, uint64_t draw) {
    return (draw % (kBlock * kBlocks)) * n + slot;
  }
  // From one draw of an agent to its next. A block is kBlock consecutive draw
  // numbers, the ring a whole number of blocks: a block never wraps round the
  // ring's end.
  static size_t ring_step(size_t n) { return n; }

  NoiseStreams() = default;
  NoiseStreams(const NoiseStreams &) = delete;
  NoiseStreams &operator=(const NoiseStreams &) = delete;
  ~NoiseStreams() { stop(); }

  // One entry per NoiseTrader: its parameters and its RNG (seeded, not yet
  // drawn from). The inline side keeps using that RNG; the producer, if
  // `threaded`, works on a copy. Bytes of the arena carve-out init() takes for
  // n agents' rings (0: no producer thread).
  static size_t arena_bytes(size_t n, bool threaded) {
    return threaded && n ? Arena::need<NoiseDraw>(n * kBlock * kBlocks) : 0;
  }

  void init(std::vector<std::pair<NoiseParams, LegacyRandom *>> agents,
            bool threaded, Arena *arena = nullptr) {
    const size_t n = agents.size();
    st_.resize(n);
    pos_.resize(n);
    for (size_t s = 0; s < n; ++s) {
      st_[s].p = agents[s].first;
      st_[s].rng = agents[s].second;
    }
    if (!threaded || n == 0)
      return;
    threaded_ = true;
    step_ = ring_step(n);
    if (arena) {
      ring_ = arena->take<NoiseDraw>(
          n * kBlock * kBlocks); // populated: no page faults in the window
    } else {
      own_ring_.reset(
          new NoiseDraw[n * kBlock * kBlocks]()); // zeroed here: no page faults
                                                  // in the window
      ring_ = own_ring_.get();
    }
    done_.assign(n, 0); // allocated here: a malloc on the thread at go() would
                        // create its arena (an mmap) inside the window
    prod_done_.reset(new Counter[n]);
    cons_block_.reset(new Counter[n]);
    prod_rng_.reserve(n);
    for (size_t s = 0; s < n; ++s)
      prod_rng_.push_back(*agents[s].second);
    thread_ = std::thread([this] { produce(); });
  }

  void go() {
    go_.store(1, std::memory_order_seq_cst);
    futex_wake_all(go_);
  }
  void stop() {
    if (!thread_.joinable())
      return;
    stop_.store(true, std::memory_order_seq_cst);
    go();
    poke();
    thread_.join();
  }
  double producer_cpu_ms() const {
    return cpu_ms_;
  } // after stop(): CPU ms the producer used

  // Starts loading the agent's next draw (written by the producer on another
  // core), well before its act consumes it.
  void prefetch(uint32_t slot) const { __builtin_prefetch(pos_[slot].cur); }

  NoiseDraw next(uint32_t slot) {
    Pos &at = pos_[slot];
    if (at.cur != at.end) {

      const NoiseDraw d = *at.cur;
      at.cur += step_;
      return d;
    }
    return slow(slot);
  }

private:
  struct alignas(64) Counter {
    std::atomic<uint64_t> v{0};
  };
  // Consumer state read and written by every act, 16 bytes per agent.
  struct Pos {
    const NoiseDraw *cur =
        nullptr; // the agent's next draw in the ring; == end: see slow()
    const NoiseDraw *end = nullptr;
  };
  struct Stream {
    bool ring_mode = false;
    uint64_t next_block = 0;
    uint64_t inline_pos = 0;
    LegacyRandom *rng = nullptr;
    NoiseParams p;
  };

  NoiseDraw *block(size_t slot, uint64_t j) {
    return ring_ + ring_index(st_.size(), slot, j * kBlock);
  }

  NoiseDraw slow(uint32_t slot) {
    Stream &s = st_[slot];
    if (s.ring_mode) {
      const uint64_t j = s.next_block++;
      if (prod_done_[slot].v.load(std::memory_order_acquire) <= j)
          [[unlikely]] {
        poke(); // the producer may be idling between passes
        while (prod_done_[slot].v.load(std::memory_order_acquire) <= j) {
          if (failed_.load(std::memory_order_acquire))
            rethrow_failure();
          T3_POOL_PAUSE();
        }
      }
      Pos &at = pos_[slot];
      at.cur = block(slot, j);
      at.end = at.cur + kBlock * step_;
      cons_block_[slot].v.store(j, std::memory_order_release);

      const NoiseDraw d = *at.cur;
      at.cur += step_;
      return d;
    }
    if (threaded_ && s.inline_pos % kBlock == 0) {
      const uint64_t j = s.inline_pos / kBlock;
      cons_block_[slot].v.store(j, std::memory_order_release);
      if (prod_done_[slot].v.load(std::memory_order_acquire) > j) {
        s.ring_mode = true;
        s.next_block = j;
        return slow(slot);
      }
    }
    ++s.inline_pos;

    return draw_noise(*s.rng, s.p);
  }

  void poke() {
    wake_.fetch_add(1, std::memory_order_seq_cst);
    futex_wake_all(wake_);
  }

  // Round-robin passes, one block per agent that has ring space; an idle pass
  // naps briefly (a timed futex wait, cut short by poke()), so the thread costs
  // little when every ring is full. The producer thread. A failure (a draw its
  // model rejects) is recorded for the consumer, which rethrows it on the event
  // loop, so the run fails with an error instead of the process aborting.
  void produce() {
    try {
      produce_loop();
    } catch (...) {
      err_ = std::current_exception();
      failed_.store(true, std::memory_order_release);
    }
    cpu_ms_ = t3::thread_cpu_ms();
  }
  [[gnu::cold, gnu::noinline]] void rethrow_failure() {
    std::rethrow_exception(err_);
  }

  void produce_loop() {
    while (go_.load(std::memory_order_acquire) == 0)
      futex_wait(go_, 0);
    const size_t n = st_.size();
    std::vector<uint64_t> &done = done_;
    while (!stop_.load(std::memory_order_relaxed)) {
      bool worked = false;
      for (size_t slot = 0; slot < n; ++slot) {
        const uint64_t d = done[slot];
        if (d >= cons_block_[slot].v.load(std::memory_order_acquire) + kBlocks)
          continue;
        NoiseDraw *dst = block(slot, d);
        LegacyRandom &rng = prod_rng_[slot];
        const NoiseParams &p = st_[slot].p;
        for (uint64_t i = 0; i < kBlock; ++i)
          dst[i * step_] = draw_noise(rng, p);
        done[slot] = d + 1;
        prod_done_[slot].v.store(d + 1, std::memory_order_release);
        worked = true;
      }
      if (!worked) {
        const uint32_t seen = wake_.load(std::memory_order_seq_cst);
        if (stop_.load(std::memory_order_seq_cst))
          return;
        struct timespec ts{0, 50000}; // 50 us
        futex_wait_for(wake_, seen, ts);
      }
    }
  }

  std::vector<Stream> st_;
  std::vector<Pos> pos_;
  bool threaded_ = false;
  size_t step_ =
      1; // ring_step(n): ring entries from one draw of an agent to its next
  NoiseDraw *ring_ = nullptr;
  std::unique_ptr<NoiseDraw[]> own_ring_; // without an arena
  std::vector<uint64_t> done_;            // producer: blocks written per agent
  std::unique_ptr<Counter[]> prod_done_;
  std::unique_ptr<Counter[]> cons_block_;
  std::vector<LegacyRandom> prod_rng_;
  double cpu_ms_ = 0;
  std::thread thread_;
  alignas(64) std::atomic<uint32_t> go_{0};
  std::atomic<uint32_t> wake_{0};
  std::atomic<bool> stop_{false};
  std::atomic<bool> failed_{false}; // the producer failed: err_ holds why
  std::exception_ptr err_;
};

} // namespace t3
