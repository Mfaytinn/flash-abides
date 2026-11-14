// Minimal deterministic Parquet writer for the two Track-3 deliverables.
//
// Layout: every column OPTIONAL (pyarrow's default), DataPage V1, codec SNAPPY.
// Integers use DELTA_BINARY_PACKED; strings use RLE_DICTIONARY over a fixed
// vocabulary. Each page body is framed as one Snappy literal.
//
// Column writers take values row by row and keep a page body in separate
// buffers, so a finished chunk is a list of byte segments that are hashed and
// written with writev and never concatenated. A file is a sequence of row
// groups; every (row group x column) chunk is encoded independently, so chunks
// can be built on any thread, and the file is laid out in schema order, so the
// bytes never depend on scheduling.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <sys/uio.h>
#include <vector>

#if defined(__BMI2__)
#include <immintrin.h>
#endif

class Sha256;

namespace t3::pq {

enum class Phys : int32_t { Int32 = 1, Int64 = 2, ByteArray = 6 };

struct ColumnSpec {
  const char *name;
  Phys type;
  bool is_string; // BYTE_ARRAY annotated UTF8 / STRING
};

// Growable malloc-backed byte buffer.
class Bytes {
public:
  Bytes() = default;
  Bytes(const Bytes &) = delete;
  Bytes &operator=(const Bytes &) = delete;
  Bytes(Bytes &&o) noexcept : p_(o.p_), n_(o.n_), cap_(o.cap_) {
    o.p_ = nullptr, o.n_ = o.cap_ = 0;
  }
  Bytes &operator=(Bytes &&o) noexcept {
    if (this != &o) {
      std::free(p_);
      p_ = o.p_, n_ = o.n_, cap_ = o.cap_;
      o.p_ = nullptr, o.n_ = o.cap_ = 0;
    }
    return *this;
  }
  ~Bytes() { std::free(p_); }

  void reserve(size_t extra) {
    if (n_ + extra > cap_) [[unlikely]]
      grow(n_ + extra);
  }
  uint8_t *tail() { return p_ + n_; } // valid for bytes reserved beyond size()
  void advance(size_t k) { n_ += k; }
  void put(const void *d, size_t k) {
    reserve(k);
    std::memcpy(p_ + n_, d, k);
    n_ += k;
  }
  void byte(uint8_t b) {
    reserve(1);
    p_[n_++] = b;
  }
  void varint(uint64_t v) {
    reserve(10);
    while (v >= 0x80) {
      p_[n_++] = static_cast<uint8_t>(v) | 0x80;
      v >>= 7;
    }
    p_[n_++] = static_cast<uint8_t>(v);
  }
  // clang-format off: keep the closing brace on this line for historical GCC PGO.
  void zigzag(int64_t v) { varint((static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63)); }
  // clang-format on
  void u32(uint32_t v) { put(&v, 4); }
  uint8_t *data() { return p_; }
  const uint8_t *data() const { return p_; }
  size_t size() const { return n_; }

private:
  void grow(size_t need) {
    size_t c = cap_ ? cap_ * 2 : 256;
    while (c < need)
      c *= 2;
    void *q = std::realloc(p_, c);
    if (!q)
      throw std::bad_alloc();
    p_ = static_cast<uint8_t *>(q);
    cap_ = c;
  }
  uint8_t *p_ = nullptr;
  size_t n_ = 0, cap_ = 0;
};

struct Seg {
  const uint8_t *p;
  size_t n;
};

// A complete dictionary page (header + framed PLAIN body) for a fixed
// vocabulary, shared by every chunk of the columns that use it.
struct DictPage {
  Bytes bytes;
  int64_t uncompressed = 0; // header + uncompressed body
  unsigned width = 1;       // bit width of the codes
};
DictPage make_dict_page(const std::vector<std::string_view> &vocab);

// One encoded column chunk: an optional dictionary page, then one data page
// whose header and Snappy framing are in `head` and whose body is body[0] +
// body[1] + body[2].
struct Chunk {
  const DictPage *dict = nullptr;
  Bytes head;
  Bytes body[3];
  int32_t data_encoding = 0;
  int64_t num_values = 0;
  int64_t uncompressed_total = 0; // page headers + uncompressed bodies
  int64_t compressed_total = 0;   // bytes in the file
};

// A required-valued INT64 column first, first + 1, ... (wrapping like
// DeltaColumn's unsigned delta arithmetic). Emits identical bytes without
// visiting every row.
Chunk sequence_column(size_t rows, int64_t first);

// DELTA_BINARY_PACKED column (physical INT32 or INT64). A nullable column takes
// push() or push_null() for every row and writes bit-packed definition levels;
// a required-valued one takes push() for every row and writes a single RLE run.
//
// extend() and extend_nullable() take a run of rows at once and write the bytes
// the same pushes would.
class DeltaColumn {
public:
  DeltaColumn(size_t rows, bool nullable);
  void push(int64_t v) {
    if (nullable_)
      def_.data()[bits_at_ + (row_ >> 3)] |=
          static_cast<uint8_t>(1u << (row_ & 7));
    ++row_;
    if (nvals_++ == 0) [[unlikely]] {
      first_ = prev_ = v;
      return;
    }
    pend_[npend_++] = static_cast<int64_t>(static_cast<uint64_t>(v) -
                                           static_cast<uint64_t>(prev_));
    prev_ = v;
    if (npend_ == kBlock)
      flush_block();
  }
  void push_null() { ++row_; }

  // The next n rows, each with a value: get(i) is the value of the i-th of
  // them.
  template <class Get> void extend(size_t n, Get get) {
    if (n == 0)
      return;
    if (nullable_) {
      uint8_t *const bits = def_.data() + bits_at_;
      for (size_t r = row_; r < row_ + n; ++r)
        bits[r >> 3] |= static_cast<uint8_t>(1u << (r & 7));
    }
    row_ += n;
    size_t i = 0;
    int64_t prev = prev_;
    if (nvals_ == 0) {
      first_ = prev = get(size_t{0});
      i = 1;
    }
    nvals_ += n;
    size_t np = npend_;
    while (i < n) {
      const size_t take = n - i < kBlock - np ? n - i : kBlock - np;
      int64_t *const out = pend_ + np;
      for (size_t k = 0; k < take; ++k) {
        const int64_t v = get(i + k);
        out[k] = static_cast<int64_t>(static_cast<uint64_t>(v) -
                                      static_cast<uint64_t>(prev));
        prev = v;
      }
      i += take;
      np += take;
      if (np == kBlock) {
        npend_ = np;
        flush_block();
        np = 0;
      }
    }
    npend_ = np;
    prev_ = prev;
  }

  // The next n rows of a nullable column: get(i, v) says whether the i-th of
  // them has a value and, if so, puts it in v. No branch depends on which rows
  // are null (a wakeup's send time, a message without an order: close to
  // random).
  template <class Get> void extend_nullable(size_t n, Get get) {
    uint8_t *const bits = def_.data() + bits_at_;
    size_t row = row_, np = npend_, i = 0;
    int64_t prev = prev_;
    if (nvals_ ==
        0) { // up to the column's first value, which no delta precedes
      for (; i < n; ++i, ++row) {
        int64_t v = 0;
        if (!get(i, v))
          continue;
        bits[row >> 3] |= static_cast<uint8_t>(1u << (row & 7));
        first_ = prev = v;
        nvals_ = 1;
        ++i, ++row;
        break;
      }
    }
    size_t nv = 0;
    auto append_delta = [&](int64_t v, bool has) {
      // Written for a null row too, then overwritten by the next value (np
      // stays).
      pend_[np] = static_cast<int64_t>(static_cast<uint64_t>(v) -
                                       static_cast<uint64_t>(prev));
      prev = has ? v : prev;
      np += has;
      nv += has;
      if (np == kBlock) {
        npend_ = np;
        flush_block();
        np = 0;
      }
    };
    // Finish an already-started definition byte before writing whole bytes.
    for (; i < n && (row & 7); ++i, ++row) {
      int64_t v = 0;
      const bool has = get(i, v);
      bits[row >> 3] |=
          static_cast<uint8_t>(static_cast<unsigned>(has) << (row & 7));
      append_delta(v, has);
    }
    for (; n - i >= 8; i += 8, row += 8) {
      uint8_t present = 0;
#pragma GCC unroll 8
      for (unsigned lane = 0; lane < 8; ++lane) {
        int64_t v = 0;
        const bool has = get(i + lane, v);
        present |= static_cast<uint8_t>(static_cast<unsigned>(has) << lane);
        append_delta(v, has);
      }
      // These eight rows are new, so one store replaces eight dependent
      // read-modify-writes of the same definition byte.
      bits[row >> 3] = present;
    }
    for (; i < n; ++i, ++row) {
      int64_t v = 0;
      const bool has = get(i, v);
      bits[row >> 3] |=
          static_cast<uint8_t>(static_cast<unsigned>(has) << (row & 7));
      append_delta(v, has);
    }
    row_ = row;
    npend_ = np;
    nvals_ += nv;
    prev_ = prev;
  }
  Chunk finish();

private:
  static constexpr size_t kBlock = 128; // values per block, in four miniblocks
  void flush_block();
  size_t rows_;
  bool nullable_;
  size_t row_ = 0, nvals_ = 0, bits_at_ = 0, npend_ = 0;
  int64_t first_ = 0, prev_ = 0;
  Bytes def_, blocks_;
  int64_t pend_[kBlock];
};

// RLE_DICTIONARY column over a shared dictionary page; one code per row.
// extend() takes a run of rows as DeltaColumn's does; extend_bytes() takes
// codes that sit in consecutive bytes, eight to a step.
class DictColumn {
public:
  DictColumn(size_t rows, const DictPage &dict);
  void push(uint8_t code) {
    acc_ |= static_cast<uint64_t>(code) << (k_ * width_);
    if (++k_ == 8) {
      std::memcpy(codes_.tail(), &acc_,
                  8); // keeps `width` bytes; 8 were reserved
      codes_.advance(width_);
      acc_ = 0;
      k_ = 0;
    }
  }
  template <class Get> void extend(size_t n, Get get) {
    uint64_t acc = acc_;
    unsigned k = k_;
    const unsigned w = width_;
    uint8_t *dst = codes_.tail();
    size_t i = 0;
    // Complete a group left by push() or an earlier extend() before packing
    // whole groups. get() is still called once per row, in row order.
    for (; i < n && k; ++i) {
      acc |= static_cast<uint64_t>(static_cast<uint8_t>(get(i))) << (k * w);
      if (++k == 8) {
        std::memcpy(dst, &acc, 8);
        dst += w;
        acc = 0;
        k = 0;
      }
    }
#if defined(__BMI2__)
    if (w <= 8) {
      const uint64_t low_bits = 0x0101010101010101ull * ((1u << w) - 1u);
      for (; n - i >= 8; i += 8) {
        uint64_t eight = 0;
#pragma GCC unroll 8
        for (unsigned lane = 0; lane < 8; ++lane)
          eight |= static_cast<uint64_t>(static_cast<uint8_t>(get(i + lane)))
                   << (lane * 8);
        const uint64_t group = _pext_u64(eight, low_bits);
        std::memcpy(dst, &group, 8); // keeps `w` bytes; 8 were reserved
        dst += w;
      }
    }
#endif
    for (; i < n; ++i) {
      acc |= static_cast<uint64_t>(static_cast<uint8_t>(get(i))) << (k * w);
      if (++k == 8) {
        std::memcpy(dst, &acc, 8);
        dst += w;
        acc = 0;
        k = 0;
      }
    }
    codes_.advance(static_cast<size_t>(dst - codes_.tail()));
    acc_ = acc;
    k_ = k;
  }
  void extend_bytes(const uint8_t *codes, size_t n);
  Chunk finish();

private:
  size_t rows_;
  const DictPage &dict_;
  unsigned width_;
  unsigned k_ = 0;
  uint64_t acc_ = 0;
  Bytes codes_;
};

// Where a chunk sits in its file, plus what the footer records about it.
struct ChunkMeta {
  int32_t data_encoding = 0;
  int64_t num_values = 0;
  int64_t uncompressed_total = 0;
  int64_t compressed_total = 0;
  int64_t dict_offset = -1; // -1: no dictionary page
  int64_t data_offset = 0;
};

// Appends the FileMetaData footer, its length and the closing magic for
// metas[rg * ncols + col].
void write_footer(Bytes &out, const std::vector<ColumnSpec> &schema,
                  const std::vector<ChunkMeta> &metas,
                  const std::vector<int64_t> &row_group_rows,
                  std::string_view pandas_metadata,
                  std::string_view created_by);

// Writes a file row group by row group as the groups are produced, keeping only
// their metadata; finish() writes the footer. A group may leave one column out
// and place that chunk later (the footer records every chunk's offset, so
// chunks need not be contiguous or in schema order). With `hash`, keeps the
// SHA-256 of every byte written, in file order.
class StreamWriter {
public:
  StreamWriter() = default;
  StreamWriter(const StreamWriter &) = delete;
  StreamWriter &operator=(const StreamWriter &) = delete;
  ~StreamWriter();
  void open(const std::string &path, size_t ncols, bool hash = false);
  // Writes chunks[c] for every column c but `deferred` (-1: none). The chunks
  // may be freed after.
  void append(std::vector<Chunk> &chunks, int64_t rows, int deferred = -1);
  // Writes the chunks left out of column `deferred_col` (deferred[g] for group
  // g, if given), then the footer, in one write, and closes the file.
  void finish(const std::vector<ColumnSpec> &schema,
              std::string_view pandas_metadata, std::string_view created_by,
              size_t deferred_col = 0,
              const std::vector<Chunk> *deferred = nullptr);
  bool is_open() const { return fd_ >= 0; }
  size_t groups() const { return rows_.size(); }
  std::string sha256_hex();              // after finish(), with `hash`
  int64_t bytes() const { return off_; } // written so far
  // The bytes append() would write for these chunks, and a bound on the footer
  // for `groups` row groups: together they bound the file before it is written.
  static size_t bytes_of(const std::vector<Chunk> &chunks);
  size_t footer_bound(size_t groups) const {
    return 8192 + groups * ncols_ * 96;
  }
  // Closes the file and removes it (a stream that must not be delivered).
  void abandon();
  // Diagnostics, after finish(): ms to lay out the last chunks and build the
  // footer, to hash and write them, to close the file; then the bytes of that
  // last write and of the footer alone.
  const double *finish_parts() const { return finish_parts_; }

private:
  void write(std::vector<struct ::iovec> &iov);
  double finish_parts_[5] = {};
  std::string path_;
  int fd_ = -1;
  size_t ncols_ = 0;
  int64_t off_ = 0;
  std::vector<ChunkMeta> metas_;
  std::vector<int64_t> rows_;
  std::vector<Seg> segs_;
  std::vector<struct ::iovec> iov_;
  std::unique_ptr<Sha256> sha_;
};

// A laid-out file: chunks[rg * ncols + col], the footer, and every byte in
// order as segments.
struct File {
  std::vector<Chunk> chunks;
  Bytes footer;
  std::vector<Seg> segs;
  size_t size = 0;
};

// Computes offsets, builds the footer and the segment list; `f.chunks` must be
// complete.
void assemble(File &f, const std::vector<ColumnSpec> &schema,
              const std::vector<int64_t> &row_group_rows,
              std::string_view pandas_metadata, std::string_view created_by);

} // namespace t3::pq
