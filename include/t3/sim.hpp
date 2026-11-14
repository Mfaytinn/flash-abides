// The simulation core: kernel, exchange, order book, agents, oracle and latency
// model, reproducing the pinned Python ABIDES + abides_fork adapter event for
// event.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "t3/bigarray.hpp"
#include "t3/parquet.hpp"
#include "t3/scenario.hpp"

namespace t3 {

// Message types; the order matches the ledger vocabulary table in output.cpp.
enum class Msg : uint8_t {
  Wakeup = 0,
  MarketClosePriceRequest,
  MarketHoursRequest,
  MarketHours,
  MarketClosePrice,
  MarketClosed,
  QuerySpread,
  QuerySpreadResponse,
  LimitOrder,
  CancelOrder,
  OrderAccepted,
  OrderExecuted,
  OrderCancelled,
  Count
};

// Trace row types; the order matches the trace vocabulary table in output.cpp.
enum class TraceType : uint8_t {
  Submitted = 0,
  Accepted = 1,
  Filled = 2,
  PartialFill = 3,
  Cancelled = 4,
  Replaced = 5,
  Quote = 6
};

inline constexpr int64_t kNull = -1; // order ids are never negative

struct TraceRec {
  int64_t t;
  int64_t order_id; // -1 for quotes
  int64_t price;
  int64_t size;
  int32_t agent;
  uint8_t type; // TraceType
  uint8_t side; // 0 = BID, 1 = ASK
  uint16_t pad;
};
static_assert(sizeof(TraceRec) == 40);

// A message's log entry, written when it is constructed and sent. Its first 32
// bytes have the layout of a LedgerRow's, so a delivery copies them into its
// ledger row in one move.
struct LogEntry {
  uint64_t t_sent;
  uint64_t t_sched; // original scheduled delivery (ledger t_recv)
  int64_t order_id; // kNull when the message carries no order
  uint32_t causal;  // 0 = null (ids start at 1)
  uint8_t type;     // Msg
  uint8_t side;
  uint8_t flags;
  uint8_t pad;
  int64_t price; // limit / fill price; spread response: bid
  int64_t qty;   // order size; spread response: ask
};
static_assert(sizeof(LogEntry) == 48);

// One ledger row, captured when the message (or wakeup) is delivered.
struct LedgerRow {
  int64_t t_send;   // kNull for wakeups
  int64_t t_recv;   // the original scheduled delivery; a wakeup's effective
                    // delivery time
  int64_t order_id; // kNull when the message carries no order
  uint32_t causal;  // 0 = null
  uint8_t type;     // Msg
  uint8_t pad[3];   // (a message's side/flags/pad bytes land here)
  uint32_t id;      // message id
  uint16_t src, dst;
};
static_assert(sizeof(LedgerRow) == 40);
static_assert(offsetof(LedgerRow, t_send) == offsetof(LogEntry, t_sent) &&
              offsetof(LedgerRow, t_recv) == offsetof(LogEntry, t_sched) &&
              offsetof(LedgerRow, order_id) == offsetof(LogEntry, order_id) &&
              offsetof(LedgerRow, causal) == offsetof(LogEntry, causal) &&
              offsetof(LedgerRow, type) == offsetof(LogEntry, type));

struct SimResult {
  BigArray<TraceRec> trace; // emission order; build_outputs() canonicalizes it
  BigArray<LedgerRow> ledger; // delivery order
  bool streamed =
      false; // both files were written during the run (the arrays are empty)
  std::string trace_sha256; // streamed: of the trace.parquet written
  size_t n_trace_rows = 0, n_deliveries = 0;
  int64_t n_order_ids = 0;  // order ids are 0 .. n_order_ids - 1
  double sim_seconds = 0.0; // the timed simulation window
  double setup_seconds =
      0.0; // seeding, capacity reservation and pre-faulting before t0
  size_t est_msgs = 0, est_rows = 0;   // capacity estimates (diagnostics)
  const char *ledger_fate = "written"; // streamed: "written", "skipped" or
                                       // "dropped" (SimOptions::ledger_cap)
};

inline constexpr size_t kLedgerRowGroup =
    size_t{1} << 17; // ledger rows per Parquet row group (whole mode)

struct SimOptions {
  bool latency_thread =
      true; // compute latency draws on a producer thread (singles)
  bool deferral =
      true; // per-recipient deferral instead of repeated busy requeues
  bool prefault =
      true; // fault in the estimated capacity before the timed window
  bool lanes =
      true; // recurring wakeups in per-interval FIFO lanes instead of the heap
  bool draw_thread =
      false; // NoiseTrader draws on a producer thread (needs a third CPU)
  bool stream = false; // large runs: stream both files during the window (a
                       // fourth thread)
  unsigned threads =
      4; // CPU budget: below 4, only very large runs stream (for memory)
  std::string trace_path,
      ledger_path; // trace.parquet, message_trace.parquet (streaming only)
  // A unit whose ledger is optional (a throughput-scale single): the most bytes
  // its ledger file may take (0: no limit), or skip it outright. Streaming
  // only; see Streamer.
  uint64_t ledger_cap = 0;
  bool ledger_skip = false;
};

SimResult simulate(const Scenario &s, const SimOptions &o = {});

} // namespace t3
