#include "t3/output.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <sys/resource.h>
#include <sys/uio.h>
#include <thread>
#include <unistd.h>
#include <utility>

#include "t3/parallel.hpp"
#include "t3/sha256.hpp"
#include "t3/streams.hpp"

namespace t3 {
namespace {

// pandas metadata exactly as pyarrow 15 wrote it into the reference files, so
// pandas decodes our files with the same dtypes (string, int32, nullable Int64)
// as the references.
constexpr std::string_view kTracePandas =
    R"({"index_columns": [], "column_indexes": [], "columns": [{"name": "t_ns", "field_name": "t_ns", "pandas_type": "int64", "numpy_type": "int64", "metadata": null}, {"name": "agent_id", "field_name": "agent_id", "pandas_type": "int32", "numpy_type": "int32", "metadata": null}, {"name": "msg_type", "field_name": "msg_type", "pandas_type": "unicode", "numpy_type": "string", "metadata": null}, {"name": "side", "field_name": "side", "pandas_type": "unicode", "numpy_type": "string", "metadata": null}, {"name": "price", "field_name": "price", "pandas_type": "int64", "numpy_type": "int64", "metadata": null}, {"name": "size", "field_name": "size", "pandas_type": "int64", "numpy_type": "int64", "metadata": null}, {"name": "order_id", "field_name": "order_id", "pandas_type": "int64", "numpy_type": "int64", "metadata": null}], "creator": {"library": "pyarrow", "version": "15.0.2"}, "pandas_version": "1.5.3"})";

constexpr std::string_view kLedgerPandas =
    R"({"index_columns": [], "column_indexes": [], "columns": [{"name": "seq", "field_name": "seq", "pandas_type": "int64", "numpy_type": "int64", "metadata": null}, {"name": "t_recv_ns", "field_name": "t_recv_ns", "pandas_type": "int64", "numpy_type": "int64", "metadata": null}, {"name": "t_send_ns", "field_name": "t_send_ns", "pandas_type": "int64", "numpy_type": "Int64", "metadata": null}, {"name": "latency_ns", "field_name": "latency_ns", "pandas_type": "int64", "numpy_type": "int64", "metadata": null}, {"name": "src_id", "field_name": "src_id", "pandas_type": "int32", "numpy_type": "int32", "metadata": null}, {"name": "dst_id", "field_name": "dst_id", "pandas_type": "int32", "numpy_type": "int32", "metadata": null}, {"name": "message_id", "field_name": "message_id", "pandas_type": "int64", "numpy_type": "int64", "metadata": null}, {"name": "msg_type", "field_name": "msg_type", "pandas_type": "unicode", "numpy_type": "string", "metadata": null}, {"name": "order_id", "field_name": "order_id", "pandas_type": "int64", "numpy_type": "Int64", "metadata": null}, {"name": "causal_parent", "field_name": "causal_parent", "pandas_type": "int64", "numpy_type": "Int64", "metadata": null}], "creator": {"library": "pyarrow", "version": "15.0.2"}, "pandas_version": "1.5.3"})";

constexpr std::string_view kCreatedBy = "track3-lean-engine 1.0";

// Whole-process timing includes speculative heap touching. Normally allocate
// only the working set encoding needs; narrow-window diagnostics can opt into
// the old warming policy.
void prewarm_output_heap(size_t bytes) {
  static const bool warm = std::getenv("T3_STREAMER_PREWARM") != nullptr;
  if (warm)
    prewarm_heap(bytes);
}

const std::vector<pq::ColumnSpec> kTraceSchema = {
    {"t_ns", pq::Phys::Int64, false},
    {"agent_id", pq::Phys::Int32, false},
    {"msg_type", pq::Phys::ByteArray, true},
    {"side", pq::Phys::ByteArray, true},
    {"price", pq::Phys::Int64, false},
    {"size", pq::Phys::Int64, false},
    {"order_id", pq::Phys::Int64, false}};
const std::vector<pq::ColumnSpec> kLedgerSchema = {
    {"seq", pq::Phys::Int64, false},
    {"t_recv_ns", pq::Phys::Int64, false},
    {"t_send_ns", pq::Phys::Int64, false},
    {"latency_ns", pq::Phys::Int64, false},
    {"src_id", pq::Phys::Int32, false},
    {"dst_id", pq::Phys::Int32, false},
    {"message_id", pq::Phys::Int64, false},
    {"msg_type", pq::Phys::ByteArray, true},
    {"order_id", pq::Phys::Int64, false},
    {"causal_parent", pq::Phys::Int64, false}};
constexpr size_t kTraceCols = 7, kLedgerCols = 10;

// Dictionary pages for the fixed vocabularies; the code order matches
// TraceType, the side byte and Msg.
struct Dicts {
  pq::DictPage trace_types = pq::make_dict_page(
      {"ORDER_SUBMITTED", "ORDER_ACCEPTED", "ORDER_FILLED", "PARTIAL_FILL",
       "ORDER_CANCELLED", "ORDER_REPLACED", "QUOTE_UPDATE"});
  pq::DictPage sides = pq::make_dict_page({"BID", "ASK"});
  pq::DictPage ledger_types = pq::make_dict_page(
      {"AGENT_WAKEUP", "MarketClosePriceRequestMsg", "MarketHoursRequestMsg",
       "MarketHoursMsg", "MarketClosePriceMsg", "MarketClosedMsg",
       "QuerySpreadMsg", "QuerySpreadResponseMsg", "LimitOrderMsg",
       "CancelOrderMsg", "OrderAcceptedMsg", "OrderExecutedMsg",
       "OrderCancelledMsg"});
};
const Dicts &dicts() {
  static const Dicts d;
  return d;
}

// Row groups of about this many rows. Trace groups end only where t changes, so
// the equal-t runs that canonicalization sorts never straddle two groups (and
// each group sorts on its own thread).
constexpr size_t kRowGroup = kLedgerRowGroup;

using Range = std::pair<size_t, size_t>;

std::vector<Range> trace_groups(const BigArray<TraceRec> &tr) {
  std::vector<Range> g;
  const size_t n = tr.size();
  for (size_t lo = 0; lo < n;) {
    size_t hi = std::min(n, lo + kRowGroup);
    while (hi < n && tr[hi].t == tr[hi - 1].t)
      ++hi;
    g.push_back({lo, hi});
    lo = hi;
  }
  if (g.empty())
    g.push_back({0, 0});
  return g;
}

std::vector<Range> ledger_groups(size_t n) {
  std::vector<Range> g;
  for (size_t lo = 0; lo < n; lo += kRowGroup)
    g.push_back({lo, std::min(n, lo + kRowGroup)});
  if (g.empty())
    g.push_back({0, 0});
  return g;
}

std::vector<int64_t> group_rows(const std::vector<Range> &g) {
  std::vector<int64_t> r;
  for (const Range &x : g)
    r.push_back(static_cast<int64_t>(x.second - x.first));
  return r;
}

// last[order_id] = 1 + index of that order's last execution row (0 = none),
// from every group.
void mark_last_fills(const BigArray<TraceRec> &tr, Range g, uint64_t *last) {
  for (size_t i = g.second; i-- > g.first;) {
    const TraceRec &r = tr[i];
    if (r.type != static_cast<uint8_t>(TraceType::PartialFill))
      continue;
    std::atomic_ref<uint64_t> a(last[r.order_id]);
    uint64_t cur = a.load(std::memory_order_relaxed);
    while (cur < i + 1 &&
           !a.compare_exchange_weak(cur, i + 1, std::memory_order_relaxed)) {
    }
  }
}

// Sorts each equal-t run of rows by order_id, stably: the canonical (t_ns,
// order_id) order.
void sort_runs(TraceRec *tr, size_t n) {
  for (size_t start = 0; start < n;) {
    size_t end = start + 1;
    while (end < n && tr[end].t == tr[start].t)
      ++end;
    if (end - start > 1) {
      if (end - start <= 32) {
        for (size_t i = start + 1; i < end; ++i) {
          const TraceRec v = tr[i];
          size_t j = i;
          while (j > start && tr[j - 1].order_id > v.order_id) {
            tr[j] = tr[j - 1];
            --j;
          }
          tr[j] = v;
        }
      } else {
        std::stable_sort(tr + start, tr + end,
                         [](const TraceRec &a, const TraceRec &b) {
                           return a.order_id < b.order_id;
                         });
      }
    }
    start = end;
  }
}

// The last execution of each order becomes ORDER_FILLED; then the stable
// order_id sort inside each equal-t run. Emission order is final order within
// one order id, so marking first is equivalent.
void canonicalize(TraceRec *tr, Range g, const uint64_t *last) {
  for (size_t i = g.first; i < g.second; ++i)
    if (tr[i].type == static_cast<uint8_t>(TraceType::PartialFill) &&
        last[tr[i].order_id] == i + 1)
      tr[i].type = static_cast<uint8_t>(TraceType::Filled);
  sort_runs(tr + g.first, g.second - g.first);
}

// The seven column chunks of a trace row group; without `types`, out[2]
// (msg_type) stays empty. Rows a column takes at a time (DeltaColumn::extend):
// each column of a group reads the same 20 KiB of rows again, from the
// first-level cache.
constexpr size_t kTile = 512;

void encode_trace_rows(const TraceRec *tr, size_t n, bool types,
                       pq::Chunk *out) {
  const Dicts &d = dicts();
  pq::DeltaColumn t(n, false), agent(n, false), price(n, false), size(n, false),
      oid(n, false);
  pq::DictColumn side(n, d.sides);
  if (types) {
    pq::DictColumn type(n, d.trace_types);
    type.extend(n, [tr](size_t i) { return tr[i].type; });
    out[2] = type.finish();
  }
  for (size_t at = 0; at < n; at += kTile) {
    const TraceRec *r = tr + at;
    const size_t k = std::min(kTile, n - at);
    t.extend(k, [r](size_t i) { return r[i].t; });
    agent.extend(k, [r](size_t i) { return static_cast<int64_t>(r[i].agent); });
    side.extend(k, [r](size_t i) { return r[i].side; });
    price.extend(k, [r](size_t i) { return r[i].price; });
    size.extend(k, [r](size_t i) { return r[i].size; });
    oid.extend(k, [r](size_t i) { return r[i].order_id; });
  }
  out[0] = t.finish();
  out[1] = agent.finish();
  out[3] = side.finish();
  out[4] = price.finish();
  out[5] = size.finish();
  out[6] = oid.finish();
}

pq::Chunk encode_trace_types(const uint8_t *codes, size_t n) {
  pq::DictColumn type(n, dicts().trace_types);
  type.extend_bytes(codes, n);
  return type.finish();
}

void encode_trace_group(TraceRec *tr, Range g, const uint64_t *last,
                        pq::Chunk *out) {
  canonicalize(tr, g, last);
  encode_trace_rows(tr + g.first, g.second - g.first, true, out);
}

// Faults in the top 64 KB of this thread's stack.
[[gnu::noinline]] void touch_stack() {
  volatile char buf[64 << 10];
  for (size_t i = 0; i < sizeof buf; i += 4096)
    buf[i] = 0;
}

} // namespace

void encode_ledger_group(const LedgerRow *rows, size_t n, int64_t seq0,
                         pq::Chunk *out) {
  const Dicts &d = dicts();
  pq::DeltaColumn t_recv(n, false), t_send(n, true), lat(n, false),
      src(n, false), dst(n, false), mid(n, false), oid(n, true),
      causal(n, true);
  pq::DictColumn type(n, d.ledger_types);
  for (size_t at = 0; at < n; at += kTile) {
    const LedgerRow *r = rows + at;
    const size_t k = std::min(kTile, n - at);
    t_recv.extend(k, [r](size_t i) { return r[i].t_recv; });
    t_send.extend_nullable(k, [r](size_t i, int64_t &v) { // null for a wakeup
      v = r[i].t_send;
      return r[i].t_send != kNull;
    });
    lat.extend(k, [r](size_t i) {
      return r[i].t_send == kNull ? int64_t{0} : r[i].t_recv - r[i].t_send;
    });
    src.extend(k, [r](size_t i) { return static_cast<int64_t>(r[i].src); });
    dst.extend(k, [r](size_t i) { return static_cast<int64_t>(r[i].dst); });
    mid.extend(k, [r](size_t i) { return static_cast<int64_t>(r[i].id); });
    type.extend(k, [r](size_t i) { return r[i].type; });
    oid.extend_nullable(k, [r](size_t i, int64_t &v) {
      v = r[i].order_id;
      return r[i].order_id != kNull;
    });
    causal.extend_nullable(k, [r](size_t i, int64_t &v) {
      v = static_cast<int64_t>(r[i].causal);
      return r[i].causal != 0;
    });
  }
  out[0] = pq::sequence_column(n, seq0);
  out[1] = t_recv.finish();
  out[2] = t_send.finish();
  out[3] = lat.finish();
  out[4] = src.finish();
  out[5] = dst.finish();
  out[6] = mid.finish();
  out[7] = type.finish();
  out[8] = oid.finish();
  out[9] = causal.finish();
}

// --- whole mode
// ---------------------------------------------------------------------------------

void LedgerStream::init_whole(size_t cap, size_t prefault) {
  whole_.init(cap, prefault);
  base_ = whole_.data();
  mask_ = ~size_t{0};
  n_ = 0;
  limit_ = whole_.capacity();
}

void LedgerStream::boundary() {
  if (streamer_)
    return streamer_->ledger_boundary();
  whole_.set_size(n_);
  whole_.ensure(n_ + 1);
  base_ = whole_.data();
  limit_ = whole_.capacity();
}

void TraceStream::init_whole(size_t cap, size_t prefault) {
  whole_.init(cap, prefault);
  base_ = whole_.data();
  mask_ = ~size_t{0};
  n_ = 0;
  limit_ = whole_.capacity();
}

void TraceStream::boundary(int64_t t) {
  if (streamer_)
    return streamer_->trace_boundary(t);
  whole_.set_size(n_);
  whole_.ensure(n_ + 1);
  base_ = whole_.data();
  limit_ = whole_.capacity();
}

// --- Streamer
// -------------------------------------------------------------------------------------

namespace {
constexpr uint64_t kFailed =
    ~uint64_t{0} >> 2; // progress counters of a failed thread: never wait
constexpr size_t kPresizeTypes = size_t{32}
                                 << 20; // types_ entries (1 byte each)
constexpr size_t kPresizeOrders = size_t{8}
                                  << 20; // last_pos_ entries (4 bytes each)
constexpr size_t kLazyTypes = size_t{1} << 20,
                 kLazyOrders = size_t{1} << 18; // 1 MB each
// Minimum trace rows for finish_trace() mark and msg_type helper threads.
constexpr unsigned kFinishThreads = 4;
constexpr size_t kParFinishRows = size_t{1} << 18;

// Grows a zero-filled table to hold `need` entries: doubling, but not past its
// reserved capacity while that suffices (no reallocation, and only the new
// pages are touched).
template <class V> void grow_to(V &v, size_t need) {
  size_t n = std::max(need, 2 * v.size());
  if (need <= v.capacity())
    n = std::min(n, v.capacity());
  v.resize(n);
}
} // namespace

namespace {
// T3_RING_GROUPS (tests): groups each ring holds (a power of two).
size_t stream_ring_groups() {
  static const size_t ring_groups =
      std::getenv("T3_RING_GROUPS")
          ? std::strtoull(std::getenv("T3_RING_GROUPS"), nullptr, 10)
          : 4;
  if (ring_groups < 2 || (ring_groups & (ring_groups - 1)))
    throw std::runtime_error("T3_RING_GROUPS: power of two >= 2");
  return ring_groups;
}
} // namespace

size_t Streamer::group_for(size_t est_deliveries) {
  if (const char *env = std::getenv("T3_STREAM_GROUP")) {
    const size_t group = std::strtoull(env, nullptr, 10);
    if (group < 1 || (group & (group - 1)))
      throw std::runtime_error("T3_STREAM_GROUP: power of two");
    return group;
  }
  // Smaller runs use smaller groups; large runs keep 16384 rows per group.
  return est_deliveries >= (size_t{1} << 20) ? 16384 : 4096;
}

size_t Streamer::arena_bytes(size_t group, bool with_ledger) {
  const size_t rows = group * stream_ring_groups();
  return (with_ledger ? Arena::need<LedgerRow>(rows) : 0) +
         Arena::need<TraceRec>(rows);
}

Streamer::Streamer(LedgerStream &ledger, TraceStream &trace,
                   const std::string &ledger_path,
                   const std::string &trace_path, size_t est_trace_rows,
                   size_t est_orders, size_t group, Arena *arena,
                   bool ledger_skip, uint64_t ledger_cap, bool split)
    : L_(ledger), T_(trace) {
  ledger_skipped_ = ledger_skip;
  split_ = split && !ledger_skip;
  ledger_cap_ = ledger_cap;
  group_ = group;
  ledger_groups_ = stream_ring_groups();
  tring_rows_ = group_ * ledger_groups_;
  if (arena) { // populated already, or by the thread below
    if (!ledger_skipped_)
      lring_ = arena->take<LedgerRow>(group_ * ledger_groups_);
    tring_ = arena->take<TraceRec>(tring_rows_);
    if (arena->deferred()) {
      populate_base_ = arena->base();
      populate_bytes_ = arena->used();
    }
  } else { // zeroed: faulted in before the window
    if (!ledger_skipped_)
      own_lring_.reset(new LedgerRow[group_ * ledger_groups_]());
    own_tring_.reset(new TraceRec[tring_rows_]());
    lring_ = own_lring_.get();
    tring_ = own_tring_.get();
  }
  (void)dicts(); // built here, not on the thread during the window
  if (!ledger_skipped_)
    lw_.open(ledger_path, kLedgerCols);
  tw_.open(trace_path, kTraceCols, true);
  L_.n_ = 0;
  if (ledger_skipped_) { // no ring and no group boundaries: the kernel's rows
                         // go nowhere
    L_.init_skipped();
  } else {
    L_.streamer_ = this;
    L_.base_ = lring_;
    L_.mask_ = group_ * ledger_groups_ - 1;
    L_.limit_ = group_;
  }
  T_.streamer_ = this;
  T_.base_ = tring_;
  T_.mask_ = tring_rows_ - 1;
  T_.n_ = 0;
  T_.limit_ = group_;
  // The ledger's thread first: if it cannot be started, one thread does both,
  // as before, and nothing has to be undone.
  if (split_) {
    try {
      lthread_ = std::thread([this] {
        try {
          while (go_.load(std::memory_order_acquire) == 0)
            futex_wait(go_, 0);
          // Its own heap for the nine columns' buffers of a group, reused group
          // after group.
          if (!stop_.load(std::memory_order_acquire))
            prewarm_output_heap(64 * group_ + (size_t{256} << 10));
        } catch (...) {
          mark_failed(std::current_exception());
          return;
        }
        run_ledger();
      });
    } catch (const std::system_error &) {
      split_ = false;
    }
  }
  try {
    start_trace_thread(est_trace_rows, est_orders);
  } catch (
      ...) { // the ledger thread, if any, is parked on go_: release and join it
    stop_thread();
    throw;
  }
}

void Streamer::start_trace_thread(size_t est_trace_rows, size_t est_orders) {
  thread_ = std::thread([this, est_trace_rows, est_orders] {
    bool warm = true;
    try {
      static const bool wait = std::getenv("T3_STREAMER_WAIT") != nullptr;
      // Parked until the window opens: the main thread maps memory until then,
      // and would wait for a populate running here (Arena).
      if (!wait)
        while (go_.load(std::memory_order_acquire) == 0)
          futex_wait(go_, 0);
      if (!stop_.load(std::memory_order_acquire)) {
        populate_rings();
        warm_up(est_trace_rows, est_orders);
      }
    } catch (...) {
      mark_failed(std::current_exception());
      warm = false;
    }
    ready_.store(1, std::memory_order_seq_cst);
    futex_wake_all(ready_);
    if (warm)
      run();
  });
}

// The thread, as the window opens: what the event loop writes from its first
// event on. The start of the message log and of each ring first (a group takes
// it about a millisecond to fill), then the rest.
void Streamer::populate_rings() {
  if (!populate_bytes_)
    return;
  constexpr size_t kHead = size_t{128} << 10;
  char *const t = reinterpret_cast<char *>(tring_);
  char *const l = lring_ ? reinterpret_cast<char *>(lring_) : t;
  const size_t log_bytes = static_cast<size_t>(std::min(l, t) - populate_base_);
  Arena::populate(populate_base_, std::min(kHead, log_bytes));
  Arena::populate(t, std::min(kHead, tring_rows_ * sizeof(TraceRec)));
  if (lring_)
    Arena::populate(
        l, std::min(kHead, group_ * ledger_groups_ * sizeof(LedgerRow)));
  Arena::populate(populate_base_, populate_bytes_);
}

// The thread: what it touches itself. The heap is warmed for the per-group
// encoder buffers (at most ~80 bytes per row of a ledger group plus a trace
// group), which malloc reuses group after group. The tables that grow with the
// run reserve at most 32 MB each and fault in only their first 1 MB here;
// encode_trace() touches the rest as they grow. T3_STREAMER_EAGER faults in
// the full reservation.
void Streamer::warm_up(size_t est_trace_rows, size_t est_orders) {
  static const bool lazy = std::getenv("T3_STREAMER_EAGER") == nullptr;
  touch_stack();
  scratch_.resize(2 * group_);
  const size_t n_types = std::min(est_trace_rows, kPresizeTypes);
  const size_t n_orders = std::min(est_orders, kPresizeOrders);
  types_.reserve(n_types);
  last_pos_.reserve(n_orders);
  types_.resize(lazy ? std::min(n_types, kLazyTypes) : n_types);
  last_pos_.assign(lazy ? std::min(n_orders, kLazyOrders) : n_orders, 0);
  t_group_rows_.reserve(n_types / group_ + 16);
  prewarm_output_heap(96 * group_ + (size_t{256} << 10));
}

// Either thread; the first failure is the one reported.
void Streamer::mark_failed(std::exception_ptr e) {
  if (!err_claimed_.exchange(true, std::memory_order_acq_rel))
    err_ = std::move(e);
  failed_.store(1, std::memory_order_release);
  l_encoded_.store(kFailed, std::memory_order_release);
  t_consumed_.store(kFailed, std::memory_order_release);
  t_taken_.store(kFailed, std::memory_order_release);
  released_.store(1, std::memory_order_release);
  futex_wake_all(released_);
}

// A write error (a full disk, a file-size limit) ends the run at the next group
// boundary rather than after the whole simulation.
void Streamer::fail() {
  // A thread that did not fail may be asleep, or waiting for the window to
  // open: release it.
  stop_thread();
  std::rethrow_exception(err_);
}

Streamer::~Streamer() { stop_thread(); }

void Streamer::stop_thread() {
  if (!thread_.joinable() && !lthread_.joinable())
    return;
  stop_.store(true, std::memory_order_seq_cst);
  go_.store(1, std::memory_order_seq_cst);
  futex_wake_all(go_);
  wake_.fetch_add(1, std::memory_order_seq_cst);
  futex_wake_all(wake_);
  lwake_.fetch_add(1, std::memory_order_seq_cst);
  futex_wake_all(lwake_);
  if (thread_.joinable())
    thread_.join();
  if (lthread_.joinable())
    lthread_.join();
}

// Releases the thread, which then faults in its memory while the event loop
// already runs: the loop hands it nothing before its first group is full, the
// rings hold four groups, and a thread that failed is noticed at the first
// boundary. T3_STREAMER_WAIT waits for the thread to fault in its memory before
// the window.
void Streamer::go() {
  static const bool wait = std::getenv("T3_STREAMER_WAIT") != nullptr;
  if (wait) {
    while (ready_.load(std::memory_order_acquire) == 0)
      futex_wait(ready_, 0);
    if (failed_.load(std::memory_order_acquire))
      fail();
  }
  go_.store(1, std::memory_order_seq_cst);
  futex_wake_all(go_);
}

void Streamer::wake() {
  wake_.fetch_add(1, std::memory_order_seq_cst);
  if (sleeping_.load(std::memory_order_seq_cst))
    futex_wake_all(wake_);
}

// Whichever thread encodes the ledger.
void Streamer::wake_ledger() {
  if (!split_)
    return wake();
  lwake_.fetch_add(1, std::memory_order_seq_cst);
  if (lsleeping_.load(std::memory_order_seq_cst))
    futex_wake_all(lwake_);
}

template <class Pred> void Streamer::wait_until(Pred done) {
  if (done())
    return;
  ++waits_;
  for (unsigned spins = 0; !done(); ++spins) {
    if (spins > 4096)
      std::this_thread::yield();
    else
      T3_POOL_PAUSE();
  }
}

// Main thread, at the first row of each ledger group: hands the finished groups
// to the thread, and waits (rarely) until the ring slot this group will use has
// been written out.
void Streamer::ledger_boundary() {
  if (failed_.load(std::memory_order_acquire)) [[unlikely]]
    fail();
  const size_t g = L_.n_ / group_;
  l_published_.store(g, std::memory_order_seq_cst);
  wake_ledger();
  if (g >= ledger_groups_) {
    const uint64_t need = g - ledger_groups_ + 1;
    wait_until(
        [&] { return l_encoded_.load(std::memory_order_acquire) >= need; });
  }
  L_.limit_ = L_.n_ + group_;
}

// Main thread, when the row about to be written (time t) is at least group_
// rows into the open group: cuts the group there if t changes (every row of an
// older t is final), else extends the equal-t run by one row. Either way the
// rows up to the next check must fit in the ring: wait for the thread to copy
// groups out, or grow the ring when the open group alone does not fit.
void Streamer::trace_boundary(int64_t t) {
  if (failed_.load(std::memory_order_acquire)) [[unlikely]]
    fail();
  const size_t n = T_.n_;
  if (n - t_open_ >= group_ && T_.base_[(n - 1) & T_.mask_].t != t) {
    wait_until([&] {
      return t_taken_.load(std::memory_order_acquire) + kMaxCuts > t_cut_count_;
    });
    t_cuts_[t_cut_count_ % kMaxCuts] = n;
    t_published_.store(++t_cut_count_, std::memory_order_seq_cst);
    wake();
    t_open_ = n;
  }
  const size_t lim = n - t_open_ >= group_ ? n + 1 : t_open_ + group_;
  if (t_consumed_.load(std::memory_order_acquire) + tring_rows_ < lim) {
    if (lim - t_open_ > tring_rows_) {
      // Every published group has to be copied out first: then the thread holds
      // no reference into the ring until the next publish, which carries the
      // new one.
      wait_until([&] {
        return t_consumed_.load(std::memory_order_acquire) >= t_open_;
      });
      grow_trace_ring();
    } else {
      wait_until([&] {
        return t_consumed_.load(std::memory_order_acquire) + tring_rows_ >= lim;
      });
    }
  }
  T_.limit_ = lim;
}

void Streamer::grow_trace_ring() {
  const size_t rows = tring_rows_ * 2;
  std::unique_ptr<TraceRec[]> r(new TraceRec[rows]());
  for (size_t i = t_open_; i < T_.n_; ++i)
    r[i & (rows - 1)] = tring_[i & (tring_rows_ - 1)];
  own_tring_ = std::move(r);
  tring_ = own_tring_.get();
  tring_rows_ = rows;
  ++grows_;
  T_.base_ = tring_;
  T_.mask_ = rows - 1;
}

// The trace's thread, which without a split is the ledger's too: encodes and
// appends every published group, ledger and trace alternately. It stops only
// between groups; finish() does the rest.
void Streamer::run() {
  try {
    while (go_.load(std::memory_order_acquire) == 0)
      futex_wait(go_, 0);
    const bool ledger = !split_;
    uint64_t lg = 0;
    for (;;) {
      if (fin_.load(std::memory_order_acquire)) {
        window_cpu_ms_ = thread_cpu_ms();
        // Hand the ledger back to finish() (which encodes the rest of it), then
        // finish the trace.
        if (ledger) {
          released_.store(1, std::memory_order_release);
          futex_wake_all(released_);
        }
        finish_trace();
        return;
      }
      bool did = false;
      if (ledger && l_published_.load(std::memory_order_acquire) > lg) {
        encode_ledger(lg, group_);
        l_encoded_.store(++lg, std::memory_order_release);
        did = true;
      }
      if (t_published_.load(std::memory_order_acquire) > t_taken_groups_) {
        const size_t hi = t_cuts_[t_taken_groups_ % kMaxCuts];
        encode_trace(t_lo_, hi);
        did = true;
      }
      if (did)
        continue;
      const uint32_t seen = wake_.load(std::memory_order_seq_cst);
      sleeping_.store(1, std::memory_order_seq_cst);
      if ((ledger && l_published_.load(std::memory_order_seq_cst) > lg) ||
          t_published_.load(std::memory_order_seq_cst) > t_taken_groups_) {
        sleeping_.store(0, std::memory_order_relaxed);
        continue;
      }
      if (stop_.load(std::memory_order_seq_cst)) {
        sleeping_.store(0, std::memory_order_relaxed);
        return;
      }
      if (fin_.load(std::memory_order_seq_cst)) { // set after `seen` was read:
                                                  // no wake is coming
        sleeping_.store(0, std::memory_order_relaxed);
        continue;
      }
      futex_wait(wake_, seen);
      sleeping_.store(0, std::memory_order_relaxed);
    }
  } catch (...) {
    mark_failed(std::current_exception());
  }
}

// The ledger's own thread: every published ledger group, until finish() takes
// the ledger back.
void Streamer::run_ledger() {
  try {
    uint64_t lg = 0;
    for (;;) {
      if (fin_.load(std::memory_order_acquire)) {
        ledger_cpu_ms_ = thread_cpu_ms();
        released_.store(1, std::memory_order_release);
        futex_wake_all(released_);
        return;
      }
      if (l_published_.load(std::memory_order_acquire) > lg) {
        encode_ledger(lg, group_);
        l_encoded_.store(++lg, std::memory_order_release);
        continue;
      }
      const uint32_t seen = lwake_.load(std::memory_order_seq_cst);
      lsleeping_.store(1, std::memory_order_seq_cst);
      if (l_published_.load(std::memory_order_seq_cst) > lg ||
          fin_.load(std::memory_order_seq_cst)) {
        lsleeping_.store(0, std::memory_order_relaxed); // published or finished
                                                        // after `seen` was read
        continue;
      }
      if (stop_.load(std::memory_order_seq_cst)) {
        lsleeping_.store(0, std::memory_order_relaxed);
        return;
      }
      futex_wait(lwake_, seen);
      lsleeping_.store(0, std::memory_order_relaxed);
    }
  } catch (...) {
    mark_failed(std::current_exception());
  }
}

void Streamer::encode_ledger(size_t g, size_t rows) {
  ++l_appended_;
  if (ledger_skipped_ || ledger_dropped_)
    return;
  std::vector<pq::Chunk> chunks(kLedgerCols);
  encode_ledger_group(lring_ + (g % ledger_groups_) * group_, rows,
                      static_cast<int64_t>(g * group_), chunks.data());
  if (ledger_cap_ && static_cast<uint64_t>(lw_.bytes()) +
                             pq::StreamWriter::bytes_of(chunks) +
                             lw_.footer_bound(lw_.groups() + 1) >
                         ledger_cap_) {
    ledger_dropped_ = true;
    return;
  }
  lw_.append(chunks, static_cast<int64_t>(rows));
}

// Copies rows [lo, hi) out of the ring (freeing their slots), then sorts and
// encodes them as the next row group: every column but msg_type, whose codes
// wait in types_ until finish().
void Streamer::encode_trace(size_t lo, size_t hi) {
  const size_t n = hi - lo;
  if (scratch_.size() < n)
    scratch_.resize(n);
  TraceRec *tr = scratch_.data();
  const size_t mask = tring_rows_ - 1;
  for (size_t i = 0; i < n;) { // at most two contiguous pieces
    const size_t at = (lo + i) & mask;
    const size_t k = std::min(n - i, tring_rows_ - at);
    std::memcpy(tr + i, tring_ + at, k * sizeof(TraceRec));
    i += k;
  }
  t_lo_ = hi;
  ++t_taken_groups_;
  t_consumed_.store(hi, std::memory_order_release);
  t_taken_.store(t_taken_groups_, std::memory_order_release);

  sort_runs(tr, n);
  if (types_.size() < hi)
    grow_to(types_, hi);
  for (size_t i = 0; i < n; ++i) {
    types_[lo + i] = tr[i].type;
    if (tr[i].type == static_cast<uint8_t>(TraceType::PartialFill)) {
      const size_t oid = static_cast<size_t>(tr[i].order_id);
      if (oid >= last_pos_.size())
        grow_to(last_pos_, oid + 1);
      last_pos_[oid] = static_cast<uint32_t>(lo + i + 1);
    }
  }
  std::vector<pq::Chunk> chunks(kTraceCols);
  encode_trace_rows(tr, n, false, chunks.data());
  tw_.append(chunks, static_cast<int64_t>(n), 2);
  t_group_rows_.push_back(static_cast<int64_t>(n));
}

void Streamer::finish() {
  fin_.store(1, std::memory_order_seq_cst);
  wake_.fetch_add(1, std::memory_order_seq_cst);
  futex_wake_all(wake_);
  lwake_.fetch_add(1, std::memory_order_seq_cst);
  futex_wake_all(lwake_);
  while (released_.load(std::memory_order_acquire) == 0)
    futex_wait(released_, 0);
  if (failed_.load(std::memory_order_acquire))
    fail();
  // Ledger: groups the thread did not reach, then the partial last group (the
  // thread encodes no ledger group after releasing it).
  if (!ledger_skipped_) {
    const size_t full = L_.n_ / group_;
    for (size_t g = l_appended_; g < full; ++g)
      encode_ledger(g, group_);
    if (L_.n_ % group_ || L_.n_ == 0)
      encode_ledger(full, L_.n_ % group_);
    if (ledger_dropped_)
      lw_.abandon();
    else
      lw_.finish(kLedgerSchema, kLedgerPandas, kCreatedBy);
  }
  thread_.join();
  if (lthread_.joinable())
    lthread_.join();
  if (err_)
    std::rethrow_exception(err_);
  trace_sha256_ = tw_.sha256_hex();
}

// On the thread, after finish() released it: published groups not taken yet,
// then the open group; each order's last execution becomes ORDER_FILLED; then
// the msg_type chunks and the footer.
void Streamer::finish_trace() {
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  while (t_taken_groups_ < t_cut_count_)
    encode_trace(t_lo_, t_cuts_[t_taken_groups_ % kMaxCuts]);
  if (T_.n_ > t_open_ || tw_.groups() == 0)
    encode_trace(t_open_, T_.n_);
  const auto t1 = clock::now();

  // Then two passes over every row: the ORDER_FILLED marks, and the msg_type
  // chunk of each group (which reads the marks). On large runs both are split
  // over kFinishThreads threads (the other CPUs are idle by now); each thread
  // writes its own marks and chunks, so the bytes are the same.
  const size_t ng = t_group_rows_.size();
  std::vector<size_t> start(ng + 1, 0);
  for (size_t g = 0; g < ng; ++g)
    start[g + 1] = start[g] + static_cast<size_t>(t_group_rows_[g]);
  std::vector<pq::Chunk> types(ng);
  static const size_t par_min =
      std::getenv("T3_PAR_FINISH_MIN")
          ? std::strtoull(std::getenv("T3_PAR_FINISH_MIN"), nullptr, 10)
          : kParFinishRows;
  // One thread per 128k rows, up to kFinishThreads.
  unsigned k = start[ng] >= par_min ? static_cast<unsigned>(std::clamp<size_t>(
                                          start[ng] >> 17, 1, kFinishThreads))
                                    : 1;
  std::atomic<unsigned> marked{0};
  std::atomic<int> gate{
      0}; // helpers start on 1; 2: one failed to start, the caller does it all
  std::exception_ptr errs[kFinishThreads];
  auto part = [&](unsigned w) {
    try {
      const size_t n = last_pos_.size();
      for (size_t i = n * w / k, e = n * (w + 1) / k; i < e; ++i)
        if (const uint32_t p = last_pos_[i])
          types_[p - 1] = static_cast<uint8_t>(TraceType::Filled);
    } catch (...) {
      errs[w] = std::current_exception();
    }
    marked.fetch_add(1, std::memory_order_acq_rel); // every mark is in before
                                                    // any chunk is encoded
    while (marked.load(std::memory_order_acquire) < k)
      T3_POOL_PAUSE();
    try {
      for (size_t g = ng * w / k, e = ng * (w + 1) / k; g < e; ++g)
        types[g] = encode_trace_types(types_.data() + start[g],
                                      start[g + 1] - start[g]);
    } catch (...) {
      if (!errs[w])
        errs[w] = std::current_exception();
    }
  };
  std::vector<std::thread> helpers;
  try {
    for (unsigned w = 1; w < k; ++w)
      helpers.emplace_back([&, w] {
        int g;
        while ((g = gate.load(std::memory_order_acquire)) == 0)
          T3_POOL_PAUSE();
        if (g == 1)
          part(w);
      });
    gate.store(1, std::memory_order_release);
  } catch (...) {
    k = 1; // before the helpers read it: they only do so once the gate opens,
           // and it will not
    gate.store(2, std::memory_order_release);
  }
  part(0);
  for (std::thread &h : helpers)
    h.join();
  for (unsigned w = 0; w < k; ++w)
    if (errs[w])
      std::rethrow_exception(errs[w]);
  const auto t3 = clock::now();
  tw_.finish(kTraceSchema, kTracePandas, kCreatedBy, 2, &types);
  const auto t4 = clock::now();
  auto ms = [](auto a, auto b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };
  finish_ms_[0] = ms(t0, t1), finish_ms_[1] = static_cast<double>(k),
  finish_ms_[2] = ms(t1, t3), finish_ms_[3] = ms(t3, t4);
  if (std::getenv("T3_PHASES")) {
    const double *w = tw_.finish_parts();
    std::fprintf(stderr,
                 "  trace last write: layout+footer %.2f ms, hash+write %.2f "
                 "ms, close %.2f ms; %.2f MiB, footer %.0f KiB\n",
                 w[0], w[1], w[2], w[3] / 1048576.0, w[4] / 1024.0);
  }
}

std::string sha256_hex(const pq::File &f) {
  Sha256 h;
  for (const pq::Seg &s : f.segs)
    h.update(s.p, s.n);
  return Sha256::hex(h.digest());
}

Outputs build_outputs(SimResult &res, SpinPool *pool, bool with_ledger) {
  const size_t nl = res.ledger.size();
  // Runs fn(k) for k in [0, n) on the pool, or inline.
  auto run = [pool](size_t n, auto &&fn) {
    if (pool && n > 1)
      pool->run(n, fn);
    else
      for (size_t k = 0; k < n; ++k)
        fn(k);
  };

  const bool dbg = std::getenv("T3_PHASES") != nullptr;
  auto q0 = std::chrono::steady_clock::now();
  auto lap = [&](const char *what) {
    if (!dbg)
      return;
    const auto q = std::chrono::steady_clock::now();
    std::fprintf(stderr, "  out %-10s %.2f ms\n", what,
                 std::chrono::duration<double, std::milli>(q - q0).count());
    q0 = q;
  };

  const std::vector<Range> tg =
      res.streamed ? std::vector<Range>{} : trace_groups(res.trace);
  const std::vector<Range> lg =
      res.streamed || !with_ledger ? std::vector<Range>{} : ledger_groups(nl);

  // Phase 1: the last execution of every order, from all trace groups.
  std::unique_ptr<uint64_t[], decltype(&std::free)> last(nullptr, &std::free);
  if (!tg.empty()) {
    last.reset(static_cast<uint64_t *>(std::calloc(
        static_cast<size_t>(res.n_order_ids) + 1, sizeof(uint64_t))));
    if (!last)
      throw std::bad_alloc();
  }
  run(tg.size(),
      [&](size_t k) { mark_last_fills(res.trace, tg[k], last.get()); });
  lap("marks");

  // Phase 2: every row group of both files (ledger groups first: they cost more
  // per row).
  Outputs o;
  o.trace.chunks.resize(tg.size() * kTraceCols);
  o.ledger.chunks.resize(lg.size() * kLedgerCols);
  const size_t todo = lg.size();
  run(todo + tg.size(), [&](size_t k) {
    if (k < todo) {
      const Range g = lg[k];
      encode_ledger_group(res.ledger.data() + g.first, g.second - g.first,
                          static_cast<int64_t>(g.first),
                          &o.ledger.chunks[k * kLedgerCols]);
    } else {
      const size_t g = k - todo;
      encode_trace_group(res.trace.data(), tg[g], last.get(),
                         &o.trace.chunks[g * kTraceCols]);
    }
  });
  lap("encode");

  if (!res.streamed) {
    pq::assemble(o.trace, kTraceSchema, group_rows(tg), kTracePandas,
                 kCreatedBy);
    if (with_ledger)
      pq::assemble(o.ledger, kLedgerSchema, group_rows(lg), kLedgerPandas,
                   kCreatedBy);
  }
  lap("assemble");
  return o;
}

namespace {

int create(const std::string &path) {
  const int fd =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0)
    throw std::runtime_error("cannot create " + path + ": " +
                             std::strerror(errno));
  return fd;
}

void finish(int fd, const std::string &path) {
  if (::close(fd) != 0)
    throw std::runtime_error("close failed for " + path);
}

} // namespace

void write_file(const std::string &path, const pq::File &f) {
  std::vector<iovec> iov;
  iov.reserve(f.segs.size());
  for (const pq::Seg &s : f.segs)
    iov.push_back({const_cast<uint8_t *>(s.p), s.n});
  const int fd = create(path);
  size_t i = 0;
  while (i < iov.size()) {
    const int cnt =
        static_cast<int>(std::min<size_t>(iov.size() - i, 1024)); // IOV_MAX
    const ssize_t w = ::writev(fd, iov.data() + i, cnt);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      ::close(fd);
      throw std::runtime_error("write failed for " + path + ": " +
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
  finish(fd, path);
}

void write_file(const std::string &path, const std::string &bytes) {
  const int fd = create(path);
  size_t done = 0;
  while (done < bytes.size()) {
    const ssize_t w = ::write(fd, bytes.data() + done, bytes.size() - done);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      ::close(fd);
      throw std::runtime_error("write failed for " + path + ": " +
                               std::strerror(errno));
    }
    done += static_cast<size_t>(w);
  }
  finish(fd, path);
}

long long peak_rss_bytes() {
  struct rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  return static_cast<long long>(ru.ru_maxrss) * 1024;
}

} // namespace t3
