// Vector log and exp for the latency producer's lognormal draws, with the error
// bounds the callers rely on. The engine must give exactly the numbers numpy's
// RandomState gives, so these are never used for a value that reaches the
// output: they only decide an integer (the rounded latency) when the value is
// further from every rounding boundary than a proven error bound; otherwise the
// caller recomputes that draw with the scalar libm code from the same random
// inputs (latency.hpp).
//
// Bounds (relative error of the result against the exact function, checked
// against long double over millions of inputs, with at least 4x margin):
//   fl_log: < 2^-50 for a normal positive argument (the argument is in (0, 1)
//   here) fl_exp: < 2^-51 for |x| <= 700
#pragma once

#if defined(__AVX2__) && defined(__FMA__)
#define T3_FASTLOG 1
// The relative margin a lane must clear: 8x the error bound's coefficient
// (tests widen it to force the exact fallback: -DT3_FASTLOG_GUARD=1e-3).
#ifndef T3_FASTLOG_GUARD
#define T3_FASTLOG_GUARD 7.105427357601002e-15 // 2^-47
#endif
#include <immintrin.h>

#include <cstdint>

namespace t3 {

// Natural log of four normal positive doubles. Reduction x = m * 2^e with m in
// [sqrt(2)/2, sqrt(2)), then log(m) = 2 s (1 + z/3 + z^2/5 + ... + z^9/19) with
// s = (m - 1)/(m + 1), z = s^2 <= 0.0295 (truncation 2^-55 of the sum); log(x)
// = e ln2 + log(m), where the two terms cannot cancel
// (|log m| <= 0.347 < ln2).
inline __m256d fl_log(__m256d x) {
  const __m256i bits = _mm256_castpd_si256(x);
  // exponent field minus the bias, as a double: add a bias that keeps the
  // integer positive and use the 2^52 trick to convert without AVX-512
  const __m256i ebits =
      _mm256_srli_epi64(bits, 52); // sign bit is zero for a positive x
  const __m256d magic = _mm256_set1_pd(4503599627370496.0); // 2^52
  __m256d e = _mm256_sub_pd(
      _mm256_castsi256_pd(_mm256_or_si256(ebits, _mm256_castpd_si256(magic))),
      _mm256_set1_pd(4503599627370496.0 + 1023.0));
  __m256d m = _mm256_castsi256_pd(_mm256_or_si256(
      _mm256_and_si256(bits, _mm256_set1_epi64x(0x000FFFFFFFFFFFFFll)),
      _mm256_set1_epi64x(0x3FF0000000000000ll))); // [1, 2)
  const __m256d big =
      _mm256_cmp_pd(m, _mm256_set1_pd(1.4142135623730951), _CMP_GT_OQ);
  m = _mm256_blendv_pd(m, _mm256_mul_pd(m, _mm256_set1_pd(0.5)),
                       big); // [sqrt(2)/2, sqrt(2))
  e = _mm256_add_pd(e, _mm256_and_pd(big, _mm256_set1_pd(1.0)));
  const __m256d one = _mm256_set1_pd(1.0);
  const __m256d s = _mm256_div_pd(_mm256_sub_pd(m, one), _mm256_add_pd(m, one));
  const __m256d z = _mm256_mul_pd(s, s);
  __m256d p = _mm256_set1_pd(1.0 / 19.0);
  p = _mm256_fmadd_pd(p, z, _mm256_set1_pd(1.0 / 17.0));
  p = _mm256_fmadd_pd(p, z, _mm256_set1_pd(1.0 / 15.0));
  p = _mm256_fmadd_pd(p, z, _mm256_set1_pd(1.0 / 13.0));
  p = _mm256_fmadd_pd(p, z, _mm256_set1_pd(1.0 / 11.0));
  p = _mm256_fmadd_pd(p, z, _mm256_set1_pd(1.0 / 9.0));
  p = _mm256_fmadd_pd(p, z, _mm256_set1_pd(1.0 / 7.0));
  p = _mm256_fmadd_pd(p, z, _mm256_set1_pd(1.0 / 5.0));
  p = _mm256_fmadd_pd(p, z, _mm256_set1_pd(1.0 / 3.0));
  p = _mm256_fmadd_pd(p, z, one);
  const __m256d logm = _mm256_mul_pd(_mm256_add_pd(s, s), p);
  // e * ln2 in two parts (ln2_hi has trailing zero bits, so e * ln2_hi is exact
  // for |e| < 2^11)
  const __m256d r =
      _mm256_fmadd_pd(e, _mm256_set1_pd(1.90821492927058770002e-10), logm);
  return _mm256_fmadd_pd(e, _mm256_set1_pd(6.93147180369123816490e-01), r);
}

// exp of four doubles with |x| <= 700 (the caller checks the range): x = k ln2
// + r, |r| <= 0.347, exp(r) by its Taylor polynomial of degree 13 (truncation
// 2^-57), scaled by 2^k.
inline __m256d fl_exp(__m256d x) {
  const __m256d kd =
      _mm256_round_pd(_mm256_mul_pd(x, _mm256_set1_pd(1.4426950408889634)),
                      _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
  __m256d r =
      _mm256_fnmadd_pd(kd, _mm256_set1_pd(6.93147180369123816490e-01), x);
  r = _mm256_fnmadd_pd(kd, _mm256_set1_pd(1.90821492927058770002e-10), r);
  __m256d p = _mm256_set1_pd(1.0 / 6227020800.0); // 1/13!
  p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(1.0 / 479001600.0));
  p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(1.0 / 39916800.0));
  p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(1.0 / 3628800.0));
  p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(1.0 / 362880.0));
  p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(1.0 / 40320.0));
  p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(1.0 / 5040.0));
  p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(1.0 / 720.0));
  p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(1.0 / 120.0));
  p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(1.0 / 24.0));
  p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(1.0 / 6.0));
  p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(0.5));
  p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(1.0));
  p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(1.0));
  const __m256i k = _mm256_cvtepi32_epi64(_mm256_cvtpd_epi32(kd));
  const __m256d scale = _mm256_castsi256_pd(
      _mm256_slli_epi64(_mm256_add_epi64(k, _mm256_set1_epi64x(1023)), 52));
  return _mm256_mul_pd(p, scale);
}

} // namespace t3
#endif
