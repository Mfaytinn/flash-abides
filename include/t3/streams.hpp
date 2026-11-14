// Where the simulation's rows go: the in-flight message log, the ledger
// (message_trace.parquet) and the trace (trace.parquet).
//
// Every delivery appends one LedgerRow holding everything its ledger row needs,
// so a message's log entry is dead once delivered: the log only holds messages
// in flight (MsgLog, a ring indexed by message id). Every trace event appends
// one TraceRec.
//
// Small runs (and batch subs) keep all rows ("whole" mode) and encode them
// after the window. Larger runs stream both files: rows go into rings, and a
// Streamer thread encodes finished row groups and appends them to the files
// during the window, so memory stays bounded and little output work is left
// afterwards. Trace groups end only where t changes (canonicalization sorts
// each equal-t run), and their msg_type chunks are written at the end of the
// file: which PARTIAL_FILL is an order's last execution (ORDER_FILLED) is only
// known once the run is over. The bytes differ between the two modes; the
// decoded tables do not.
#pragma once

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "t3/arena.hpp"
#include "t3/bigarray.hpp"
#include "t3/parquet.hpp"
#include "t3/sim.hpp"

namespace t3 {

// Encodes ledger rows[0, n) (sequence numbers seq0 + i) as the ten column
// chunks of one row group.
void encode_ledger_group(const LedgerRow *rows, size_t n, int64_t seq0,
                         pq::Chunk *out);

// Log entries of messages in flight, by message id. Ids only grow, so a ring
// indexed by id holds the recent ones; an entry still in flight when its slot
// comes round again (a very long latency) moves to a side table. Entries are
// taken (and freed) at delivery.
class MsgLog {
public:
  static size_t ring_size(size_t ring) {
    size_t r = 1;
    while (r < ring)
      r <<= 1;
    return r;
  }
  // Bytes of an arena carve-out for a log of ring_size(ring) entries.
  static size_t arena_bytes(size_t ring) {
    return Arena::need<LogEntry>(ring_size(ring)) +
           Arena::need<uint32_t>(ring_size(ring));
  }
  // From `arena` when given (populated, zero); otherwise from the heap, where
  // `lazy` leaves the entries untouched (their pages fault in on first use) and
  // the default zeroes them now, so the window takes no first-touch faults on
  // them.
  void init(size_t ring, bool lazy, Arena *arena = nullptr) {
    const size_t r = ring_size(ring);
    if (arena) {
      entries_ = arena->take<LogEntry>(r);
      owner_ = arena->take<uint32_t>(r); // zero = free (ids start at 1)
    } else {
      own_entries_.reset(lazy ? new LogEntry[r] : new LogEntry[r]());
      own_owner_.reset(new uint32_t[r]());
      entries_ = own_entries_.get();
      owner_ = own_owner_.get();
    }
    mask_ = static_cast<uint32_t>(r - 1);
    spill_.reserve(1024);
  }
  [[gnu::always_inline]] LogEntry &put(uint32_t id) {
    const uint32_t slot = id & mask_;
    if (owner_[slot]) [[unlikely]]
      spill(slot);
    owner_[slot] = id;
    return entries_[slot];
  }
  // Frees id's entry and returns it; the reference stays valid until the next
  // put() or take().
  [[gnu::always_inline]] const LogEntry &take(uint32_t id) {
    const uint32_t slot = id & mask_;
    if (owner_[slot] == id) [[likely]] {
      owner_[slot] = 0;
      return entries_[slot];
    }
    return take_spilled(id);
  }
  // Starts loading id's entry (the event loop knows its next event one event
  // ahead).
  void prefetch(uint32_t id) const {
    __builtin_prefetch(entries_ + (id & mask_));
  }
  size_t spills() const { return spills_; } // diagnostics
  size_t ring() const { return size_t{mask_} + 1; }

private:
  [[gnu::noinline]] void spill(uint32_t slot) {
    spill_.emplace(owner_[slot], entries_[slot]);
    ++spills_;
  }
  [[gnu::noinline]] const LogEntry &take_spilled(uint32_t id) {
    const auto it = spill_.find(id);
    if (it == spill_.end())
      throw std::logic_error("message log: delivered an unknown message");
    taken_ = it->second;
    spill_.erase(it);
    return taken_;
  }

  LogEntry *entries_ = nullptr;
  uint32_t *owner_ = nullptr;
  uint32_t mask_ = 0;
  std::unique_ptr<LogEntry[]> own_entries_; // heap mode only
  std::unique_ptr<uint32_t[]> own_owner_;
  std::unordered_map<uint32_t, LogEntry> spill_;
  LogEntry taken_{};
  size_t spills_ = 0;
};

class Streamer;

// Ledger rows in delivery order; next() returns the slot for the next row.
class LedgerStream {
public:
  LedgerStream() = default;
  LedgerStream(const LedgerStream &) = delete;
  LedgerStream &operator=(const LedgerStream &) = delete;

  // Whole mode: capacity for `cap` rows, the first `prefault` of them faulted
  // in now (it grows past `cap` if it must). Streaming: Streamer.
  void init_whole(size_t cap, size_t prefault);
  // A ledger that is not written (the unit's card does not require it): next()
  // hands out the same scratch row every time, so the kernel's stores stay in
  // one cache line, no group is ever handed to the Streamer, and size() still
  // counts the deliveries.
  void init_skipped() {
    base_ = &scratch_;
    mask_ = 0;
    limit_ = ~size_t{0};
    skipped_ = true;
  }
  bool skipped() const { return skipped_; }
  LedgerRow &next() {
    if (n_ == limit_) [[unlikely]]
      boundary();
    return base_[n_++ & mask_];
  }
  // A skipped ledger's delivery in the kernel that fills no row: counted,
  // nothing handed out.
  void count() { ++n_; }
  size_t size() const { return n_; }
  bool streaming() const { return streamer_ != nullptr; }
  BigArray<LedgerRow> &rows() { // whole mode; no rows when skipped
    whole_.set_size(skipped_ ? 0 : n_);
    return whole_;
  }

private:
  friend class Streamer;
  void boundary();
  LedgerRow *base_ = nullptr;
  size_t n_ = 0, limit_ = 0, mask_ = ~size_t{0};
  Streamer *streamer_ = nullptr;
  BigArray<LedgerRow> whole_;
  LedgerRow scratch_{};
  bool skipped_ = false;
};

// Trace rows in emission order; push(t) returns the slot for the next row (time
// t), at(i) reaches back to a row of the current t (quote coalescing rewrites
// the last quote row in place).
class TraceStream {
public:
  TraceStream() = default;
  TraceStream(const TraceStream &) = delete;
  TraceStream &operator=(const TraceStream &) = delete;

  void init_whole(size_t cap, size_t prefault);
  TraceRec &push(int64_t t) {
    if (n_ == limit_) [[unlikely]]
      boundary(t);
    return base_[n_++ & mask_];
  }
  TraceRec &at(size_t i) { return base_[i & mask_]; }
  size_t size() const { return n_; }
  bool streaming() const { return streamer_ != nullptr; }
  BigArray<TraceRec> &rows() { // whole mode
    whole_.set_size(n_);
    return whole_;
  }

private:
  friend class Streamer;
  void boundary(int64_t t);
  TraceRec *base_ = nullptr;
  size_t n_ = 0, limit_ = 0, mask_ = ~size_t{0};
  Streamer *streamer_ = nullptr;
  BigArray<TraceRec> whole_;
};

// Streams both files during the window, from one encoder thread or, with
// `split`, from two: the trace on one and the ledger on the other. Constructed
// before the window (it opens both files and starts the threads); go() at the
// start of the window, when the threads fault in the memory the first groups
// need, without the event loop waiting for them; finish() after it.
class Streamer {
public:
  // Rows per ledger group, and the minimum per trace group, by run size
  // (estimated deliveries). Each ring holds 4 groups. T3_STREAM_GROUP
  // overrides it.
  static size_t group_for(size_t est_deliveries);

  // `arena` (optional): where the two rings come from; see arena_bytes(). When
  // its pages are not populated yet (Arena::deferred), the thread populates,
  // from go() on, every carve-out taken from it so far: the message log (taken
  // before this constructor) and the two rings, which are what the event loop
  // writes first. `ledger_skip` / `ledger_cap`, for a unit whose ledger is
  // optional: the ledger file is not written at all (no ring, no rows kept:
  // LedgerStream::init_skipped), or is dropped (removed at finish()) once it
  // would exceed ledger_cap bytes. The decision depends only on the ledger's
  // own bytes, so every repeat makes the same one. `split`: the ledger is
  // encoded on a thread of its own (ignored when it is not written).
  Streamer(LedgerStream &ledger, TraceStream &trace,
           const std::string &ledger_path, const std::string &trace_path,
           size_t est_trace_rows, size_t est_orders, size_t group,
           Arena *arena = nullptr, bool ledger_skip = false,
           uint64_t ledger_cap = 0, bool split = false);
  Streamer(const Streamer &) = delete;
  Streamer &operator=(const Streamer &) = delete;
  ~Streamer();

  void go();
  // After the window: the thread completes the trace file (its remaining
  // groups, the msg_type chunks and the footer) while the calling thread
  // completes the ledger; then joins.
  void finish();
  const std::string &trace_sha256() const { return trace_sha256_; }
  // Bytes of the arena carve-outs for the two rings.
  static size_t
  arena_bytes(size_t group,
              bool with_ledger = true); // a skipped ledger has no ring
  // Diagnostics: trace groups, trace ring growths, and main-thread waits on the
  // thread.
  size_t trace_groups() const { return t_group_rows_.size(); }
  size_t ring_grows() const { return grows_; }
  size_t waits() const { return waits_; }
  // After finish(): "written", "skipped" or "dropped" (see the constructor).
  const char *ledger_fate() const {
    return ledger_skipped_   ? "skipped"
           : ledger_dropped_ ? "dropped"
                             : "written";
  }
  // After finish(), the thread's part of it (diagnostics): ms for the last
  // groups, the threads used, ms for the ORDER_FILLED marks and msg_type
  // chunks, ms for the final write.
  const double *finish_parts() const { return finish_ms_; }
  // After finish(): CPU ms each thread used up to the end of the window
  // (diagnostics); the ledger thread's is 0 without one.
  double window_cpu_ms() const { return window_cpu_ms_; }
  double ledger_cpu_ms() const { return ledger_cpu_ms_; }
  bool split() const { return split_; }

private:
  friend class LedgerStream;
  friend class TraceStream;
  static constexpr size_t kMaxCuts =
      1024; // trace groups published and not yet taken

  void ledger_boundary();
  void trace_boundary(int64_t t);
  void grow_trace_ring();
  void wake();
  void wake_ledger();
  void run();
  void run_ledger();   // the ledger's own thread
  void finish_trace(); // the thread's part of finish()
  void start_trace_thread(size_t est_trace_rows, size_t est_orders);
  void populate_rings();
  void warm_up(size_t est_trace_rows, size_t est_orders);
  void stop_thread();
  void encode_ledger(size_t g, size_t rows);
  void encode_trace(size_t lo, size_t hi); // rows [lo, hi) of the trace ring
  template <class Pred> void wait_until(Pred done);
  void mark_failed(
      std::exception_ptr e); // the thread: record the error, release every wait
  [[noreturn]] void
  fail(); // the main thread: join the thread, rethrow its error

  LedgerStream &L_;
  TraceStream &T_;
  size_t group_; // rows per group (group_for)

  // --- ledger: a ring of ledger_groups_ groups of group_ rows
  size_t ledger_groups_;
  LedgerRow *lring_ = nullptr;
  std::unique_ptr<LedgerRow[]> own_lring_; // without an arena
  pq::StreamWriter lw_;
  size_t l_appended_ =
      0; // groups done (thread, then the main thread after the join)
  uint64_t ledger_cap_ = 0;
  bool ledger_skipped_ = false; // never opened
  bool ledger_dropped_ =
      false; // over ledger_cap_: no more groups; removed at finish()

  // --- trace: a ring of rows (grown, rarely, only while the thread holds no
  // reference to it)
  size_t tring_rows_;
  TraceRec *tring_ = nullptr;
  std::unique_ptr<TraceRec[]> own_tring_; // without an arena, or after a growth
  pq::StreamWriter tw_;
  size_t t_open_ = 0;        // main thread: first row of the group being filled
  uint64_t t_cut_count_ = 0; // main thread: groups published
  uint64_t t_cuts_[kMaxCuts] = {};
  // thread (then the main thread after the join)
  uint64_t t_taken_groups_ = 0;
  size_t t_lo_ = 0; // first row of the next group to take
  std::vector<TraceRec> scratch_;
  std::vector<uint8_t>
      types_; // msg_type code of every trace row, in file order
  std::vector<uint32_t>
      last_pos_; // per order id: 1 + file row of its last execution (0: none)
  std::vector<int64_t> t_group_rows_;
  std::vector<pq::Chunk> chunks_;
  std::string trace_sha256_;
  char *populate_base_ =
      nullptr; // the arena's carve-outs to populate on the thread, if any
  size_t populate_bytes_ = 0;
  size_t grows_ = 0, waits_ = 0; // main thread
  double finish_ms_[4] = {};
  double window_cpu_ms_ = 0, ledger_cpu_ms_ = 0;
  bool split_ = false; // set before either thread starts

  // --- synchronization
  alignas(64) std::atomic<uint64_t> l_published_{0}; // complete ledger groups
  alignas(64) std::atomic<uint64_t> l_encoded_{0};
  alignas(64) std::atomic<uint64_t> t_published_{0}; // trace groups published
  alignas(64) std::atomic<uint64_t> t_consumed_{
      0}; // trace rows copied out of the ring
  alignas(64) std::atomic<uint64_t> t_taken_{
      0}; // trace groups copied out of the ring
  std::atomic<uint32_t> wake_{0}, sleeping_{0}, go_{0}, ready_{0};
  std::atomic<uint32_t> lwake_{0},
      lsleeping_{0};             // the ledger thread's, when split
  std::atomic<uint32_t> fin_{0}; // set by finish(): every row is final
  std::atomic<uint32_t> released_{
      0}; // set by the thread: it encodes no more ledger groups
  std::atomic<uint32_t> failed_{
      0}; // set by the thread: the main thread stops at its next boundary
  std::atomic<bool> stop_{false};
  std::atomic<bool> err_claimed_{
      false};              // the first thread to fail records its error
  std::exception_ptr err_; // a thread's failure; read after the joins
  std::thread thread_;     // the trace, and the ledger too unless split
  std::thread lthread_;    // the ledger, when split
};

} // namespace t3
