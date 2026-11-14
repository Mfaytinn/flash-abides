// numpy legacy RandomState (MT19937) reproduced bit for bit.
//
// Every formula mirrors numpy 1.26 (the pinned baseline): mt19937.c,
// legacy-distributions.c and _bounded_integers.pyx. Floating-point expressions
// keep numpy's operation order; the build uses -ffp-contract=off so no FMA can
// change a rounding.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace t3 {

class MT19937 {
public:
  static constexpr int kN = 624;
  static constexpr int kM = 397;

  void seed(uint32_t s) {
    // numpy mt19937_seed == init_genrand.
    key_[0] = s;
    for (int i = 1; i < kN; ++i)
      key_[i] = 1812433253u * (key_[i - 1] ^ (key_[i - 1] >> 30)) +
                static_cast<uint32_t>(i);
    pos_ = kN;
  }

  uint32_t next32() {
    if (pos_ >= kN)
      gen();
    uint32_t y = key_[pos_++];
    y ^= (y >> 11);
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    y ^= (y >> 18);
    return y;
  }

  double next_double() {
    const int32_t a = static_cast<int32_t>(next32() >> 5);
    const int32_t b = static_cast<int32_t>(next32() >> 6);
    return (a * 67108864.0 + b) / 9007199254740992.0;
  }

private:
  [[gnu::noinline]] void gen() {
    if (pos_ == kUnseeded)
      seed(0);
    constexpr uint32_t kUpper = 0x80000000u, kLower = 0x7fffffffu,
                       kMatrix = 0x9908b0dfu;
    int i = 0;
    for (; i < kN - kM; ++i) {
      const uint32_t y = (key_[i] & kUpper) | (key_[i + 1] & kLower);
      key_[i] = key_[i + kM] ^ (y >> 1) ^ (-(y & 1u) & kMatrix);
    }
    for (; i < kN - 1; ++i) {
      const uint32_t y = (key_[i] & kUpper) | (key_[i + 1] & kLower);
      key_[i] = key_[i + (kM - kN)] ^ (y >> 1) ^ (-(y & 1u) & kMatrix);
    }
    const uint32_t y = (key_[kN - 1] & kUpper) | (key_[0] & kLower);
    key_[kN - 1] = key_[kM - 1] ^ (y >> 1) ^ (-(y & 1u) & kMatrix);
    pos_ = 0;
  }

  uint32_t key_[kN];
  // kUnseeded until seed() or the first draw: a default-constructed generator
  // is numpy's RandomState(0), but the 623-step recurrence that makes it so
  // runs only if something draws before seeding. The kernel reseeds
  // every agent's generator right after creating it.
  static constexpr int kUnseeded = kN + 1;
  int pos_ = kUnseeded;
};

class LegacyRandom {
public:
  LegacyRandom() =
      default; // RandomState(0), seeded at the first draw (MT19937::kUnseeded)
  explicit LegacyRandom(uint32_t seed) { reseed(seed); }

  void reseed(uint32_t seed) {
    mt_.seed(seed);
    has_gauss_ = false;
    gauss_ = 0.0;
  }

  uint32_t next32() { return mt_.next32(); }
  double next_double() { return mt_.next_double(); }

  // legacy_gauss: polar Box-Muller, returns f*x2 and caches f*x1.
  double gauss() {
    if (has_gauss_) {
      const double t = gauss_;
      has_gauss_ = false;
      gauss_ = 0.0;
      return t;
    }
    double f, x1, x2, r2;
    do {
      x1 = 2.0 * mt_.next_double() - 1.0;
      x2 = 2.0 * mt_.next_double() - 1.0;
      r2 = x1 * x1 + x2 * x2;
    } while (r2 >= 1.0 || r2 == 0.0);
    f = std::sqrt(-2.0 * std::log(r2) / r2);
    gauss_ = f * x1;
    has_gauss_ = true;
    return f * x2;
  }

  double normal(double loc, double scale) { return loc + scale * gauss(); }
  double lognormal(double mean, double sigma) {
    return std::exp(normal(mean, sigma));
  }
  double standard_exponential() { return -std::log(1.0 - mt_.next_double()); }
  double exponential(double scale) { return scale * standard_exponential(); }
  double pareto(double a) { return std::exp(standard_exponential() / a) - 1; }
  double uniform(double low, double high) {
    const double range = high - low;
    return low + range * mt_.next_double();
  }

  // RandomState.randint(low, high) for the default int64 dtype, scalar form:
  // masked rejection on 32-bit words when the range fits; a range of one value
  // consumes no randomness.
  int64_t randint(int64_t low, int64_t high) {
    const uint64_t rng = static_cast<uint64_t>(high - 1 - low);
    const uint64_t off = static_cast<uint64_t>(low);
    if (rng == 0)
      return low;
    if (rng <= 0xFFFFFFFFull) {
      if (rng == 0xFFFFFFFFull)
        return static_cast<int64_t>(off + mt_.next32());
      const uint32_t mask = static_cast<uint32_t>(gen_mask(rng));
      uint32_t val;
      while ((val = (mt_.next32() & mask)) > rng) {
      }
      return static_cast<int64_t>(off + val);
    }
    const uint64_t mask = gen_mask(rng);
    uint64_t val;
    do {
      const uint64_t hi = mt_.next32();
      val = ((hi << 32) | mt_.next32()) & mask;
    } while (val > rng);
    return static_cast<int64_t>(off + val);
  }

  // np.random.randint(low=0, high=2**32, dtype="uint64") as used for seed
  // derivation.
  uint32_t seed_draw() { return mt_.next32(); }

  bool has_gauss() const { return has_gauss_; }

  // `n` accepted pairs of the polar method, drawing exactly the doubles gauss()
  // draws for them (x1, then x2, rejecting r2 >= 1 or 0). The caller turns each
  // pair into its two normals, f * x2 first and f * x1 second, as gauss()
  // would. Only for a generator with no cached normal.
  void polar_pairs(size_t n, double *x1, double *x2, double *r2) {
    for (size_t p = 0; p < n; ++p) {
      double a, b, r;
      do {
        a = 2.0 * mt_.next_double() - 1.0;
        b = 2.0 * mt_.next_double() - 1.0;
        r = a * a + b * b;
      } while (r >= 1.0 || r == 0.0);
      x1[p] = a;
      x2[p] = b;
      r2[p] = r;
    }
  }

private:
  static uint64_t gen_mask(uint64_t max) {
    uint64_t mask = max;
    mask |= mask >> 1;
    mask |= mask >> 2;
    mask |= mask >> 4;
    mask |= mask >> 8;
    mask |= mask >> 16;
    mask |= mask >> 32;
    return mask;
  }

  MT19937 mt_;
  bool has_gauss_ = false;
  double gauss_ = 0.0;
};

// Python's round() on a float (and numpy float64.__round__): round half to
// even.
inline double py_round(double x) { return std::nearbyint(x); }

} // namespace t3
