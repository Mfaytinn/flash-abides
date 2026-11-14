// Minimal parallel-for over independent tasks, plus CPU budget detection.
// Results never depend on which thread ran which task (callers write each
// task's output into its own slot).
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <functional>
#include <linux/futex.h>
#include <mutex>
#include <sched.h>
#include <stdexcept>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(__x86_64__)
#define T3_POOL_PAUSE() __builtin_ia32_pause()
#else
#define T3_POOL_PAUSE() ((void)0)
#endif

namespace t3 {

// Faults `bytes` of this thread's malloc heap in and hands it back to malloc
// (not to the kernel, given a high M_TRIM_THRESHOLD), so later allocations on
// this thread take no page faults. The barrier matters: the compiler otherwise
// deletes the whole malloc+memset+free sequence.
inline void prewarm_heap(size_t bytes) {
  if (!bytes)
    return;
  void *p = std::malloc(bytes);
  if (!p)
    return;
  // Not zero: GCC turns malloc + memset(0) into calloc, and calloc skips
  // writing fresh pages.
  std::memset(p, 0x5a, bytes);
  asm volatile("" : : "r"(p) : "memory");
  std::free(p);
}

// CPU time the calling thread has used so far, in ms (diagnostics: how busy a
// helper thread was).
inline double thread_cpu_ms() {
  struct timespec ts{};
  ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return static_cast<double>(ts.tv_sec) * 1e3 +
         static_cast<double>(ts.tv_nsec) * 1e-6;
}

// Direct futex sleep/wake on a 32-bit atomic (std::atomic::wait spins through
// sched_yield calls first, and every syscall is expensive under gVisor).
// Spurious returns are fine: callers loop.
inline void futex_wait(std::atomic<uint32_t> &word, uint32_t expected) {
  ::syscall(SYS_futex, reinterpret_cast<uint32_t *>(&word), FUTEX_WAIT_PRIVATE,
            expected, nullptr, nullptr, 0);
}
inline void futex_wait_for(std::atomic<uint32_t> &word, uint32_t expected,
                           const struct timespec &rel) {
  ::syscall(SYS_futex, reinterpret_cast<uint32_t *>(&word), FUTEX_WAIT_PRIVATE,
            expected, &rel, nullptr, 0);
}
inline void futex_wake_all(std::atomic<uint32_t> &word) {
  ::syscall(SYS_futex, reinterpret_cast<uint32_t *>(&word), FUTEX_WAKE_PRIVATE,
            INT32_MAX, nullptr, nullptr, 0);
}

// Usable CPUs: min(affinity mask, cgroup v2 cpu.max quota, 4 = the card's
// cpus), overridable with T3_THREADS.
inline unsigned cpu_budget() {
  if (const char *env = std::getenv("T3_THREADS")) {
    const int v = std::atoi(env);
    if (v >= 1)
      return static_cast<unsigned>(std::min(v, 64));
  }
  unsigned n = 4;
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof set, &set) == 0)
    n = std::min<unsigned>(n, static_cast<unsigned>(CPU_COUNT(&set)));
  if (FILE *f = std::fopen("/sys/fs/cgroup/cpu.max", "r")) {
    char quota[32] = {0};
    long period = 0;
    if (std::fscanf(f, "%31s %ld", quota, &period) == 2 && period > 0 &&
        quota[0] != 'm') {
      const long q = std::atol(quota);
      const unsigned c =
          static_cast<unsigned>(std::max(1L, (q + period - 1) / period));
      n = std::min(n, c);
    }
    std::fclose(f);
  }
  return std::max(1u, n);
}

// Runs fn(i) for i in [0, n) on up to `threads` threads (the caller is one of
// them). Tasks are claimed in index order; put the most expensive tasks first.
// Exceptions propagate.
template <class Fn> void parallel_for(size_t n, unsigned threads, Fn &&fn) {
  if (n == 0)
    return;
  threads = static_cast<unsigned>(std::min<size_t>(threads, n));
  if (threads <= 1) {
    for (size_t i = 0; i < n; ++i)
      fn(i);
    return;
  }
  std::atomic<size_t> next{0};
  std::exception_ptr err;
  std::mutex err_mu;
  auto worker = [&] {
    for (;;) {
      const size_t i = next.fetch_add(1, std::memory_order_relaxed);
      if (i >= n)
        return;
      try {
        fn(i);
      } catch (...) {
        std::lock_guard<std::mutex> g(err_mu);
        if (!err)
          err = std::current_exception();
        next.store(n);
        return;
      }
    }
  };
  std::vector<std::thread> pool;
  pool.reserve(threads - 1);
  for (unsigned t = 1; t < threads; ++t)
    pool.emplace_back(worker);
  worker();
  for (auto &th : pool)
    th.join();
  if (err)
    std::rethrow_exception(err);
}

// A pool whose workers are started ahead of a timed section and spin (they do
// not sleep) until run() hands them work, so the section pays neither thread
// creation nor an idle CPU's wake-up. run() is called from one thread at a
// time; the caller works too. Exceptions propagate.
class SpinPool {
public:
  // Returns once every worker is running (thread start is slow under gVisor; a
  // worker that is still starting when run() is first called would stall that
  // run).
  explicit SpinPool(unsigned threads) {
    for (unsigned t = 1; t < threads; ++t)
      workers_.emplace_back([this] { loop(); });
    while (started_.load(std::memory_order_acquire) != workers_.size())
      pause();
  }
  ~SpinPool() {
    stop_.store(true, std::memory_order_release);
    bump();
    for (auto &w : workers_)
      w.join();
  }
  SpinPool(const SpinPool &) = delete;
  SpinPool &operator=(const SpinPool &) = delete;

  unsigned size() const { return static_cast<unsigned>(workers_.size()) + 1; }

  // While hot, idle workers keep spinning instead of going to sleep, so a timed
  // section never pays a futex wake (a syscall plus a reschedule, slow and
  // erratic under gVisor) to start or resume them. For pools no larger than the
  // CPU budget, whose spinning takes no CPU from work.
  void set_hot(bool on) {
    require_active();
    hot_.store(on, std::memory_order_seq_cst);
    if (on && sleepers_.load(std::memory_order_seq_cst) != 0)
      futex_wake_all(epoch_); // they spin on
  }

  // Runs fn(i) exactly once on every thread of the pool (i = 0 for the
  // caller, 1.. for workers), e.g. to set up per-thread state such as each
  // thread's malloc arena.
  template <class Fn> void run_on_all(Fn &&fn) {
    require_active();
    std::function<void(size_t)> f = std::forward<Fn>(fn);
    fn_ = &f;
    per_thread_ = true;
    err_ = nullptr;
    done_.store(0, std::memory_order_relaxed);
    bump();
    f(0);
    while (done_.load(std::memory_order_acquire) != workers_.size())
      pause();
    per_thread_ = false;
    fn_ = nullptr;
    if (err_)
      std::rethrow_exception(err_);
  }

  template <class Fn> void run(size_t n, Fn &&fn) {
    require_active();
    if (n == 0)
      return;
    run_tasks(n, std::forward<Fn>(fn), false);
  }

  // The last task phase: each worker exits as soon as it runs out of work, so
  // it does not spin alongside a longer remaining task. Even an empty or
  // throwing phase retires the pool; later task submissions are rejected, and
  // the destructor joins the workers.
  template <class Fn> void run_final(size_t n, Fn &&fn) {
    require_active();
    run_tasks(n, std::forward<Fn>(fn), true);
  }

private:
  void require_active() const {
    if (retiring_)
      throw std::logic_error("SpinPool: task phase submitted after retirement");
  }

  template <class Fn> void run_tasks(size_t n, Fn &&fn, bool retire) {
    std::function<void(size_t)> f = std::forward<Fn>(fn);
    fn_ = &f;
    n_ = n;
    retiring_ = retire;
    err_ = nullptr;
    next_.store(0, std::memory_order_relaxed);
    done_.store(0, std::memory_order_relaxed);
    bump(); // publishes fn_, n_
    work();
    while (done_.load(std::memory_order_acquire) != workers_.size())
      pause();
    fn_ = nullptr;
    if (err_)
      std::rethrow_exception(err_);
  }

  static void pause() {
#if defined(__x86_64__)
    __builtin_ia32_pause();
#endif
  }
  void work() {
    for (;;) {
      const size_t i = next_.fetch_add(1, std::memory_order_relaxed);
      if (i >= n_)
        return;
      try {
        (*fn_)(i);
      } catch (...) {
        std::lock_guard<std::mutex> g(err_mu_);
        if (!err_)
          err_ = std::current_exception();
        next_.store(n_, std::memory_order_relaxed);
        return;
      }
    }
  }
  // Spins for a while (a few tens of microseconds) waiting for the next run(),
  // then sleeps, so an idle worker does not compete for a CPU with a slow
  // task's own threads for long. Advances the epoch; the futex wake is issued
  // only when a worker is actually asleep. (A worker registers in sleepers_
  // before re-checking the epoch, and the kernel re-checks it again before
  // sleeping, so no wake-up can be lost.)
  void bump() {
    epoch_.fetch_add(1, std::memory_order_seq_cst);
    if (sleepers_.load(std::memory_order_seq_cst) != 0)
      futex_wake_all(epoch_);
  }
  void loop() {
    const size_t index = started_.fetch_add(1, std::memory_order_acq_rel) + 1;
    uint32_t seen = 0;
    for (;;) {
      uint32_t e;
      for (unsigned spins = 0;
           (e = epoch_.load(std::memory_order_acquire)) == seen; ++spins) {
        if (spins < 4096 || hot_.load(std::memory_order_relaxed)) {
          pause();
          continue;
        }
        sleepers_.fetch_add(1, std::memory_order_seq_cst);
        if (epoch_.load(std::memory_order_seq_cst) == seen)
          futex_wait(epoch_, seen);
        sleepers_.fetch_sub(1, std::memory_order_seq_cst);
      }
      if (stop_.load(std::memory_order_acquire))
        return;
      seen = e;
      // Capture before publishing done_: after that publication the caller may
      // start another phase and change retiring_, while this worker finishes
      // its old phase.
      const bool retire = retiring_;
      if (per_thread_) {
        try {
          (*fn_)(index);
        } catch (...) {
          std::lock_guard<std::mutex> g(err_mu_);
          if (!err_)
            err_ = std::current_exception();
        }
      } else {
        work();
      }
      done_.fetch_add(1, std::memory_order_release);
      if (retire)
        return;
    }
  }

  std::vector<std::thread> workers_;
  std::function<void(size_t)> *fn_ = nullptr;
  bool per_thread_ = false;
  bool retiring_ = false;
  size_t n_ = 0;
  std::exception_ptr err_;
  std::mutex err_mu_;
  alignas(64) std::atomic<uint32_t> epoch_{0};
  std::atomic<uint32_t> sleepers_{0};
  std::atomic<size_t> started_{0};
  alignas(64) std::atomic<size_t> next_{0};
  alignas(64) std::atomic<size_t> done_{0};
  std::atomic<bool> stop_{false};
  std::atomic<bool> hot_{false};
};

} // namespace t3
