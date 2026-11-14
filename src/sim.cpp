// Simulation core. Everything that runs per event lives in this translation
// unit so the compiler can inline across the kernel, the exchange and the
// agents.
#include "t3/sim.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <vector>

#include "t3/book.hpp"
#include "t3/deferqueue.hpp"
#include "t3/drawstream.hpp"
#include "t3/eventheap.hpp"
#include "t3/eventqueue.hpp"
#include "t3/eventwheel.hpp"
#include "t3/latency.hpp"
#include "t3/nswheel.hpp"
#include "t3/openorders.hpp"
#include "t3/repset.hpp"
#include "t3/rng.hpp"
#include "t3/streams.hpp"

namespace t3 {
namespace {

// Whole-mode buffers: the most rows an estimate reserves (they grow past it),
// and pre-faults.
constexpr size_t kWholeCapRows = size_t{64} << 20;
constexpr size_t kWholePrefaultRows = size_t{8} << 20;
// Estimated ledger file bytes per delivery.
constexpr double kLedgerBytesPerDelivery = 12.0;
// Minimum estimated deliveries for arena huge pages (t3/arena.hpp).
constexpr size_t kArenaHugeMsgs = size_t{1} << 20;

// ---------------------------------------------------------------------------------------------
// Event keys: the ABIDES priority (t, sender, recipient, message_id) packed
// into one u128 so the heap compares with a single 128-bit compare. A key with
// sender == recipient is a wakeup.
// ---------------------------------------------------------------------------------------------
// s and r are agent indices below 2^16.
inline u128 make_key(uint64_t t, uint64_t s, uint64_t r, uint32_t id) {
  return (static_cast<u128>(t) << 64) | (s << 48) | (r << 32) | id;
}
inline uint64_t key_t(u128 k) { return static_cast<uint64_t>(k >> 64); }
inline uint32_t key_s(u128 k) {
  return static_cast<uint32_t>(static_cast<uint64_t>(k) >> 48);
}
inline uint32_t key_r(u128 k) {
  return static_cast<uint32_t>((static_cast<uint64_t>(k) >> 32) & 0xFFFF);
}
inline uint32_t key_id(u128 k) { return static_cast<uint32_t>(k); }
inline u128 with_time(u128 k, uint64_t t) {
  return (static_cast<u128>(t) << 64) | static_cast<uint64_t>(k);
}

// Reserves capacity for n elements and writes it once (so the pages are faulted
// in now), leaving the vector empty.
template <class V> void prepare(V &v, size_t n) {
  v.resize(std::max(n, v.size()));
  v.clear();
}

// The median latency of the scenario's model, in ns: the timing wheel's slot
// width follows it.
double latency_median_ns(const Scenario &s) {
  double v = 1.0;
  switch (s.latency_model) {
  case LatencyModel::LogNormal:
    v = std::exp(s.lat_mu);
    break;
  case LatencyModel::Uniform:
    v = 0.5 * (s.lat_min_ns + s.lat_max_ns);
    break;
  case LatencyModel::Pareto:
    v = (s.lat_min_ns > 0 ? s.lat_min_ns : 1.0) * std::exp2(1.0 / s.lat_alpha);
    break;
  case LatencyModel::Constant:
    v = s.lat_mean_ns;
    break;
  // Two uniform points on the Seattle-NYC line (latency.hpp): the median
  // distance is 0.29 of its length, in light-ns.
  case LatencyModel::Pairwise:
    v = 0.29 * 3866660.0 / 0.299792458;
    break;
  }
  v = std::min(std::max(v, s.lat_min_ns), s.lat_max_ns);
  return std::max(v, 1.0);
}

// Estimate messages in flight: one per agent, or four per market-maker book
// level. Account for overlapping cycles when four latencies exceed an agent's
// wake interval.
double events_in_flight(const Scenario &s) {
  const double cycle_ns = 4.0 * latency_median_ns(s);
  double n = 0;
  for (const AgentSpec &a : s.agents) {
    const double per_cycle =
        a.type == AgentType::MarketMaker
            ? 4.0 * static_cast<double>(std::max<int64_t>(a.depth_levels, 1))
            : 1.0;
    n += per_cycle *
         std::max(1.0, cycle_ns / static_cast<double>(
                                      std::max<uint64_t>(a.interval_ns, 1)));
  }
  return n;
}

// Orders the market makers of a scenario keep resting at once: two per quote
// level each. The book sizes its id table from it (t3/book.hpp).
size_t resting_maker_orders(const Scenario &s) {
  size_t n = 0;
  for (const AgentSpec &a : s.agents)
    if (a.type == AgentType::MarketMaker)
      n += 2 * static_cast<size_t>(std::max<int64_t>(a.depth_levels, 1));
  return n;
}

// The spread of the scenario's latency model, in ns: `bulk`, the span between
// its 10th and 90th percentiles, and `tail`, a value that all but about three
// latencies in a thousand stay under (never above the model's clip). A constant
// model has no spread: bulk is 1.
void latency_spread_ns(const Scenario &s, double &bulk, double &tail) {
  bulk = 1.0;
  tail = 1.0;
  switch (s.latency_model) {
  case LatencyModel::LogNormal: {
    const double m = std::exp(s.lat_mu);
    bulk =
        m * (std::exp(1.2816 * s.lat_sigma) - std::exp(-1.2816 * s.lat_sigma));
    tail = m * std::exp(2.75 * s.lat_sigma);
    break;
  }
  case LatencyModel::Uniform:
    bulk = 0.8 * (s.lat_max_ns - s.lat_min_ns);
    tail = s.lat_max_ns;
    break;
  case LatencyModel::Pareto: { // base * (1 + Lomax(alpha)): P(v > x) = (base /
                               // x)^alpha
    const double base = s.lat_min_ns > 0 ? s.lat_min_ns : 1.0;
    bulk = base * (std::pow(10.0, 1.0 / s.lat_alpha) -
                   std::pow(1.0 / 0.9, 1.0 / s.lat_alpha));
    tail = base * std::pow(333.0, 1.0 / s.lat_alpha);
    break;
  }
  case LatencyModel::Constant:
    tail = s.lat_mean_ns;
    break;
  // Two uniform points on the Seattle-NYC line (latency.hpp), in light-ns: the
  // distance's 10th and 90th percentiles are 0.051 and 0.684 of its length.
  case LatencyModel::Pairwise:
    bulk = 0.633 * 3866660.0 / 0.299792458;
    tail = 3866660.0 / 0.299792458;
    break;
  }
  tail = std::max(std::min(tail, s.lat_max_ns), std::max(s.lat_min_ns, 1.0));
  bulk = std::clamp(bulk, 1.0, tail);
}

// Queue thresholds by estimated events in flight. T3_WHEEL_MIN and
// T3_LINE_MIN override the thresholds.
constexpr double kWheelMinInFlight = 10000.0;
constexpr double kLineMinInFlight = 0.0;

// Which event queue a run's kernel is built on. No wheel under a constant
// latency model, or one whose draws have no spread: every message of a burst is
// then due at the same instant (the exchange answers them all at once, its
// compute delay is 0 on every public unit), so they would fill one slot and
// spill to the heap. T3_QUEUE=heap|wheel|line|ns forces one, whatever the model
// (tests, A/B); T3_NO_WHEEL=1 is T3_QUEUE=heap.
enum class QueueKind { Heap, Wheel, LineWheel, NsWheel };
void line_wheel_geometry(const Scenario &s, unsigned &shift, size_t &slots);
[[maybe_unused]] QueueKind queue_kind(const Scenario &s) {
  if (const char *e = std::getenv("T3_QUEUE")) {
    const std::string q = e;
    if (q == "heap")
      return QueueKind::Heap;
    if (q == "wheel")
      return QueueKind::Wheel;
    if (q == "line")
      return QueueKind::LineWheel;
    if (q == "ns")
      return QueueKind::NsWheel;
    throw std::runtime_error("T3_QUEUE: heap, wheel, line or ns");
  }
  if (std::getenv("T3_NO_WHEEL") || s.latency_model == LatencyModel::Constant)
    return QueueKind::Heap;
  { // a model that draws, but always the same value (a log-normal with sigma 0,
    // equal bounds)
    double bulk = 1.0, tail = 1.0;
    latency_spread_ns(s, bulk, tail);
    if (bulk <= 1.0)
      return QueueKind::Heap;
  }
  const double in_flight = events_in_flight(s);
  double wheel_min = kWheelMinInFlight, line_min = kLineMinInFlight;
  if (const char *e = std::getenv("T3_WHEEL_MIN"))
    wheel_min = std::strtod(e, nullptr);
  if (const char *e = std::getenv("T3_LINE_MIN"))
    line_min = std::strtod(e, nullptr);
  if (in_flight >= wheel_min)
    return QueueKind::Wheel;
  if (in_flight < line_min)
    return QueueKind::Heap;
  // Crowded enough for slots of 1 ns: the wheel of one slot per nanosecond.
  unsigned shift = 0;
  size_t slots = 0;
  line_wheel_geometry(s, shift, slots);
  return shift == 0 ? QueueKind::NsWheel : QueueKind::LineWheel;
}

// The geometry of the timing wheel of large populations: slots of 2^shift ns
// sized so that the events in flight spread to about two per slot, and enough
// of them to cover 32 medians of latency. T3_WHEEL_SHIFT and T3_WHEEL_SLOTS
// override either value.
[[maybe_unused]] void wheel_geometry(const Scenario &s, unsigned &shift,
                                     size_t &slots) {
  const double in_flight = events_in_flight(s);
  const double p50 = latency_median_ns(s);
  const double width = 8.0 * p50 / std::max(in_flight, 8.0);
  shift = static_cast<unsigned>(
      std::clamp<long>(std::lround(std::log2(std::max(width, 1.0))), 0, 40));
  const double want = 32.0 * p50 / std::exp2(static_cast<double>(shift));
  slots = EventQueue::kMinSlots;
  while (slots < 4096 && static_cast<double>(slots) < want)
    slots <<= 1;
  if (const char *e = std::getenv("T3_WHEEL_SHIFT"))
    shift = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
  if (const char *e = std::getenv("T3_WHEEL_SLOTS"))
    slots = std::strtoull(e, nullptr, 10);
}

// The geometry of the one-line wheel (EventWheel). The events of a burst are
// due across the bulk of the latency model, four fifths of them within `bulk`:
// slots of 2^shift ns sized so that those make about kLineKeysPerSlot keys per
// slot (a slot holds two; a third goes to the heap), and enough slots to reach
// the model's tail, at most kLineMaxSlots (256 KiB of slots; what is due later
// goes to the heap). T3_LINE_KEYS, T3_WHEEL_SHIFT and T3_WHEEL_SLOTS override.
// (Each kernel's translation unit uses one of the geometries, and only the
// heap's the choice.)
//
// Runs needing 1 ns slots use NsWheel (queue_kind()), so shift is at least 1
// here unless T3_QUEUE=line forces this wheel.
constexpr double kLineKeysPerSlot = 0.35;
constexpr size_t kLineMaxSlots = 8192;
[[maybe_unused]] void line_wheel_geometry(const Scenario &s, unsigned &shift,
                                          size_t &slots) {
  double bulk = 1.0, tail = 1.0;
  latency_spread_ns(s, bulk, tail);
  double keys = kLineKeysPerSlot;
  if (const char *e = std::getenv("T3_LINE_KEYS"))
    keys = std::strtod(e, nullptr);
  const double width = keys * bulk / (0.8 * std::max(events_in_flight(s), 8.0));
  shift = static_cast<unsigned>(
      std::clamp<long>(std::lround(std::log2(std::max(width, 1.0))), 0, 40));
  const double want = tail / std::exp2(static_cast<double>(shift));
  slots = EventWheel::kMinSlots;
  while (slots < kLineMaxSlots && static_cast<double>(slots) < want)
    slots <<= 1;
  if (const char *e = std::getenv("T3_WHEEL_SHIFT"))
    shift = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
  if (const char *e = std::getenv("T3_WHEEL_SLOTS"))
    slots = std::strtoull(e, nullptr, 10);
}

// The geometry of the wheel of one slot per nanosecond (NsWheel): enough slots
// to reach the latency model's tail, at most its 4,096 (what is due later goes
// to the heap, as it did beyond the 4,096 slots of 1 ns that the one-line wheel
// had for these runs). T3_WHEEL_SLOTS overrides.
[[maybe_unused]] void ns_wheel_geometry(const Scenario &s, size_t &slots) {
  double bulk = 1.0, tail = 1.0;
  latency_spread_ns(s, bulk, tail);
  slots = NsWheel::kMinSlots;
  while (slots < NsWheel::kMaxSlots && static_cast<double>(slots) < tail)
    slots <<= 1;
  if (const char *e = std::getenv("T3_WHEEL_SLOTS"))
    slots = std::strtoull(e, nullptr, 10);
}

// Recurring agent wakeups, one FIFO lane per distinct wake interval. An agent
// has at most one recurring wakeup pending, it re-arms at now + interval while
// handling its own wakeup, now never decreases, and wakeups at equal times are
// handled in agent-id order (the (t, s, r) key order with s = r = agent). So
// each lane receives its keys in increasing key order and its front is its
// minimum: the event loop takes the smaller of the heap top and the smallest
// lane front, which is exactly the heap order, while the heap keeps only
// messages in flight (a few) instead of one entry per agent.
class WakeLanes {
public:
  static constexpr size_t kMaxLanes =
      8; // beyond this, the per-event front scan costs too much

  // interval[a] for every agent (0 = none: the exchange). Returns false (lanes
  // unused) when the agents use more than kMaxLanes distinct intervals.
  bool init(const std::vector<uint64_t> &interval) {
    std::vector<uint64_t> distinct;
    for (uint64_t v : interval)
      if (v && std::find(distinct.begin(), distinct.end(), v) == distinct.end())
        distinct.push_back(v);
    if (distinct.empty() || distinct.size() > kMaxLanes)
      return false;
    std::vector<size_t> members(distinct.size(), 0);
    lane_of_.assign(interval.size(), 0);
    for (size_t a = 0; a < interval.size(); ++a) {
      if (!interval[a])
        continue;
      const size_t l = static_cast<size_t>(
          std::find(distinct.begin(), distinct.end(), interval[a]) -
          distinct.begin());
      lane_of_[a] = static_cast<uint8_t>(l);
      ++members[l];
    }
    lanes_.resize(distinct.size());
    for (size_t l = 0; l < lanes_.size(); ++l) {
      size_t cap = 1;
      while (cap < members[l] + 1)
        cap <<= 1; // at most one pending wakeup per member
      lanes_[l].buf.assign(cap, 0);
      lanes_[l].mask = static_cast<uint32_t>(cap - 1);
    }
    return true;
  }

  bool any() const { return best_ >= 0; }
  u128 best_key() const { return best_key_; }

  void push(uint32_t agent, u128 k) {
    const uint8_t l = lane_of_[agent];
    Lane &L = lanes_[l];
    const bool was_empty = L.head == L.tail;
    L.buf[L.tail++ & L.mask] = k;
    if (was_empty && (best_ < 0 || k < best_key_)) {
      best_ = l;
      best_key_ = k;
    }
  }

  u128 pop_best() {
    Lane &L = lanes_[static_cast<size_t>(best_)];
    const u128 k = L.buf[L.head++ & L.mask];
    refresh();
    return k;
  }

private:
  struct Lane {
    std::vector<u128> buf;
    uint32_t mask = 0, head = 0, tail = 0;
  };
  void refresh() {
    best_ = -1;
    for (size_t l = 0; l < lanes_.size(); ++l) {
      const Lane &L = lanes_[l];
      if (L.head == L.tail)
        continue;
      const u128 f = L.buf[L.head & L.mask];
      if (best_ < 0 || f < best_key_) {
        best_ = static_cast<int>(l);
        best_key_ = f;
      }
    }
  }
  std::vector<Lane> lanes_;
  std::vector<uint8_t> lane_of_;
  int best_ = -1;
  u128 best_key_ = 0;
};

enum : uint8_t { kHasBid = 1, kHasAsk = 2, kClosed = 4 };

// ---------------------------------------------------------------------------------------------
// Oracle (SparseMeanRevertingOracle) with Python's int/float timestamp
// semantics.
// ---------------------------------------------------------------------------------------------
struct PyNum {
  bool is_float = false;
  int64_t i = 0;
  double f = 0.0;
  double as_double() const { return is_float ? f : static_cast<double>(i); }
};

inline bool py_le(int64_t a, const PyNum &b) { // a <= b, exact
  if (!b.is_float)
    return a <= b.i;
  return static_cast<long double>(a) <= static_cast<long double>(b.f);
}
inline bool py_lt(double a, int64_t b) { // a < b, exact
  return static_cast<long double>(a) < static_cast<long double>(b);
}
inline double py_sub(const PyNum &a, const PyNum &b) {
  if (!a.is_float && !b.is_float)
    return static_cast<double>(a.i - b.i);
  return a.as_double() - b.as_double();
}

class Oracle {
public:
  void init(const Scenario &s, LegacyRandom *master, uint32_t oracle_seed) {
    master_ = master;
    rng_.reseed(oracle_seed);
    r_bar_ = s.r_bar;
    kappa_ = s.kappa;
    fund_vol_ = s.fund_vol;
    lambda_ = s.megashock_lambda;
    ms_mean_ = s.megashock_mean;
    ms_sd_ = std::sqrt(s.megashock_var);
    close_ = static_cast<int64_t>(kOracleCloseNs);
    has_jump_ = s.has_scheduled_jump;
    jump_time_ = static_cast<int64_t>(s.mkt_open()) + s.jump_time_rel_ns;
    jump_mag_ = s.jump_magnitude;
    pt_.is_float = false;
    pt_.i = static_cast<int64_t>(s.mkt_open());
    pv_ = r_bar_;
    const double delta = master_->exponential(1.0 / lambda_);
    mst_ = static_cast<double>(static_cast<int64_t>(s.mkt_open())) + delta;
    msv_ = rng_.normal(ms_mean_, ms_sd_);
    if (rng_.randint(0, 2) != 0)
      msv_ = -msv_;
  }

  int64_t observe(int64_t t, LegacyRandom &agent_rng, double sigma_n) {
    const int64_t r_t = (t >= close_) ? advance(close_ - 1) : advance(t);
    if (sigma_n == 0.0)
      return r_t;
    const double obs =
        agent_rng.normal(static_cast<double>(r_t), std::sqrt(sigma_n));
    return static_cast<int64_t>(py_round(obs));
  }

private:
  int64_t compute(const PyNum &ts, double v_adj, bool add_adj) {
    const double d = py_sub(ts, pt_);
    const double mu = static_cast<double>(r_bar_);
    const double gamma = kappa_;
    const double theta = fund_vol_;
    const double loc =
        mu + static_cast<double>(pv_ - r_bar_) * std::exp(-gamma * d);
    const double scale = std::sqrt((std::pow(theta, 2.0) / (2 * gamma)) *
                                   (1 - std::exp(-2 * gamma * d)));
    double v = rng_.normal(loc, scale);
    if (add_adj)
      v += v_adj;
    int64_t iv = v > 0 ? static_cast<int64_t>(py_round(v)) : 0;
    if (has_jump_ && !jump_consumed_) {
      const bool reached = ts.is_float
                               ? static_cast<long double>(ts.f) >=
                                     static_cast<long double>(jump_time_)
                               : ts.i >= jump_time_;
      if (reached) {
        iv = std::max<int64_t>(0, iv + jump_mag_);
        jump_consumed_ = true;
      }
    }
    pt_ = ts;
    pv_ = iv;
    return iv;
  }

  int64_t advance(int64_t t) {
    if (py_le(t, pt_))
      return pv_;
    while (py_lt(mst_, t)) {
      PyNum ts;
      ts.is_float = true;
      ts.f = mst_;
      compute(ts, msv_, true);
      const double delta = master_->exponential(1.0 / lambda_);
      mst_ = mst_ + std::trunc(delta);
      msv_ = rng_.normal(ms_mean_, ms_sd_);
      if (rng_.randint(0, 2) != 0)
        msv_ = -msv_;
    }
    PyNum ts;
    ts.is_float = false;
    ts.i = t;
    return compute(ts, 0.0, false);
  }

  LegacyRandom *master_ = nullptr;
  LegacyRandom rng_;
  int64_t r_bar_ = 0;
  double kappa_ = 0, fund_vol_ = 0, lambda_ = 0, ms_mean_ = 0, ms_sd_ = 0;
  int64_t close_ = 0;
  bool has_jump_ = false, jump_consumed_ = false;
  int64_t jump_time_ = 0, jump_mag_ = 0;
  PyNum pt_;
  int64_t pv_ = 0;
  double mst_ = 0, msv_ = 0;
};

// ---------------------------------------------------------------------------------------------
// Agents
// ---------------------------------------------------------------------------------------------
// Per-agent state in three arrays, by how often a delivery reads it. Agent: the
// 16 bytes every delivery reads, so the whole population's flags fit in a few
// cache lines. AgentVecs: the headers of a market maker's open orders and a
// momentum trader's history, read by their fills, cancels and acts; one cache
// line each, so those deliveries touch one extra line near the others' rather
// than one 2.6 KB away. AgentCold: the spec and the 2.5 KB random state, read
// once per act.
struct Agent {
  uint64_t interval = 1;
  uint16_t noise_slot =
      0; // NoiseTrader: its stream in Sim::noise_ (agents number below 2^16)
  AgentType type = AgentType::Noise;
  bool first_wake = true;
  bool knows_hours = false;
  bool mkt_closed = false;
  bool awaiting_spread = false;
};
static_assert(sizeof(Agent) == 16);

struct alignas(64) AgentVecs {
  OpenOrders open; // MarketMaker's TradingAgent.orders (t3/openorders.hpp)
  std::vector<double> hist; // MomentumTrader
};
static_assert(sizeof(AgentVecs) == 64);

struct AgentCold {
  AgentSpec spec;
  LegacyRandom rng;
};

// ---------------------------------------------------------------------------------------------
// The simulation
// ---------------------------------------------------------------------------------------------
// The kernel (sim_kernel.inc) is compiled once per event queue, each in its own
// translation unit: this file with the heap alone, sim_wheel.cpp (which
// includes this file with T3_WHEEL_UNIT defined) with the timing wheel of large
// populations in front of the heap, sim_linewheel.cpp (T3_LINE_WHEEL_UNIT) with
// the one-line wheel, sim_linewheel_lean.cpp (T3_LINE_WHEEL_LEAN_UNIT) with
// that wheel and no ledger rows, and sim_nswheel.cpp and sim_nswheel_lean.cpp
// (T3_NS_WHEEL_UNIT, T3_NS_WHEEL_LEAN_UNIT) the same two with the wheel of one
// slot per nanosecond; queue_kind() and the run's options say which a run uses.
#if defined(T3_WHEEL_UNIT)
#define T3_SIM SimWheel
#define T3_QUEUE EventQueue
#define T3_WHEEL 1
#define T3_LEDGER 1
#elif defined(T3_LINE_WHEEL_UNIT)
#define T3_SIM SimLineWheel
#define T3_QUEUE EventWheel
#define T3_WHEEL 2
#define T3_LEDGER 1
#elif defined(T3_LINE_WHEEL_LEAN_UNIT)
#define T3_SIM SimLineWheelLean
#define T3_QUEUE EventWheel
#define T3_WHEEL 2
#define T3_LEDGER 0
#elif defined(T3_NS_WHEEL_UNIT)
#define T3_SIM SimNsWheel
#define T3_QUEUE NsWheel
#define T3_WHEEL 3
#define T3_LEDGER 1
#elif defined(T3_NS_WHEEL_LEAN_UNIT)
#define T3_SIM SimNsWheelLean
#define T3_QUEUE NsWheel
#define T3_WHEEL 3
#define T3_LEDGER 0
#else
#define T3_SIM SimHeap
#define T3_QUEUE EventHeap
#define T3_WHEEL 0
#define T3_LEDGER 1
#endif
#include "sim_kernel.inc"

} // namespace

#if defined(T3_WHEEL_UNIT)
SimResult simulate_wheel(const Scenario &s, const SimOptions &o) {
  Arena arena; // declared first: released after the Sim has joined its threads
  SimWheel sim(s, o);
  return sim.run(arena);
}
#elif defined(T3_LINE_WHEEL_UNIT)
SimResult simulate_line_wheel(const Scenario &s, const SimOptions &o) {
  Arena arena; // declared first: released after the Sim has joined its threads
  SimLineWheel sim(s, o);
  return sim.run(arena);
}
#elif defined(T3_LINE_WHEEL_LEAN_UNIT)
SimResult simulate_line_wheel_lean(const Scenario &s, const SimOptions &o) {
  Arena arena; // declared first: released after the Sim has joined its threads
  SimLineWheelLean sim(s, o);
  return sim.run(arena);
}
#elif defined(T3_NS_WHEEL_UNIT)
SimResult simulate_ns_wheel(const Scenario &s, const SimOptions &o) {
  Arena arena; // declared first: released after the Sim has joined its threads
  SimNsWheel sim(s, o);
  return sim.run(arena);
}
#elif defined(T3_NS_WHEEL_LEAN_UNIT)
SimResult simulate_ns_wheel_lean(const Scenario &s, const SimOptions &o) {
  Arena arena; // declared first: released after the Sim has joined its threads
  SimNsWheelLean sim(s, o);
  return sim.run(arena);
}
#else
SimResult simulate_wheel(const Scenario &s,
                         const SimOptions &o); // sim_wheel.cpp
SimResult simulate_line_wheel(const Scenario &s,
                              const SimOptions &o); // sim_linewheel.cpp
SimResult
simulate_line_wheel_lean(const Scenario &s,
                         const SimOptions &o); // sim_linewheel_lean.cpp
SimResult simulate_ns_wheel(const Scenario &s,
                            const SimOptions &o); // sim_nswheel.cpp
SimResult simulate_ns_wheel_lean(const Scenario &s,
                                 const SimOptions &o); // sim_nswheel_lean.cpp

SimResult simulate(const Scenario &s, const SimOptions &o) {
  // The ledger is not written (the unit's card): the kernel that fills no
  // ledger row. T3_NO_LEAN (A/B) keeps the other one, which then writes its
  // rows to a scratch line.
  const bool lean = o.ledger_skip && !std::getenv("T3_NO_LEAN");
  switch (queue_kind(s)) {
  case QueueKind::Wheel:
    return simulate_wheel(s, o);
  case QueueKind::LineWheel:
    return lean ? simulate_line_wheel_lean(s, o) : simulate_line_wheel(s, o);
  case QueueKind::NsWheel:
    return lean ? simulate_ns_wheel_lean(s, o) : simulate_ns_wheel(s, o);
  case QueueKind::Heap:
    break;
  }
  Arena arena; // declared first: released after the Sim has joined its threads
  SimHeap sim(s, o);
  return sim.run(arena);
}
#endif

} // namespace t3
