// Scenario latency model (config.py ScenarioLatencyModel) with an optional
// producer thread.
//
// The latency value sequence does not depend on the market: only which message
// consumes which value does. The producer thread computes the same sequence
// from the same seed into a ring of blocks. The consumer (the event loop)
// computes values inline until it reaches a block boundary the producer has
// already passed, then switches to the ring for good. Both sides compute
// identical values, so the output never depends on thread timing; the consumer
// never waits during start-up.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "t3/arena.hpp"
#include "t3/fastlog.hpp"
#include "t3/parallel.hpp"
#include "t3/rng.hpp"
#include "t3/scenario.hpp"

#if defined(__x86_64__)
#include <immintrin.h>
#define T3_PAUSE() _mm_pause()
#else
#define T3_PAUSE() ((void)0)
#endif

namespace t3 {

class LatencyModelCore {
public:
  void init(const Scenario &s, uint32_t seed) {
    model_ = s.latency_model;
    mu_ = s.lat_mu;
    sigma_ = s.lat_sigma;
    min_ = s.lat_min_ns;
    max_ = s.lat_max_ns;
    alpha_ = s.lat_alpha;
    base_ = s.lat_min_ns > 0 ? s.lat_min_ns : 1.0;
    rng_.reseed(seed);
    if (model_ == LatencyModel::Constant)
      const_value_ = finish(s.lat_mean_ns);
#ifdef T3_FASTLOG
    static const bool disabled = std::getenv("T3_NO_FASTLOG") != nullptr;
    // The vector path decides the rounded integer only; it needs the clip to
    // keep it in range (so finish() cannot throw) and finite parameters (the
    // error bound assumes |mu|, |sigma| <= 1e3).
    fast_ = !disabled && model_ == LatencyModel::LogNormal &&
            std::isfinite(mu_) && std::isfinite(sigma_) &&
            std::fabs(mu_) <= 1e3 && std::fabs(sigma_) <= 1e3 && min_ >= 0.0 &&
            min_ <= max_ && max_ <= 9.0e18;
#endif
  }
  bool is_constant() const { return model_ == LatencyModel::Constant; }
  uint64_t constant() const { return const_value_; }

  uint64_t next() {
    double v;
    switch (model_) {
    case LatencyModel::LogNormal:
      v = rng_.lognormal(mu_, sigma_);
      break;
    case LatencyModel::Uniform:
      v = rng_.uniform(min_, max_);
      break;
    case LatencyModel::Pareto:
      v = base_ * (1.0 + rng_.pareto(alpha_));
      break;
    default:
      return const_value_;
    }
    return finish(v);
  }

  // out[0, n) = the values n successive next() calls give, and the generator
  // ends in the same state. Lognormal draws take the vector path (below);
  // everything else is the loop.
  void fill(uint64_t *out, size_t n) {
#ifdef T3_FASTLOG
    if (fast_) {
      fill_lognormal(out, n);
      return;
    }
#endif
    for (size_t i = 0; i < n; ++i)
      out[i] = next();
  }

private:
#ifdef T3_FASTLOG
  // The two outputs of one polar pair, as gauss() and lognormal() compute them
  // with libm.
  void exact_pair(double a, double b, double r2, uint64_t *first,
                  uint64_t *second) const {
    const double f = std::sqrt(-2.0 * std::log(r2) / r2);
    *first = finish(std::exp(mu_ + sigma_ * (f * b)));
    *second = finish(std::exp(mu_ + sigma_ * (f * a)));
  }

  // Four lognormal values from four normals g: the rounded latency, and which
  // lanes the error bound cannot decide. v = exp(mu + sigma g) with a relative
  // error below 8x (|sigma g| + |x| + 2) 2^-50 (the bounds of fl_log and fl_exp
  // pass through the arithmetic). A lane is decided only if v is further than
  // that error from the clip bounds and from a half-integer, so the integer is
  // the one the exact value gives.
  void lognormal4(__m256d g, double *rounded, int *bad) const {
    const __m256d sg = _mm256_mul_pd(_mm256_set1_pd(sigma_), g);
    const __m256d x = _mm256_add_pd(_mm256_set1_pd(mu_), sg);
    const __m256d sign = _mm256_set1_pd(-0.0);
    const __m256d ax = _mm256_andnot_pd(sign, x);
    const __m256d out_of_range =
        _mm256_cmp_pd(ax, _mm256_set1_pd(700.0), _CMP_NLE_UQ); // also NaN
    const __m256d v =
        fl_exp(_mm256_blendv_pd(x, _mm256_setzero_pd(), out_of_range));
    const __m256d rel = _mm256_mul_pd(
        _mm256_add_pd(_mm256_add_pd(_mm256_andnot_pd(sign, sg), ax),
                      _mm256_set1_pd(2.0)),
        _mm256_set1_pd(T3_FASTLOG_GUARD));
    const __m256d delta = _mm256_mul_pd(v, rel);
    const __m256d lo = _mm256_set1_pd(min_), hi = _mm256_set1_pd(max_);
    // One rounded difference per bound, compared with delta in both directions,
    // so below / near / above partition the line: a double d > delta means the
    // exact difference exceeds delta (rounding is monotone and delta is a
    // double). Comparing v with the rounded lo - delta instead leaves a sliver
    // where that sum rounds up past v and a half-integer bound is clipped the
    // wrong way.
    const __m256d dlo = _mm256_sub_pd(lo, v), dhi = _mm256_sub_pd(v, hi);
    const __m256d below = _mm256_cmp_pd(dlo, delta, _CMP_GT_OQ);
    const __m256d above = _mm256_cmp_pd(dhi, delta, _CMP_GT_OQ);
    const __m256d near_lo =
        _mm256_cmp_pd(_mm256_andnot_pd(sign, dlo), delta, _CMP_LE_OQ);
    const __m256d near_hi =
        _mm256_cmp_pd(_mm256_andnot_pd(sign, dhi), delta, _CMP_LE_OQ);
    const __m256d n =
        _mm256_round_pd(v, _MM_FROUND_TO_NEAREST_INT |
                               _MM_FROUND_NO_EXC); // nearbyint: ties to even
    const __m256d d = _mm256_andnot_pd(sign, _mm256_sub_pd(v, n));
    const __m256d half =
        _mm256_cmp_pd(d, _mm256_sub_pd(_mm256_set1_pd(0.5), delta), _CMP_GE_OQ);
    const __m256d interior =
        _mm256_andnot_pd(_mm256_or_pd(below, above),
                         _mm256_castsi256_pd(_mm256_set1_epi64x(-1)));
    const __m256d unsure = _mm256_or_pd(
        out_of_range,
        _mm256_and_pd(interior,
                      _mm256_or_pd(_mm256_or_pd(near_lo, near_hi), half)));
    __m256d r = _mm256_blendv_pd(
        n, _mm256_round_pd(lo, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC),
        below);
    r = _mm256_blendv_pd(
        r, _mm256_round_pd(hi, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC),
        above);
    _mm256_storeu_pd(rounded, r);
    *bad = _mm256_movemask_pd(unsure);
  }

  void fill_lognormal(uint64_t *out, size_t n) {
    size_t i = 0;
    while (i < n && rng_.has_gauss())
      out[i++] = next(); // a normal cached by an earlier call
    while (n - i >= 2) {
      const size_t pairs = std::min<size_t>((n - i) / 2, 4);
      alignas(32) double a[4] = {0.5, 0.5, 0.5, 0.5},
                         b[4] = {0.5, 0.5, 0.5, 0.5},
                         r2[4] = {0.5, 0.5, 0.5, 0.5};
      rng_.polar_pairs(pairs, a, b,
                       r2); // lanes past `pairs` hold harmless values
      const __m256d va = _mm256_load_pd(a), vb = _mm256_load_pd(b),
                    vr = _mm256_load_pd(r2);
      // f = sqrt(-2 log(r2) / r2): the division and the square root are
      // correctly rounded
      const __m256d f = _mm256_sqrt_pd(
          _mm256_div_pd(_mm256_mul_pd(_mm256_set1_pd(-2.0), fl_log(vr)), vr));
      alignas(32) double first[4], second[4];
      int bad1, bad2;
      lognormal4(_mm256_mul_pd(f, vb), first,
                 &bad1); // gauss() returns f * x2 first ...
      lognormal4(_mm256_mul_pd(f, va), second,
                 &bad2); // ... and f * x1 on the next call
      const int bad = bad1 | bad2;
      for (size_t p = 0; p < pairs; ++p) {

        if (bad >> p & 1) {
          exact_pair(a[p], b[p], r2[p], &out[i + 2 * p], &out[i + 2 * p + 1]);
        } else {
          out[i + 2 * p] = static_cast<uint64_t>(first[p]);
          out[i + 2 * p + 1] = static_cast<uint64_t>(second[p]);
        }
      }
      i += 2 * pairs;
    }
    if (i < n)
      out[i++] =
          next(); // one left: the scalar path caches the other half of its pair
  }
#endif

  uint64_t finish(double v) const {
    // int(round(float(np.clip(v, min, max)))): numpy clip == minimum(maximum(v,
    // lo), hi).
    double c = v;
    if (c < min_)
      c = min_;
    if (c > max_)
      c = max_;
    const double r = py_round(c);
    if (!(r >= 0.0) || r > 9.2e18)
      throw std::runtime_error("latency out of range");
    return static_cast<uint64_t>(r);
  }

  LatencyModel model_ = LatencyModel::Constant;
  double mu_ = 0, sigma_ = 0, min_ = 0, max_ = 0, alpha_ = 1.5, base_ = 1;
  uint64_t const_value_ = 0;
  bool fast_ = false; // fill() may use the vector path
  LegacyRandom rng_;
};

class LatencyStream {
public:
  static constexpr uint64_t kBlock = 1024; // values per block
  static constexpr uint64_t kBlocks = 64;  // ring slots (512 KiB)
  // The consumer wakes a sleeping producer only every kNotifyEvery blocks, so a
  // producer that runs ahead costs about one futex wake per kNotifyEvery
  // blocks. It sleeps only on a full ring, so the consumer always has kBlocks -
  // kNotifyEvery blocks in hand when it wakes it.
  static constexpr uint64_t kNotifyEvery = 32;
  static_assert(kNotifyEvery < kBlocks);
  static constexpr uint64_t kInline =
      64; // draws per refill when drawing inline
  static_assert(kBlock % kInline == 0);

  LatencyStream() = default;
  LatencyStream(const LatencyStream &) = delete;
  LatencyStream &operator=(const LatencyStream &) = delete;
  ~LatencyStream() { stop_thread(); }

  // Bytes of the arena carve-out init() takes for the ring (0: no producer
  // thread).
  static size_t arena_bytes(const Scenario &s, bool threaded) {
    return threaded && s.latency_model != LatencyModel::Constant &&
                   s.latency_model != LatencyModel::Pairwise
               ? Arena::need<uint64_t>(kBlock * kBlocks)
               : 0;
  }

  // Initializes the inline model; if `threaded` and the model draws random
  // numbers, also starts a parked producer thread (it begins computing only
  // after go()).
  void init(const Scenario &s, uint32_t seed, bool threaded,
            Arena *arena = nullptr) {
    if (s.latency_model == LatencyModel::Pairwise) {
      init_pairwise(s.agents.size() + 1, seed);
      return;
    }
    inline_.init(s, seed);
    if (inline_.is_constant()) {
      constant_ = true;
      const_value_ = inline_.constant();
      ibuf_.reset(new uint64_t[kInline]);
      for (uint64_t i = 0; i < kInline; ++i)
        ibuf_[i] = const_value_;
      return;
    }
    ibuf_.reset(new uint64_t[kInline]());
    if (!threaded)
      return;
    producer_model_.init(s, seed);
    if (arena) {
      ring_ = arena->take<uint64_t>(
          kBlock * kBlocks); // populated: no page faults in the window
    } else {
      own_ring_.reset(new uint64_t[kBlock * kBlocks]()); // zeroed here: no page
                                                         // faults in the window
      ring_ = own_ring_.get();
    }
    thread_ = std::thread([this] { produce(); });
  }

  // Releases the producer; call right before the timed window.
  void go() {
    go_.store(1, std::memory_order_seq_cst);
    futex_wake_all(go_);
  }

  // Stops and joins the producer; call after the timed window.
  void stop_thread() {
    if (thread_.joinable()) {
      stop_.store(true, std::memory_order_seq_cst);
      go();
      wake_.fetch_add(1, std::memory_order_seq_cst);
      futex_wake_all(wake_);
      thread_.join();
    }
  }

  // The latency of the next message, from sender s to recipient r (only the
  // pairwise model looks at them).
  uint64_t next(uint64_t s, uint64_t r) {
    if (cur_ != end_)
      return *cur_++;
    return slow_next(s, r);
  }

  // Diagnostics: blocks the consumer had to wait for; the block where it
  // switched to the ring.
  uint64_t stalls() const { return stalls_; }
  double producer_cpu_ms() const {
    return cpu_ms_;
  } // after stop_thread(): CPU ms the producer used
  uint64_t switch_block() const {
    return ring_mode_ ? switch_block_ : ~uint64_t{0};
  }
  uint64_t blocks_consumed() const {
    return ring_mode_ ? next_block_ : inline_pos_ / kBlock;
  }

private:
  // Out of line: inlined into its callers (the kernel's send), its vector RNG
  // code forces them to realign their stack and spill registers on every call.
  [[gnu::noinline]] uint64_t slow_next(uint64_t s, uint64_t r) {
    if (pairwise_)
      return pair_ ? pair_[s * pair_n_ + r] : pair_latency(s, r);
    if (constant_) { // a block of the constant: one call per kInline sends
      cur_ = ibuf_.get();
      end_ = cur_ + kInline;
      return *cur_++;
    }
    if (ring_mode_) {
      const uint64_t j = next_block_++;
      if (prod_done_.load(std::memory_order_acquire) <= j) [[unlikely]] {
        ++stalls_;
        while (prod_done_.load(std::memory_order_acquire) <= j) {
          if (failed_.load(std::memory_order_acquire))
            rethrow_failure();
          T3_PAUSE();
        }
      }
      cur_ = ring_ + (j % kBlocks) * kBlock;
      end_ = cur_ + kBlock;
      cons_block_.store(j, std::memory_order_seq_cst);
      if (j % kNotifyEvery == 0)
        wake_producer();
      return *cur_++;
    }
    if (ring_ && inline_pos_ % kBlock == 0) {
      const uint64_t j = inline_pos_ / kBlock;
      cons_block_.store(j, std::memory_order_seq_cst);
      if (j % kNotifyEvery == 0)
        wake_producer();
      if (prod_done_.load(std::memory_order_acquire) > j) {
        ring_mode_ = true;
        switch_block_ = j;
        next_block_ = j;
        return slow_next(s, r);
      }
    }
    // Inline: the model's next kInline draws at once (the latency RNG draws
    // nothing else, so drawing ahead yields the same sequence). kInline divides
    // kBlock, so the switch check above still runs exactly at block boundaries.
    uint64_t *b = ibuf_.get();
    inline_.fill(b, kInline);
    inline_pos_ += kInline;
    cur_ = b;
    end_ = b + kInline;
    return *cur_++;
  }

  // abides_markets.utils.generate_latency_model(n) ("deterministic"): the n
  // agents sit at uniform random points on a line from Seattle to NYC (a
  // RandomState seeded with the master draw, like the configured models); a
  // message takes the light travel time between its sender and its recipient,
  // truncated to whole ns: int(pdist(x) / 299792458e-9). Up to kPairTable
  // agents the n x n table is built once; above that (8 * n^2 bytes: 3.2 GB at
  // 20k agents) each latency is computed when needed, by the same expression,
  // so the values are identical. T3_PAIRWISE=table|compute forces either way
  // (tests).
  static constexpr size_t kPairTable = 2048;
  void init_pairwise(size_t n, uint32_t seed) {
    LegacyRandom rng(seed);
    pair_x_.resize(n);
    for (size_t i = 0; i < n; ++i)
      pair_x_[i] = rng.uniform(0.0, 3866660.0);
    const char *force = std::getenv("T3_PAIRWISE");
    const bool table = force ? std::string(force) == "table" : n <= kPairTable;
    if (table) {
      pair_.reset(new uint64_t[n * n]);
      for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j)
          pair_[i * n + j] = pair_latency(i, j);
    }
    pair_n_ = n;
    pairwise_ = true;
  }
  uint64_t pair_latency(uint64_t i, uint64_t j) const {
    const double d = pair_x_[i] - pair_x_[j];
    const double dist =
        std::sqrt(d * d); // scipy pdist, "euclidean", on 1-D points
    return static_cast<uint64_t>(static_cast<int64_t>(dist / 299792458e-9));
  }

  // The consumer's side of the sleep protocol: a wake syscall only when the
  // producer sleeps.
  void wake_producer() {
    wake_.fetch_add(1, std::memory_order_seq_cst);
    if (sleeping_.load(std::memory_order_seq_cst))
      futex_wake_all(wake_);
  }

  // The producer thread. A failure (a draw its model rejects) is recorded for
  // the consumer, which rethrows it on the event loop, so the run fails with an
  // error instead of the process aborting.
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
      futex_wait(go_, 0); // parked until the window
    for (uint64_t j = 0;; ++j) {
      for (int spins = 0;; ++spins) {
        const uint64_t c = cons_block_.load(std::memory_order_seq_cst);
        if (j < c + kBlocks)
          break;
        if (stop_.load(std::memory_order_relaxed))
          return;
        if (spins < 64) {
          T3_PAUSE();
          continue;
        }
        // Ring full: sleep until the consumer frees blocks (it wakes us every
        // kNotifyEvery blocks). Register, then re-check, so a wake between the
        // check and the sleep is not lost; the kernel also re-checks wake_
        // against `seen`.
        const uint32_t seen = wake_.load(std::memory_order_seq_cst);
        sleeping_.store(1, std::memory_order_seq_cst);
        if (cons_block_.load(std::memory_order_seq_cst) == c &&
            !stop_.load(std::memory_order_seq_cst))
          futex_wait(wake_, seen);
        sleeping_.store(0, std::memory_order_relaxed);
      }
      if (stop_.load(std::memory_order_relaxed))
        return;
      uint64_t *dst = ring_ + (j % kBlocks) * kBlock;
      producer_model_.fill(dst, kBlock);
      prod_done_.store(j + 1, std::memory_order_release);
    }
  }

  // consumer side (event-loop thread)
  const uint64_t *cur_ = nullptr;
  const uint64_t *end_ = nullptr;
  bool constant_ = false;
  uint64_t const_value_ = 0;
  bool ring_mode_ = false;
  uint64_t next_block_ = 0;
  uint64_t inline_pos_ = 0;
  uint64_t stalls_ = 0, switch_block_ = 0;
  LatencyModelCore inline_;
  std::unique_ptr<uint64_t[]>
      ibuf_;              // inline draws (or the constant), kInline of them
  bool pairwise_ = false; // the default model: latencies by (sender, recipient)
  std::unique_ptr<uint64_t[]>
      pair_; // the n x n table, or empty (computed per message)
  std::vector<double> pair_x_; // each agent's position on the line
  size_t pair_n_ = 0;

  // shared
  uint64_t *ring_ = nullptr;
  std::unique_ptr<uint64_t[]> own_ring_; // without an arena
  alignas(64) std::atomic<uint64_t> prod_done_{0};
  alignas(64) std::atomic<uint64_t> cons_block_{0};
  alignas(64) std::atomic<uint32_t> wake_{0};
  std::atomic<uint32_t> sleeping_{0};
  alignas(64) std::atomic<uint32_t> go_{0};
  std::atomic<bool> stop_{false};
  std::atomic<bool> failed_{false}; // the producer failed: err_ holds why
  std::exception_ptr err_;

  // producer side
  alignas(64) LatencyModelCore producer_model_;
  double cpu_ms_ = 0;
  std::thread thread_;
};

} // namespace t3
