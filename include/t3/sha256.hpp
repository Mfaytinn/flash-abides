// SHA-256 (FIPS 180-4), streaming interface. Used to compute trace_sha256
// for events.json: the parquet writer hashes its own output bytes as they
// are flushed, so the digest matches the file on disk exactly.
//
// Uses SHA-NI when available, selected once per process with a scalar fallback.
// The block schedule follows the canonical msg1/msg2/rnds2 interleaving (public
// domain reference: J. Walton, sha256-x86.c, after Intel/Sean Gulley);
// the digest is bit-identical to the scalar FIPS code either way.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#if defined(__clang__)
#include <cpuid.h>
#endif
#define T3_HAVE_SHA_NI 1
#else
#define T3_HAVE_SHA_NI 0
#endif

class Sha256 {
public:
  Sha256() {
    state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
              0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  }

  void update(const void *data, size_t len) {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    total_ += len;
    if (buf_len_ > 0) {
      size_t take = kBlockSize - buf_len_;
      if (take > len)
        take = len;
      std::memcpy(buf_.data() + buf_len_, p, take);
      buf_len_ += take;
      p += take;
      len -= take;
      if (buf_len_ == kBlockSize) {
        compress(buf_.data());
        buf_len_ = 0;
      }
    }
    while (len >= kBlockSize) {
      compress(p);
      p += kBlockSize;
      len -= kBlockSize;
    }
    if (len > 0) {
      std::memcpy(buf_.data(), p, len);
      buf_len_ = static_cast<uint32_t>(len);
    }
  }

  // Finalizes and returns the 32-byte digest (idempotent-ish: repeated
  // calls re-derive from the snapshot taken on first call).
  std::array<uint8_t, 32> digest() {
    if (!finalized_) {
      uint64_t bit_len = total_ * 8;
      uint8_t pad = 0x80;
      update(&pad, 1);
      uint8_t zero = 0;
      while (buf_len_ != 56)
        update(&zero, 1);
      uint8_t len_be[8];
      for (int i = 0; i < 8; ++i)
        len_be[i] = static_cast<uint8_t>(bit_len >> (56 - 8 * i));
      // Bypass update()'s total_ accounting for the padding itself.
      std::memcpy(buf_.data() + 56, len_be, 8);
      compress(buf_.data());
      buf_len_ = 0;
      finalized_ = true;
    }
    std::array<uint8_t, 32> out;
    for (int i = 0; i < 8; ++i) {
      out[i * 4 + 0] = static_cast<uint8_t>(state_[i] >> 24);
      out[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16);
      out[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8);
      out[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
    }
    return out;
  }

  static std::string hex(const std::array<uint8_t, 32> &d) {
    static const char *kHex = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (uint8_t b : d) {
      s.push_back(kHex[b >> 4]);
      s.push_back(kHex[b & 0xf]);
    }
    return s;
  }

private:
  static constexpr uint32_t kBlockSize = 64;

  static uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
  }

  void compress(const uint8_t *block) {
#if T3_HAVE_SHA_NI
    if (sha_ni_available()) {
      compress_ni(block);
      return;
    }
#endif
    compress_sw(block);
  }

  static const uint32_t *round_constants() {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    return k;
  }

  void compress_sw(const uint8_t *block) {
    const uint32_t *k = round_constants();
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = (uint32_t(block[i * 4]) << 24) |
             (uint32_t(block[i * 4 + 1]) << 16) |
             (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (int i = 0; i < 64; ++i) {
      uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      uint32_t ch = (e & f) ^ (~e & g);
      uint32_t t1 = h + S1 + ch + k[i] + w[i];
      uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = S0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

#if T3_HAVE_SHA_NI
  // One-time CPUID check, cached; safe to call from any thread
  // (function-local statics initialize exactly once).
  static bool sha_ni_available() {
    static const bool v = [] {
#if defined(__clang__)
      // Clang 18 rejects "sha" in __builtin_cpu_supports. Query the
      // same feature bit directly; missing CPUID leaves the scalar path.
      unsigned int eax, ebx, ecx, edx;
      return __get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) && (ebx & bit_SHA);
#else
      __builtin_cpu_init();
      return __builtin_cpu_supports("sha") != 0;
#endif
    }();
    return v;
  }

  // SHA-NI block transform. Same input/output contract as compress_sw
  // (folds one 64-byte big-endian block into state_).
  // Compiled with SHA instructions via the function target regardless of
  // the TU's -march, and only ever CALLED after the CPUID gate above.
  __attribute__((target("ssse3,sse4.1,sha"))) void
  compress_ni(const uint8_t *block) {
    const uint32_t *k = round_constants();
    const __m128i MASK =
        _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
    __m128i STATE0, STATE1, MSG, TMP, M[4], ABEF_SAVE, CDGH_SAVE;

    TMP = _mm_loadu_si128(reinterpret_cast<const __m128i *>(&state_[0]));
    STATE1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(&state_[4]));
    TMP = _mm_shuffle_epi32(TMP, 0xB1);          // CDAB
    STATE1 = _mm_shuffle_epi32(STATE1, 0x1B);    // EFGH
    STATE0 = _mm_alignr_epi8(TMP, STATE1, 8);    // ABEF
    STATE1 = _mm_blend_epi16(STATE1, TMP, 0xF0); // CDGH

    const __m128i *B = reinterpret_cast<const __m128i *>(block);
    M[0] = _mm_shuffle_epi8(_mm_loadu_si128(B + 0), MASK);
    M[1] = _mm_shuffle_epi8(_mm_loadu_si128(B + 1), MASK);
    M[2] = _mm_shuffle_epi8(_mm_loadu_si128(B + 2), MASK);
    M[3] = _mm_shuffle_epi8(_mm_loadu_si128(B + 3), MASK);

    ABEF_SAVE = STATE0;
    CDGH_SAVE = STATE1;
    // Fixed indices let the compiler keep the four message vectors in
    // registers.
#pragma GCC unroll 16
    for (int g = 0; g < 16; ++g) {
      const int x = g & 3;
      MSG = _mm_add_epi32(M[x], _mm_set_epi32(static_cast<int>(k[4 * g + 3]),
                                              static_cast<int>(k[4 * g + 2]),
                                              static_cast<int>(k[4 * g + 1]),
                                              static_cast<int>(k[4 * g])));
      STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
      MSG = _mm_shuffle_epi32(MSG, 0x0E);
      STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
      if (g >= 3 && g <= 14) {
        const int p = (g - 1) & 3, n = (g + 1) & 3;
        TMP = _mm_alignr_epi8(M[x], M[p], 4);
        M[n] = _mm_add_epi32(M[n], TMP);
        M[n] = _mm_sha256msg2_epu32(M[n], M[x]);
      }
      if (g >= 1 && g <= 12) {
        const int p = (g - 1) & 3;
        M[p] = _mm_sha256msg1_epu32(M[p], M[x]);
      }
    }
    STATE0 = _mm_add_epi32(STATE0, ABEF_SAVE);
    STATE1 = _mm_add_epi32(STATE1, CDGH_SAVE);

    TMP = _mm_shuffle_epi32(STATE0, 0x1B);       // FEBA
    STATE1 = _mm_shuffle_epi32(STATE1, 0xB1);    // DCHG
    STATE0 = _mm_blend_epi16(TMP, STATE1, 0xF0); // DCBA
    STATE1 = _mm_alignr_epi8(STATE1, TMP, 8);    // ABEF
    _mm_storeu_si128(reinterpret_cast<__m128i *>(&state_[0]), STATE0);
    _mm_storeu_si128(reinterpret_cast<__m128i *>(&state_[4]), STATE1);
  }
#endif

  std::array<uint32_t, 8> state_;
  std::array<uint8_t, kBlockSize> buf_{};
  uint32_t buf_len_ = 0;
  uint64_t total_ = 0;
  bool finalized_ = false;
};
