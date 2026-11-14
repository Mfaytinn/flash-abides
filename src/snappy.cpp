#include "t3/snappy.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

void put_varint(std::string &out, uint64_t v) {
  while (v >= 0x80) {
    out.push_back(static_cast<char>(static_cast<uint8_t>(v) | 0x80));
    v >>= 7;
  }
  out.push_back(static_cast<char>(static_cast<uint8_t>(v)));
}

constexpr size_t kWindowSize = 65536; // copy2 offset limit (uint16)
constexpr size_t kMinMatch = 4;
constexpr size_t kMaxMatch = 64; // copy2 payload limit
// 4K-entry match table (16 KB).
constexpr size_t kHashBits = 12;
constexpr size_t kHashTableSize = 1 << kHashBits;
// Skip-ahead cap: consecutive misses advance the probe sparsely (long
// literal runs converge to 1 probe per 64 bytes); any match resets to
// dense probing, so compressible regions encode as before.
constexpr size_t kMaxSkipAdvance = 64;
// Minimum accepted match length.
constexpr size_t kMinAcceptLen = 6;

inline uint32_t hash4(uint32_t u) {
  return (u * 0x1e35a7bdu) >> (32 - kHashBits);
}

} // namespace

namespace snappy {

void compress(const uint8_t *in, size_t n, std::string &out) {
  put_varint(out, n);
  if (n == 0)
    return;
  // Worst-case bound (snappy MaxCompressedLength): encode never regrows.
  out.reserve(out.size() + 32 + n + n / 6);

  auto emit_literal = [&](size_t from, size_t to) {
    // Empty literals arise between back-to-back matches; the length-1
    // encoding would underflow to a 6-byte no-op tag, so skip them.
    if (to == from)
      return;
    const size_t len = to - from; // len >= 1 by construction
    const uint64_t len1 = len - 1;
    if (len1 < 60) {
      out.push_back(static_cast<char>(len1 << 2));
    } else {
      uint8_t extra_bytes;
      if (len1 < (1ull << 8)) {
        extra_bytes = 1;
      } else if (len1 < (1ull << 16)) {
        extra_bytes = 2;
      } else if (len1 < (1ull << 24)) {
        extra_bytes = 3;
      } else {
        extra_bytes = 4;
      }
      out.push_back(static_cast<char>((60 + extra_bytes - 1) << 2));
      for (uint8_t k = 0; k < extra_bytes; ++k) {
        out.push_back(static_cast<char>(static_cast<uint8_t>(len1 >> (8 * k))));
      }
    }
    out.append(reinterpret_cast<const char *>(in) + from, len);
  };

  // Jobs on one lane run serially, so reusing thread-local storage is
  // race-free.
  thread_local std::vector<int32_t> tl_table;
  if (tl_table.size() != kHashTableSize) {
    tl_table.assign(kHashTableSize, -1);
  } else {
    std::fill(tl_table.begin(), tl_table.end(), -1);
  }
  int32_t *table = tl_table.data();
  size_t lit_start = 0;
  size_t i = 0;
  size_t skip = 0; // consecutive hash misses since the last emitted match

  auto emit_copy = [&](size_t offset, size_t len) {
    // COPY_2 tag: offset in [1, 65535] as uint16 LE, len in [1, 64].
    out.push_back(static_cast<char>(0x02 | (((len - 1) << 2) & 0xff)));
    out.push_back(static_cast<char>(offset & 0xff));
    out.push_back(static_cast<char>((offset >> 8) & 0xff));
  };

  while (i + kMinMatch <= n) {
    uint32_t cur;
    std::memcpy(&cur, in + i, sizeof(cur));
    const uint32_t h = hash4(cur);
    const int32_t cand = table[h];
    table[h] = static_cast<int32_t>(i);
    size_t match_len = 0;
    size_t match_dist = 0;
    if (cand >= 0) {
      const size_t dist = i - static_cast<size_t>(cand);
      uint32_t prev;
      std::memcpy(&prev, in + cand, sizeof(prev));
      if (dist < kWindowSize && prev == cur) {
        // Byte-identical match length via word-at-a-time extension:
        // the first differing byte index equals ctz(xor)/8 on
        // little-endian (low bytes first). Reads stay in bounds:
        // cand < i keeps the candidate side inside whenever the
        // probe side is.
        size_t len = kMinMatch;
        while (len + 8 <= kMaxMatch && i + len + 8 <= n) {
          uint64_t a, b;
          std::memcpy(&a, in + i + len, sizeof(a));
          std::memcpy(&b, in + cand + len, sizeof(b));
          const uint64_t diff = a ^ b;
          if (diff == 0) {
            len += 8;
          } else {
            len += static_cast<size_t>(__builtin_ctzll(diff)) / 8;
            break;
          }
        }
        while (len < kMaxMatch && i + len < n &&
               in[cand + len] == in[i + len]) {
          ++len;
        }
        // Reject short matches without resetting skip-ahead to dense probing.
        if (len >= kMinAcceptLen) {
          match_len = len;
          match_dist = dist;
        }
      }
    }
    if (match_len > 0) {
      emit_literal(lit_start, i);
      emit_copy(match_dist, match_len);
      i += match_len;
      lit_start = i;
      skip = 0; // back to dense probing inside compressible runs
      continue;
    }
    // Hash miss (or rejected short match): advance sparsely in long
    // literal runs (1,1,... then 2,2,... up to 64 per probe). Skipped
    // bytes are still emitted as literals — they just never become
    // match candidates.
    ++skip;
    size_t adv = 1 + (skip >> 5);
    if (adv > kMaxSkipAdvance)
      adv = kMaxSkipAdvance;
    i += adv;
  }
  emit_literal(lit_start, n);
}

void literal(const uint8_t *in, size_t n, std::string &out) {
  put_varint(out, n);
  size_t off = 0;
  while (off < n) {
    const size_t len = std::min<size_t>(n - off, size_t{1} << 31);
    const uint64_t len1 = len - 1;
    if (len1 < 60) {
      out.push_back(static_cast<char>(len1 << 2));
    } else {
      unsigned extra = len1 < (1ull << 8)    ? 1
                       : len1 < (1ull << 16) ? 2
                       : len1 < (1ull << 24) ? 3
                                             : 4;
      out.push_back(static_cast<char>((59 + extra) << 2));
      for (unsigned k = 0; k < extra; ++k)
        out.push_back(static_cast<char>(static_cast<uint8_t>(len1 >> (8 * k))));
    }
    out.append(reinterpret_cast<const char *>(in) + off, len);
    off += len;
  }
}

} // namespace snappy
