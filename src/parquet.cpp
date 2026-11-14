#include "t3/parquet.hpp"

#include "t3/sha256.hpp"

#if defined(__BMI2__) || defined(__AVX2__)
#include <immintrin.h>
#endif
#if defined(__BMI2__)
#include <cpuid.h>
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/uio.h>
#include <unistd.h>
#include <utility>

namespace t3::pq {
namespace {

constexpr int32_t kEncPlain = 0;
constexpr int32_t kEncRle = 3;
constexpr int32_t kEncDelta = 5;
constexpr int32_t kEncRleDict = 8;
constexpr int32_t kCodecSnappy = 1;
constexpr int32_t kRepRequired = 0;
constexpr int32_t kRepOptional = 1;
constexpr int32_t kConvertedUtf8 = 0;
constexpr int32_t kPageData = 0;
constexpr int32_t kPageDict = 2;

constexpr uint8_t kMagic[4] = {'P', 'A', 'R', '1'};

// --- Thrift compact protocol
// -----------------------------------------------------------------
class Thrift {
public:
  explicit Thrift(Bytes &out) : buf(out) {}
  Bytes &buf;

  void struct_begin() {
    stack_.push_back(last_);
    last_ = 0;
  }
  void struct_end() {
    buf.byte(0);
    last_ = stack_.back();
    stack_.pop_back();
  }
  void field(int id, int type) {
    const int delta = id - last_;
    if (delta > 0 && delta <= 15) {
      buf.byte(static_cast<uint8_t>((delta << 4) | type));
    } else {
      buf.byte(static_cast<uint8_t>(type));
      zigzag32(static_cast<uint32_t>(id));
    }
    last_ = id;
  }
  void i32(int id, int32_t v) {
    field(id, 5);
    zigzag32(static_cast<uint32_t>(v));
  }
  void i64(int id, int64_t v) {
    field(id, 6);
    buf.zigzag(v);
  }
  void str(int id, std::string_view v) {
    field(id, 8);
    buf.varint(v.size());
    buf.put(v.data(), v.size());
  }
  void i32_list(int id, std::initializer_list<int32_t> vs) {
    field(id, 9);
    list_begin(vs.size(), 5);
    for (int32_t v : vs)
      zigzag32(static_cast<uint32_t>(v));
  }
  void str_list1(int id, std::string_view s) {
    field(id, 9);
    list_begin(1, 8);
    buf.varint(s.size());
    buf.put(s.data(), s.size());
  }
  void struct_list(int id, size_t size) {
    field(id, 9);
    list_begin(size, 12);
  }
  void struct_field(int id) { field(id, 12); }

private:
  void zigzag32(uint32_t v) {
    buf.varint((v << 1) ^
               (static_cast<uint32_t>(static_cast<int32_t>(v) >> 31)));
  }
  void list_begin(size_t size, int type) {
    if (size < 15) {
      buf.byte(static_cast<uint8_t>((size << 4) | type));
    } else {
      buf.byte(static_cast<uint8_t>(0xF0 | type));
      buf.varint(size);
    }
  }
  int last_ = 0;
  std::vector<int> stack_;
};

// Page header + the Snappy framing of a `body_len`-byte body sent as a single
// literal. Returns the header size (the framing is counted as compressed body).
size_t page_head(Bytes &out, int32_t page_type, size_t body_len,
                 int64_t num_values, int32_t encoding) {
  Bytes frame;
  frame.varint(body_len); // Snappy preamble: uncompressed length
  const uint64_t len1 =
      body_len - 1; // body_len >= 1: every page body has content
  if (len1 < 60) {
    frame.byte(static_cast<uint8_t>(len1 << 2));
  } else {
    const unsigned extra = len1 < (1ull << 8)    ? 1
                           : len1 < (1ull << 16) ? 2
                           : len1 < (1ull << 24) ? 3
                                                 : 4;
    frame.byte(static_cast<uint8_t>((59 + extra) << 2));
    for (unsigned k = 0; k < extra; ++k)
      frame.byte(static_cast<uint8_t>(len1 >> (8 * k)));
  }
  const size_t comp = frame.size() + body_len;
  Thrift h(out);
  h.struct_begin();
  h.i32(1, page_type);
  h.i32(2, static_cast<int32_t>(body_len));
  h.i32(3, static_cast<int32_t>(comp));
  if (page_type == kPageData) {
    h.struct_field(5);
    h.struct_begin();
    h.i32(1, static_cast<int32_t>(num_values));
    h.i32(2, encoding);
    h.i32(3, kEncRle);
    h.i32(4, kEncRle);
    h.struct_end();
  } else {
    h.struct_field(7);
    h.struct_begin();
    h.i32(1, static_cast<int32_t>(num_values));
    h.i32(2, kEncPlain);
    h.struct_end();
  }
  h.struct_end();
  const size_t header = out.size();
  out.put(frame.data(), frame.size());
  return header;
}

void finish_data_page(Chunk &c, int32_t encoding, int64_t rows) {
  const size_t body = c.body[0].size() + c.body[1].size() + c.body[2].size();
  const size_t header = page_head(c.head, kPageData, body, rows, encoding);
  c.data_encoding = encoding;
  c.num_values = rows;
  c.uncompressed_total += static_cast<int64_t>(header + body);
  c.compressed_total += static_cast<int64_t>(c.head.size() + body);
  if (c.dict) {
    c.uncompressed_total += c.dict->uncompressed;
    c.compressed_total += static_cast<int64_t>(c.dict->bytes.size());
  }
}

// Packs 32 values of W bits each (every value < 2^W), LSB first: 4 * W logical
// bytes. Narrow PEXT packers can overwrite up to seven scratch bytes after that
// payload; flush_block reserves this tail, and subsequent miniblocks overwrite
// any overlap.
template <unsigned W, bool Pext = false>
[[gnu::always_inline]] inline void pack32(const uint64_t *r, uint8_t *dst) {
  if constexpr (W == 0) {
    (void)r, (void)dst;
#if defined(__BMI2__)
  } else if constexpr (Pext && W < 8) {
    uint8_t bytes[32];
    pack32<8>(r, bytes);
    constexpr uint64_t mask = 0x0101010101010101ull * ((uint64_t{1} << W) - 1);
    for (unsigned group = 0; group < 4; ++group) {
      uint64_t word;
      std::memcpy(&word, bytes + 8 * group, 8);
      const uint64_t packed = _pext_u64(word, mask);
      // flush_block reserves eight extra bytes. Overlapping full-word
      // stores avoid a partial-copy chain; only 4 * W bytes are advanced.
      std::memcpy(dst + W * group, &packed, 8);
    }
#endif
  } else if constexpr (W == 8) {
#if defined(__AVX2__)
    const __m256i pick = _mm256_setr_epi8(
        0, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 8, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    for (unsigned i = 0; i < 32; i += 16) {
      const __m256i a = _mm256_shuffle_epi8(
          _mm256_loadu_si256(reinterpret_cast<const __m256i *>(r + i)), pick);
      const __m256i b = _mm256_shuffle_epi8(
          _mm256_loadu_si256(reinterpret_cast<const __m256i *>(r + i + 4)),
          pick);
      const __m256i c = _mm256_shuffle_epi8(
          _mm256_loadu_si256(reinterpret_cast<const __m256i *>(r + i + 8)),
          pick);
      const __m256i d = _mm256_shuffle_epi8(
          _mm256_loadu_si256(reinterpret_cast<const __m256i *>(r + i + 12)),
          pick);
      const __m256i ab = _mm256_unpacklo_epi16(a, b),
                    cd = _mm256_unpacklo_epi16(c, d);
      const __m256i bytes =
          _mm256_permute4x64_epi64(_mm256_unpacklo_epi32(ab, cd), 0xd8);
      const __m128i order =
          _mm_setr_epi8(0, 1, 8, 9, 2, 3, 10, 11, 4, 5, 12, 13, 6, 7, 14, 15);
      _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + i),
                       _mm_shuffle_epi8(_mm256_castsi256_si128(bytes), order));
    }
#else
    for (unsigned i = 0; i < 32; ++i)
      dst[i] = static_cast<uint8_t>(r[i]);
#endif
  } else if constexpr (W == 16) {
    uint16_t words[32];
    for (unsigned i = 0; i < 32; ++i)
      words[i] = static_cast<uint16_t>(r[i]);
    std::memcpy(dst, words, sizeof words);
  } else if constexpr (W == 32) {
    uint32_t words[32];
    for (unsigned i = 0; i < 32; ++i)
      words[i] = static_cast<uint32_t>(r[i]);
    std::memcpy(dst, words, sizeof words);
  } else if constexpr (W == 64) {
    std::memcpy(dst, r, 32 * sizeof *r);
  } else {
    uint64_t acc = 0;
    unsigned bits = 0;
#pragma GCC unroll 32
    for (unsigned i = 0; i < 32; ++i) {
      const uint64_t v = r[i];
      acc |= v << bits;
      if (bits + W >= 64) {
        std::memcpy(dst, &acc, 8);
        dst += 8;
        acc = bits ? v >> (64 - bits) : 0;
        bits = bits + W - 64;
      } else {
        bits += W;
      }
    }
    if (bits) { // 32 * W bits end on a 32-bit boundary
      const uint32_t lo = static_cast<uint32_t>(acc);
      std::memcpy(dst, &lo, 4);
    }
  }
}

using PackFn = void (*)(const uint64_t *, uint8_t *);
template <bool Pext, size_t... I>
constexpr std::array<PackFn, sizeof...(I)>
pack_table(std::index_sequence<I...>) {
  return {&pack32 < static_cast<unsigned>(I), Pext && (I > 0 && I < 8) > ...};
}

bool fast_pext() {
#if defined(__BMI2__)
  if (std::getenv("T3_NO_PEXT_PACK"))
    return false; // developer comparison / fallback tests
  unsigned a, b, c, d;
  if (!__get_cpuid(0, &a, &b, &c, &d))
    return false;
  char vendor[13] = {};
  std::memcpy(vendor, &b, 4);
  std::memcpy(vendor + 4, &d, 4);
  std::memcpy(vendor + 8, &c, 4);
  if (std::strcmp(vendor, "GenuineIntel") == 0)
    return true;
  if (std::strcmp(vendor, "AuthenticAMD") != 0 ||
      !__get_cpuid(1, &a, &b, &c, &d))
    return false;
  unsigned family = (a >> 8) & 15;
  if (family == 15)
    family += (a >> 20) & 255;
  // Earlier AMD implementations microcode PEXT; keep the shift/OR packers
  // there.
  return family >= 0x19;
#else
  return false;
#endif
}

// Resolve once at process start, not in the per-block/per-miniblock hot path.
const auto kPack = fast_pext()
                       ? pack_table<true>(std::make_index_sequence<65>{})
                       : pack_table<false>(std::make_index_sequence<65>{});

} // namespace

// --- DeltaColumn
// --------------------------------------------------------------------------------

DeltaColumn::DeltaColumn(size_t rows, bool nullable)
    : rows_(rows), nullable_(nullable) {
  if (nullable) {
    // Definition levels, bit width 1, as one bit-packed run of ceil(rows / 8)
    // groups (a reader stops after `rows` levels, so the padding bits are
    // ignored).
    const size_t groups = (rows + 7) / 8;
    Bytes hdr;
    hdr.varint((static_cast<uint64_t>(groups) << 1) | 1);
    def_.u32(static_cast<uint32_t>(hdr.size() + groups));
    def_.put(hdr.data(), hdr.size());
    bits_at_ = def_.size();
    def_.reserve(groups);
    std::memset(def_.tail(), 0, groups);
    def_.advance(groups);
  } else {
    // Every value present: one RLE run of `rows` ones.
    Bytes run;
    run.varint(static_cast<uint64_t>(rows) << 1);
    run.byte(1);
    def_.u32(static_cast<uint32_t>(run.size()));
    def_.put(run.data(), run.size());
  }
  blocks_.reserve(rows * 2 + 64);
}

void DeltaColumn::flush_block() {
  const size_t used = npend_;
  npend_ = 0;
  int64_t mn = INT64_MAX;
  // Independent vector accumulators hide compare/blend latency. Starting at
  // index zero also leaves no scalar tail for the common full 128-delta block.
#if defined(__clang__)
#pragma clang loop vectorize_width(4) interleave_count(4)
#endif
  for (size_t i = 0; i < used; ++i)
    mn = pend_[i] < mn ? pend_[i] : mn;
  for (size_t i = used; i < 128; ++i)
    pend_[i] = mn; // padding: residual 0
  uint64_t res[128];
  uint8_t widths[4];
  for (size_t mb = 0; mb < 4; ++mb) {
    uint64_t mask = 0;
    for (size_t i = mb * 32; i < mb * 32 + 32; ++i) {
      res[i] = static_cast<uint64_t>(pend_[i]) - static_cast<uint64_t>(mn);
      mask |= res[i];
    }
    widths[mb] = static_cast<uint8_t>(std::bit_width(mask));
  }
  blocks_.zigzag(mn);
  blocks_.put(widths, 4);
  // Miniblocks past the last value are not written (their width bytes are 0
  // anyway).
  const size_t live = (used + 31) / 32;
  size_t need = 0;
  for (size_t mb = 0; mb < live; ++mb)
    need += 4u * widths[mb];
  blocks_.reserve(
      need + 8); // scratch tail for narrow packers' overlapping word stores
  uint8_t *dst = blocks_.tail();
  for (size_t mb = 0; mb < live; ++mb) {
    kPack[widths[mb]](res + mb * 32, dst);
    dst += 4u * widths[mb];
  }
  blocks_.advance(need);
}

Chunk DeltaColumn::finish() {
  if (row_ != rows_)
    throw std::logic_error("parquet: row count mismatch");
  if (npend_)
    flush_block();
  Chunk c;
  Bytes hdr;
  hdr.varint(128); // block size
  hdr.varint(4);   // miniblocks per block
  hdr.varint(nvals_);
  hdr.zigzag(nvals_ ? first_ : 0);
  c.body[0] = std::move(def_);
  c.body[1] = std::move(hdr);
  c.body[2] = std::move(blocks_);
  finish_data_page(c, kEncDelta, static_cast<int64_t>(rows_));
  return c;
}

Chunk sequence_column(size_t rows, int64_t first) {
  Chunk c;
  Bytes run;
  run.varint(static_cast<uint64_t>(rows) << 1);
  run.byte(1);
  c.body[0].u32(static_cast<uint32_t>(run.size()));
  c.body[0].put(run.data(), run.size());
  c.body[1].varint(128);
  c.body[1].varint(4);
  c.body[1].varint(rows);
  c.body[1].zigzag(rows ? first : 0);

  // Each 128-delta block has minimum 1 and four zero-width miniblocks. Padding
  // in a partial final block also equals that minimum, so it has the same
  // bytes.
  constexpr uint8_t block[] = {2, 0, 0, 0, 0};
  const size_t blocks = rows > 1 ? (rows - 2) / 128 + 1 : 0;
  c.body[2].reserve(blocks * sizeof block);
  for (size_t i = 0; i < blocks; ++i)
    c.body[2].put(block, sizeof block);
  finish_data_page(c, kEncDelta, static_cast<int64_t>(rows));
  return c;
}

// --- DictColumn
// ---------------------------------------------------------------------------------

DictPage make_dict_page(const std::vector<std::string_view> &vocab) {
  DictPage d;
  Bytes body;
  for (auto w : vocab) {
    body.u32(static_cast<uint32_t>(w.size()));
    body.put(w.data(), w.size());
  }
  const size_t header =
      page_head(d.bytes, kPageDict, body.size(),
                static_cast<int64_t>(vocab.size()), kEncPlain);
  d.bytes.put(body.data(), body.size());
  d.uncompressed = static_cast<int64_t>(header + body.size());
  d.width =
      std::max(1u, static_cast<unsigned>(std::bit_width(vocab.size() - 1)));
  return d;
}

DictColumn::DictColumn(size_t rows, const DictPage &dict)
    : rows_(rows), dict_(dict), width_(dict.width) {
  const size_t groups = (rows + 7) / 8;
  codes_.reserve(32 + groups * width_ + 8);
  Bytes run; // definition levels: every value present
  run.varint(static_cast<uint64_t>(rows) << 1);
  run.byte(1);
  codes_.u32(static_cast<uint32_t>(run.size()));
  codes_.put(run.data(), run.size());
  codes_.byte(static_cast<uint8_t>(width_));
  if (rows)
    codes_.varint((static_cast<uint64_t>(groups) << 1) |
                  1); // one bit-packed run
  codes_.reserve(groups * width_ + 8);
}

// Eight codes are eight bytes; their low `width` bits, packed in order, are the
// group push() builds one shift at a time (pext, BMI2). Codes are below
// 2^width, as push() requires.
void DictColumn::extend_bytes(const uint8_t *codes, size_t n) {
  size_t i = 0;
  while (i < n && k_)
    push(codes[i++]); // the group under way
#if defined(__BMI2__)
  const unsigned w = width_;
  const uint64_t low_bits = 0x0101010101010101ull * ((1u << w) - 1u);
  uint8_t *dst = codes_.tail();
  for (; i + 8 <= n; i += 8) {
    uint64_t eight;
    std::memcpy(&eight, codes + i, 8);
    const uint64_t group = _pext_u64(eight, low_bits);
    std::memcpy(dst, &group, 8); // keeps `width` bytes; 8 were reserved
    dst += w;
  }
  codes_.advance(static_cast<size_t>(dst - codes_.tail()));
#endif
  for (; i < n; ++i)
    push(codes[i]);
}

Chunk DictColumn::finish() {
  if (k_) { // partial last group, zero padded
    std::memcpy(codes_.tail(), &acc_, 8);
    codes_.advance(width_);
    acc_ = 0;
    k_ = 0;
  }
  Chunk c;
  c.dict = &dict_;
  c.body[0] = std::move(codes_);
  finish_data_page(c, kEncRleDict, static_cast<int64_t>(rows_));
  return c;
}

// --- file assembly
// --------------------------------------------------------------------------------

namespace {

// Appends a chunk's bytes as segments starting at file offset `off` (advanced
// past them) and returns its metadata.
ChunkMeta place_chunk(const Chunk &c, int64_t &off, std::vector<Seg> &segs) {
  ChunkMeta m;
  auto add = [&](const uint8_t *p, size_t n) {
    if (n)
      segs.push_back({p, n});
    off += static_cast<int64_t>(n);
  };
  m.dict_offset = -1;
  if (c.dict) {
    m.dict_offset = off;
    add(c.dict->bytes.data(), c.dict->bytes.size());
  }
  m.data_offset = off;
  add(c.head.data(), c.head.size());
  for (const Bytes &b : c.body)
    add(b.data(), b.size());
  m.data_encoding = c.data_encoding;
  m.num_values = c.num_values;
  m.uncompressed_total = c.uncompressed_total;
  m.compressed_total = c.compressed_total;
  return m;
}

} // namespace

void write_footer(Bytes &out, const std::vector<ColumnSpec> &schema,
                  const std::vector<ChunkMeta> &metas,
                  const std::vector<int64_t> &row_group_rows,
                  std::string_view pandas_metadata,
                  std::string_view created_by) {
  const size_t ncols = schema.size();
  if (metas.size() != ncols * row_group_rows.size())
    throw std::logic_error("parquet: chunk count");
  int64_t num_rows = 0;
  for (int64_t r : row_group_rows)
    num_rows += r;

  const size_t start = out.size();
  Thrift th(out);
  th.struct_begin();
  th.i32(1, 1);
  th.struct_list(2, ncols + 1);
  th.struct_begin();
  th.i32(3, kRepRequired);
  th.str(4, "schema");
  th.i32(5, static_cast<int32_t>(ncols));
  th.struct_end();
  for (const ColumnSpec &col : schema) {
    th.struct_begin();
    th.i32(1, static_cast<int32_t>(col.type));
    th.i32(3, kRepOptional);
    th.str(4, col.name);
    if (col.is_string) {
      th.i32(6, kConvertedUtf8);
      th.struct_field(10); // LogicalType union
      th.struct_begin();
      th.struct_field(1); // STRING
      th.struct_begin();
      th.struct_end();
      th.struct_end();
    }
    th.struct_end();
  }
  th.i64(3, num_rows);
  th.struct_list(4, row_group_rows.size());
  for (size_t rg = 0; rg < row_group_rows.size(); ++rg) {
    th.struct_begin();
    th.struct_list(1, ncols);
    int64_t rg_uncompressed = 0, rg_compressed = 0;
    for (size_t col = 0; col < ncols; ++col) {
      const ChunkMeta &m = metas[rg * ncols + col];
      rg_uncompressed += m.uncompressed_total;
      rg_compressed += m.compressed_total;
      th.struct_begin();
      th.i64(2, m.dict_offset >= 0 ? m.dict_offset : m.data_offset);
      th.struct_field(3);
      th.struct_begin();
      th.i32(1, static_cast<int32_t>(schema[col].type));
      if (m.dict_offset >= 0)
        th.i32_list(2, {kEncPlain, kEncRle, kEncRleDict});
      else
        th.i32_list(2, {kEncRle, m.data_encoding});
      th.str_list1(3, schema[col].name);
      th.i32(4, kCodecSnappy);
      th.i64(5, m.num_values);
      th.i64(6, m.uncompressed_total);
      th.i64(7, m.compressed_total);
      th.i64(9, m.data_offset);
      if (m.dict_offset >= 0)
        th.i64(11, m.dict_offset);
      th.struct_end();
      th.struct_end();
    }
    th.i64(2, rg_uncompressed);
    th.i64(3, row_group_rows[rg]);
    th.i64(6, rg_compressed);
    th.struct_end();
  }
  th.struct_list(5, 1);
  th.struct_begin();
  th.str(1, "pandas");
  th.str(2, pandas_metadata);
  th.struct_end();
  th.str(6, created_by);
  th.struct_end();
  out.u32(static_cast<uint32_t>(out.size() - start));
  out.put(kMagic, 4);
}

void assemble(File &f, const std::vector<ColumnSpec> &schema,
              const std::vector<int64_t> &row_group_rows,
              std::string_view pandas_metadata, std::string_view created_by) {
  if (f.chunks.size() != schema.size() * row_group_rows.size())
    throw std::logic_error("parquet: chunk count");
  std::vector<ChunkMeta> metas;
  metas.reserve(f.chunks.size());
  f.segs.clear();
  f.segs.reserve(f.chunks.size() * 5 + 4);
  f.segs.push_back({kMagic, 4});
  int64_t off = 4;
  for (const Chunk &c : f.chunks)
    metas.push_back(place_chunk(c, off, f.segs));
  write_footer(f.footer, schema, metas, row_group_rows, pandas_metadata,
               created_by);
  f.segs.push_back({f.footer.data(), f.footer.size()});
  f.size = static_cast<size_t>(off) + f.footer.size();
}

// --- StreamWriter
// ------------------------------------------------------------------------------

StreamWriter::~StreamWriter() {
  if (fd_ >= 0)
    ::close(fd_);
}

void StreamWriter::write(std::vector<iovec> &iov) {
  if (sha_)
    for (const iovec &v : iov)
      sha_->update(static_cast<const uint8_t *>(v.iov_base), v.iov_len);
  size_t i = 0;
  while (i < iov.size()) {
    const int cnt = static_cast<int>(std::min<size_t>(iov.size() - i, 1024));
    const ssize_t w = ::writev(fd_, iov.data() + i, cnt);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      throw std::runtime_error(std::string("parquet write failed: ") +
                               std::strerror(errno));
    }
    size_t left = static_cast<size_t>(w);
    while (i < iov.size() && left >= iov[i].iov_len)
      left -= iov[i++].iov_len;
    if (left) {
      iov[i].iov_base = static_cast<char *>(iov[i].iov_base) + left;
      iov[i].iov_len -= left;
    }
  }
}

void StreamWriter::open(const std::string &path, size_t ncols, bool hash) {
  fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd_ < 0)
    throw std::runtime_error("cannot create " + path + ": " +
                             std::strerror(errno));
  path_ = path;
  ncols_ = ncols;
  if (hash)
    sha_ = std::make_unique<Sha256>();
  std::vector<iovec> iov{{const_cast<uint8_t *>(kMagic), 4}};
  write(iov);
  off_ = 4;
}

void StreamWriter::append(std::vector<Chunk> &chunks, int64_t rows,
                          int deferred) {
  if (chunks.size() != ncols_)
    throw std::logic_error("parquet: stream chunk count");
  segs_.clear();
  for (size_t c = 0; c < ncols_; ++c)
    metas_.push_back(static_cast<int>(c) == deferred
                         ? ChunkMeta{}
                         : place_chunk(chunks[c], off_, segs_));
  iov_.clear();
  for (const Seg &sg : segs_)
    iov_.push_back({const_cast<uint8_t *>(sg.p), sg.n});
  write(iov_);
  rows_.push_back(rows);
}

void StreamWriter::finish(const std::vector<ColumnSpec> &schema,
                          std::string_view pandas_metadata,
                          std::string_view created_by, size_t deferred_col,
                          const std::vector<Chunk> *deferred) {
  using clock = std::chrono::steady_clock;
  auto ms = [](auto a, auto b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };
  const auto t0 = clock::now();
  segs_.clear();
  if (deferred)
    for (size_t g = 0; g < deferred->size(); ++g)
      metas_.at(g * ncols_ + deferred_col) =
          place_chunk((*deferred)[g], off_, segs_);
  Bytes footer;
  write_footer(footer, schema, metas_, rows_, pandas_metadata, created_by);
  iov_.clear();
  for (const Seg &sg : segs_)
    iov_.push_back({const_cast<uint8_t *>(sg.p), sg.n});
  iov_.push_back({footer.data(), footer.size()});
  const auto t1 = clock::now();
  size_t bytes = 0;
  for (const iovec &v : iov_)
    bytes += v.iov_len;
  write(iov_);
  const auto t2 = clock::now();
  if (::close(fd_) != 0)
    throw std::runtime_error("close failed for a parquet stream");
  fd_ = -1;
  finish_parts_[0] = ms(t0, t1), finish_parts_[1] = ms(t1, t2),
  finish_parts_[2] = ms(t2, clock::now());
  finish_parts_[3] = static_cast<double>(bytes),
  finish_parts_[4] = static_cast<double>(footer.size());
}

size_t StreamWriter::bytes_of(const std::vector<Chunk> &chunks) {
  size_t n = 0;
  for (const Chunk &c : chunks) { // as place_chunk() lays them out
    if (c.dict)
      n += c.dict->bytes.size();
    n += c.head.size();
    for (const Bytes &b : c.body)
      n += b.size();
  }
  return n;
}

void StreamWriter::abandon() {
  if (fd_ >= 0)
    ::close(fd_);
  fd_ = -1;
  if (!path_.empty())
    ::unlink(path_.c_str());
}

std::string StreamWriter::sha256_hex() {
  if (!sha_)
    throw std::logic_error("parquet: stream not hashed");
  return Sha256::hex(sha_->digest());
}

} // namespace t3::pq
