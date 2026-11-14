// One mapping for the fixed-size buffers a single run touches during its
// window: the Streamer's rings, the in-flight message log and the helper
// threads' rings. Fresh anonymous pages are zero, which the buffers rely on.
//
// Draw threads populate their own rings. The Streamer populates the message
// log and output rings after the window opens; earlier population would hold
// the address-space lock while the main thread still maps memory.
//
// Without populate-on-request support, mmap populates the whole mapping before
// helper threads start. T3_ARENA_EAGER forces that path.
//
// init(bytes, true) aligns the mapping to 2 MiB and advises MADV_HUGEPAGE.
// init() touches no buffer pages. T3_NO_ARENA_HUGE disables huge pages.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <sys/mman.h>

#ifndef MADV_POPULATE_WRITE
#define MADV_POPULATE_WRITE 23
#endif

namespace t3 {

class Arena {
public:
  static constexpr size_t kAlign = 64;
  // A gap after every carve-out, so buffers whose sizes are page multiples (the
  // two Streamer rings, walked in step) start at different offsets modulo 4 KiB
  // and do not alias in L1.
  static constexpr size_t kStagger = 64 * 5;

  Arena() = default;
  Arena(const Arena &) = delete;
  Arena &operator=(const Arena &) = delete;
  ~Arena() {
    if (base_)
      ::munmap(base_, bytes_);
  }

  // Bytes to reserve for a carve-out of n elements of T (alignment and gap
  // included).
  template <class T> static size_t need(size_t n) {
    return round(n * sizeof(T)) + kStagger + kAlign;
  }

  // Maps `bytes` (a sum of need() values). No-op for 0. Afterwards either every
  // page is populated, or deferred() is true and the helper threads populate
  // what they use. With `huge`, and from one huge page of bytes on, the mapping
  // starts on a 2 MiB boundary and asks the kernel for huge pages (see the top
  // of this file); huge() tells whether it did.
  void init(size_t bytes, bool huge = false) {
    if (!bytes)
      return;
    bytes_ = (bytes + 4095) & ~size_t{4095};
    static const bool eager = std::getenv("T3_ARENA_EAGER") != nullptr;
    if (!eager && populates_on_request()) {
      huge = huge && bytes_ >= kHugePage;
      base_ = huge ? map_aligned(bytes_) : map(bytes_, 0);
      deferred_ = true;
      huge_ = huge;
      return;
    }
    base_ = map(bytes_, MAP_POPULATE);
  }
  bool active() const { return base_ != nullptr; }
  // The pages are not populated yet (see the top of this file).
  bool deferred() const { return deferred_; }
  // The carve-outs taken so far span [base(), base() + used()).
  char *base() const { return base_; }
  size_t used() const { return used_; }

  // Faults in the pages of [p, p + n) for writing, without reading or changing
  // their contents: safe while another thread uses them. A failure is ignored
  // (the pages then fault in on use).
  static void populate(void *p, size_t n) {
    if (!n)
      return;
    const uintptr_t lo = reinterpret_cast<uintptr_t>(p) & ~uintptr_t{4095};
    const uintptr_t hi =
        (reinterpret_cast<uintptr_t>(p) + n + 4095) & ~uintptr_t{4095};
    (void)::madvise(reinterpret_cast<void *>(lo), hi - lo, MADV_POPULATE_WRITE);
  }

  // n zeroed elements of T, 64-byte aligned. Throws if the reservation is
  // exhausted.
  template <class T> T *take(size_t n) {
    const size_t start = round(used_);
    const size_t end = start + n * sizeof(T);
    if (!base_ || end > bytes_)
      throw std::bad_alloc();
    used_ = end + kStagger;
    return reinterpret_cast<T *>(base_ + start);
  }

  // The mapping starts on a huge page boundary and asks for huge pages.
  bool huge() const { return huge_; }

  // Kilobytes of this process backed by huge pages just now (AnonHugePages of
  // smaps_rollup; -1 where the file is missing). A diagnostic: the kernel walks
  // every mapping to answer.
  static long anon_huge_kb() {
    std::FILE *f = std::fopen("/proc/self/smaps_rollup", "r");
    if (!f)
      return -1;
    char line[256];
    long kb = -1;
    while (std::fgets(line, sizeof line, f))
      if (std::strncmp(line, "AnonHugePages:", 14) == 0)
        kb = std::atol(line + 14);
    std::fclose(f);
    return kb;
  }

private:
  static constexpr size_t kHugePage = size_t{2} << 20;
  static size_t round(size_t b) { return (b + kAlign - 1) & ~(kAlign - 1); }
  // Whether this kernel populates on request. Asked of a page of its own: one
  // of the arena's would be populated by the calling thread, and with huge
  // pages that is a whole 2 MiB.
  static bool populates_on_request() {
    char *p = map(4096, 0);
    const bool yes = ::madvise(p, 4096, MADV_POPULATE_WRITE) == 0;
    ::munmap(p, 4096);
    return yes;
  }
  static char *map(size_t bytes, int flags) {
    void *p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | flags, -1, 0);
    if (p == MAP_FAILED)
      throw std::bad_alloc();
    return static_cast<char *>(p);
  }
  // `bytes` at an address divisible by 2 MiB: a mapping one huge page longer,
  // trimmed at both ends.
  static char *map_aligned(size_t bytes) {
    char *lo = map(bytes + kHugePage, 0);
    char *a = reinterpret_cast<char *>(
        (reinterpret_cast<uintptr_t>(lo) + kHugePage - 1) & ~(kHugePage - 1));
    if (a > lo)
      ::munmap(lo, static_cast<size_t>(a - lo));
    char *const end = lo + bytes + kHugePage;
    if (end > a + bytes)
      ::munmap(a + bytes, static_cast<size_t>(end - (a + bytes)));
    (void)::madvise(
        a, bytes,
        MADV_HUGEPAGE); // a kernel without huge pages says no: plain pages then
    return a;
  }
  char *base_ = nullptr;
  size_t bytes_ = 0, used_ = 0;
  bool deferred_ = false;
  bool huge_ = false;
};

} // namespace t3
