// Contiguous append-only array backed by an anonymous mapping. Growth uses
// mremap (page tables move, data is not copied), transparent huge pages are
// requested, and a prefix can be pre-faulted during initialization so the timed
// simulation window does not take page faults.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <sys/mman.h>
#include <type_traits>
#include <utility>

namespace t3 {

// Process-wide switch: when set, BigArrays live on the malloc heap instead of
// their own mappings. Batch runs set it: each worker's heap is faulted in once
// before the batch timer and reused by every sub it runs, so the timed loop
// issues no mmap/munmap and takes (almost) no page faults, the two things
// gVisor makes expensive.
inline bool &bigarray_heap_mode() {
  static bool on = false;
  return on;
}

template <class T> class BigArray {
  static_assert(std::is_trivially_copyable_v<T>);

public:
  BigArray() = default;
  BigArray(const BigArray &) = delete;
  BigArray &operator=(const BigArray &) = delete;
  BigArray(BigArray &&o) noexcept { swap(o); }
  BigArray &operator=(BigArray &&o) noexcept {
    if (this != &o) {
      release();
      swap(o);
    }
    return *this;
  }
  ~BigArray() { release(); }

  // Reserves capacity for n elements; pre-faults the first `prefault` of them.
  // Pages are populated in one call (MAP_POPULATE / MADV_POPULATE_WRITE).
  void init(size_t n, size_t prefault = 0) {
    release();
    cap_ = n < 256 ? 256 : n;
    if (bigarray_heap_mode()) {
      heap_ = true;
      bytes_ = cap_ * sizeof(T);
      data_ = static_cast<T *>(std::malloc(bytes_));
      if (!data_)
        throw std::bad_alloc();
      set_limit();
      if (prefault)
        std::memset(static_cast<void *>(data_), 0,
                    std::min(prefault, cap_) * sizeof(T));
      return;
    }
    bytes_ = round_up(cap_ * sizeof(T));
    const size_t pf = prefault < cap_ ? prefault : cap_;
    // A request for the whole capacity populates the whole (page-rounded)
    // mapping.
    const size_t pf_bytes =
        !pf       ? 0
        : pf >= n ? bytes_
                  : std::min(bytes_, (pf * sizeof(T) + 4095) & ~size_t{4095});
    // Small mappings that are pre-faulted whole are populated by mmap itself;
    // huge-page candidates must be advised first, so they are populated after
    // madvise.
    const bool populate_now = pf_bytes == bytes_ && bytes_ < kHugeThreshold;
    void *p = ::mmap(
        nullptr, bytes_, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | (populate_now ? MAP_POPULATE : 0), -1, 0);
    if (p == MAP_FAILED)
      throw std::bad_alloc();
    data_ = static_cast<T *>(p);
    cap_ = bytes_ / sizeof(T);
    set_limit();
    advise();
    if (pf_bytes && !populate_now)
      populate(pf_bytes);
  }

  size_t capacity() const { return cap_; }
  // Grows (as push() would) until at least n elements fit.
  void ensure(size_t n) {
    while (cap_ < n)
      grow();
    set_limit();
  }
  // For owners that write data()[i] directly: records how many elements are in
  // use.
  void set_size(size_t n) {
    n_ = n;
    set_limit();
  }

  T *data() { return data_; }
  const T *data() const { return data_; }
  size_t size() const { return n_; }
  bool empty() const { return n_ == 0; }
  T &operator[](size_t i) { return data_[i]; }
  const T &operator[](size_t i) const { return data_[i]; }
  T &back() { return data_[n_ - 1]; }

  // Appends one uninitialized element and returns it. One compare per push
  // covers both growth and the optional boundary callback (limit_ is the nearer
  // of the capacity and the next boundary).
  T &push() {
    if (n_ == limit_) [[unlikely]]
      at_limit();
    return data_[n_++];
  }
  // fn(ctx, n) runs whenever n (a positive multiple of `step`) elements are
  // complete, i.e. at the push of element n. Clear with step = 0.
  void set_boundary(size_t step, void (*fn)(void *, size_t), void *ctx) {
    step_ = fn ? step : 0;
    boundary_fn_ = fn;
    boundary_ctx_ = ctx;
    set_limit();
  }
  void push_back(const T &v) { push() = v; }
  // fn(ctx) runs once, before the mapping first moves (growth), so a concurrent
  // reader can stop.
  void set_before_move(void (*fn)(void *), void *ctx) {
    before_move_ = fn;
    before_move_ctx_ = ctx;
  }
  void resize_down(size_t n) { n_ = n; }

private:
  // Large arrays are rounded to 2 MiB and asked for transparent huge pages;
  // small ones use 4 KiB pages (zeroing a 2 MiB huge page for a few KiB of data
  // costs more than it saves).
  static constexpr size_t kHugeThreshold = 8u << 20;
  static size_t round_up(size_t b) {
    const size_t page = b >= kHugeThreshold ? (2u << 20) : 4096u;
    return (b + page - 1) & ~(page - 1);
  }
  void advise() {
#ifdef MADV_HUGEPAGE
    if (bytes_ >= kHugeThreshold)
      ::madvise(data_, bytes_, MADV_HUGEPAGE);
#endif
  }
  void populate(size_t len) {
#ifndef MADV_POPULATE_WRITE
#define MADV_POPULATE_WRITE 23 // Linux 5.14
#endif
    static const bool no_madv =
        std::getenv("T3_NO_MADV_POPULATE") != nullptr; // tests the fallback
    if (!no_madv && ::madvise(data_, len, MADV_POPULATE_WRITE) == 0)
      return;
    // Older kernels and gVisor lack MADV_POPULATE_WRITE. When the whole (still
    // untouched) mapping is to be populated, replace it with a populated one in
    // a single call instead of taking one fault per page. Only the whole
    // mapping: a replaced prefix would split it in two, and mremap cannot grow
    // a range that spans two mappings.
    if (len >= bytes_) {
      void *p =
          ::mmap(data_, bytes_, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_POPULATE, -1, 0);
      if (p == data_) {
        advise();
        return;
      }
      if (p == MAP_FAILED) { // MAP_FIXED failed and left the range unchanged:
                             // map afresh
        p = ::mmap(nullptr, bytes_, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED)
          throw std::bad_alloc();
        ::munmap(data_, bytes_);
        data_ = static_cast<T *>(p);
        advise();
      }
    }
    char *c = reinterpret_cast<char *>(data_); // last resort: touch each page
    for (size_t off = 0; off < len; off += 4096)
      c[off] = 0;
  }
  void set_limit() {
    limit_ = step_ ? std::min(cap_, (n_ / step_ + 1) * step_) : cap_;
  }
  void at_limit() {
    if (n_ == cap_)
      grow();
    if (step_ && n_ != 0 && n_ % step_ == 0)
      boundary_fn_(boundary_ctx_, n_);
    set_limit();
  }
  void grow() {
    if (before_move_) {
      auto fn = before_move_;
      before_move_ = nullptr;
      fn(before_move_ctx_);
    }
    if (heap_) {
      void *q = std::realloc(static_cast<void *>(data_), bytes_ * 2);
      if (!q)
        throw std::bad_alloc();
      data_ = static_cast<T *>(q);
      bytes_ *= 2;
      cap_ = bytes_ / sizeof(T);
      return;
    }
    const size_t nb = round_up(bytes_ * 2);
    void *p = ::mremap(data_, bytes_, nb, MREMAP_MAYMOVE);
    if (p == MAP_FAILED)
      throw std::bad_alloc();
    data_ = static_cast<T *>(p);
    bytes_ = nb;
    cap_ = bytes_ / sizeof(T);
    advise();
  }
  void release() {
    before_move_ = nullptr;
    boundary_fn_ = nullptr;
    if (data_) {
      if (heap_)
        std::free(static_cast<void *>(data_));
      else
        ::munmap(data_, bytes_);
    }
    heap_ = false;
    data_ = nullptr;
    n_ = cap_ = bytes_ = limit_ = step_ = 0;
  }
  void swap(BigArray &o) noexcept {
    std::swap(data_, o.data_);
    std::swap(n_, o.n_);
    std::swap(cap_, o.cap_);
    std::swap(bytes_, o.bytes_);
    std::swap(before_move_, o.before_move_);
    std::swap(before_move_ctx_, o.before_move_ctx_);
    std::swap(limit_, o.limit_);
    std::swap(heap_, o.heap_);
    std::swap(step_, o.step_);
    std::swap(boundary_fn_, o.boundary_fn_);
    std::swap(boundary_ctx_, o.boundary_ctx_);
  }

  // push() touches only the first three members.
  T *data_ = nullptr;
  size_t n_ = 0, limit_ = 0;
  size_t cap_ = 0, bytes_ = 0;
  void (*before_move_)(void *) = nullptr;
  void *before_move_ctx_ = nullptr;
  size_t step_ = 0;
  bool heap_ =
      false; // allocated with malloc (bigarray_heap_mode) rather than mmap
  void (*boundary_fn_)(void *, size_t) = nullptr;
  void *boundary_ctx_ = nullptr;
};

} // namespace t3
