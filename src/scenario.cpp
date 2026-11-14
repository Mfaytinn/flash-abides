#include "t3/scenario.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

#include "t3/json.hpp"

namespace t3 {
namespace {

using json::Value;

[[noreturn]] void bad(const std::string &what) {
  throw std::runtime_error("scenario: " + what);
}

// Python float(x).
double py_float(const Value &v, const char *name) {
  switch (v.kind) {
  case Value::Kind::Number:
    return v.num;
  case Value::Kind::Bool:
    return v.b ? 1.0 : 0.0;
  case Value::Kind::String: {
    char *end = nullptr;
    const double d = std::strtod(v.str.c_str(), &end);
    if (end == v.str.c_str())
      bad(std::string("float() of non-numeric ") + name);
    return d;
  }
  default:
    bad(std::string("float() of ") + name);
  }
}

// Python int(x): truncation toward zero for floats.
int64_t py_int(const Value &v, const char *name) {
  switch (v.kind) {
  case Value::Kind::Number:
    if (v.is_int)
      return v.i;
    if (!std::isfinite(v.num))
      bad(std::string("int() of non-finite ") + name);
    return static_cast<int64_t>(std::trunc(v.num));
  case Value::Kind::Bool:
    return v.b ? 1 : 0;
  case Value::Kind::String: {
    char *end = nullptr;
    const long long iv = std::strtoll(v.str.c_str(), &end, 10);
    if (end == v.str.c_str())
      bad(std::string("int() of non-numeric ") + name);
    return iv;
  }
  default:
    bad(std::string("int() of ") + name);
  }
}

// Python int(max(lo, x)): max compares numerically, int() truncates.
int64_t py_int_max(int64_t lo, const Value *v, int64_t dflt, const char *name) {
  if (!v)
    return std::max(lo, dflt);
  if (v->kind == Value::Kind::Number && !v->is_int) {
    const double x = v->num;
    if (static_cast<double>(lo) >= x)
      return lo;
    return static_cast<int64_t>(std::trunc(x));
  }
  return std::max(lo, py_int(*v, name));
}

double get_float(const Value &obj, const char *key, double dflt) {
  const Value *v = obj.get(key);
  return v ? py_float(*v, key) : dflt;
}

int64_t get_int(const Value &obj, const char *key, int64_t dflt) {
  const Value *v = obj.get(key);
  return v ? py_int(*v, key) : dflt;
}

const Value &req(const Value &obj, const char *key) {
  const Value *v = obj.get(key);
  if (!v)
    bad(std::string("missing key ") + key);
  return *v;
}

std::string py_str(const Value &v) {
  switch (v.kind) {
  case Value::Kind::String:
    return v.str;
  case Value::Kind::Number:
    if (v.is_int)
      return std::to_string(v.i);
    return std::to_string(v.num);
  case Value::Kind::Bool:
    return v.b ? "True" : "False";
  case Value::Kind::Null:
    return "None";
  default:
    return "";
  }
}

// Python's ints do not overflow; int64 does. An interval of 2^63 ns (292 years)
// or more cannot be scheduled by the Python kernel either (now + interval
// raises OverflowError), so such a scenario has no reference; saturating at
// INT64_MAX just keeps the engine defined. Below that the value is exact (now +
// interval stays within uint64).
constexpr int64_t kMaxIntervalNs = std::numeric_limits<int64_t>::max();

int64_t saturated_interval(double x,
                           const char *name) { // int(x) of a finite float
  if (!std::isfinite(x))
    bad(std::string("int() of non-finite ") + name); // Python raises too
  x = std::trunc(x);
  if (x >= static_cast<double>(kMaxIntervalNs))
    return kMaxIntervalNs;
  if (x <= 0)
    return 0;
  return static_cast<int64_t>(x);
}

uint64_t interval_ns(const Value &params) {
  int64_t iv;
  if (const Value *r = params.get("rebalance_interval_ns")) {
    // An integer literal beyond int64 arrives here as a double.
    iv = r->kind == Value::Kind::Number && !r->is_int
             ? saturated_interval(r->num, "rebalance_interval_ns")
             : py_int(*r, "rebalance_interval_ns");
  } else {
    const double hz = get_float(params, "arrival_rate_hz", 1.0);
    iv = hz > 0 ? saturated_interval(1e9 / hz, "arrival_rate_hz")
                : static_cast<int64_t>(1e9);
  }
  iv = std::min(iv, kMaxIntervalNs);
  return static_cast<uint64_t>(
      std::max<int64_t>(1, iv)); // ScheduledAgent: int(max(1, interval_ns))
}

} // namespace

Scenario parse_scenario(const std::string &text) {
  const Value root = json::parse(text);
  if (root.kind != Value::Kind::Object)
    bad("root is not an object");
  Scenario s;
  s.scenario_id = py_str(req(root, "scenario_id"));
  // The family by name, or as `family`: an integer 1-8 or the name.
  if (const Value *f = root.get("scenario_family"))
    s.family = py_str(*f);
  else if (const Value *g = root.get("family"))
    s.family = g->kind == Value::Kind::Number && g->is_int && g->i == 6
                   ? "throughput-scale"
                   : py_str(*g);
  s.seed = py_int(req(root, "seed"), "seed");
  if (s.seed < 0 || s.seed > 0xFFFFFFFFll)
    bad("seed must be between 0 and 2**32 - 1");
  const int64_t horizon = py_int(req(root, "horizon_ns"), "horizon_ns");
  if (horizon < 0)
    bad("negative horizon_ns");
  s.horizon_ns = static_cast<uint64_t>(horizon);

  // --- exchange (config.py: protocol fields honoured only under
  // protocol_enforcement)
  const Value &ex = req(root, "exchange_config");
  const Value *pe = ex.get("protocol_enforcement");
  s.protocol = pe && pe->truthy();
  if (s.protocol) {
    const Value *stp = ex.get("stp_policy");
    if (stp && stp->truthy()) {
      s.stp_enabled = true;
      s.stp_cancel_oldest = py_str(*stp) == "cancel_oldest";
    }
    s.ack_delay_ns = get_int(ex, "ack_delay_ns", 0);
    s.compute_delay_ns = get_int(ex, "compute_delay_ns", 0);
    if (s.ack_delay_ns < 0 || s.compute_delay_ns < 0)
      bad("negative protocol delay");
  }

  // --- oracle
  const Value &oc = req(root, "oracle_config");
  static const Value kEmpty = [] {
    Value v;
    v.kind = Value::Kind::Object;
    return v;
  }();
  const Value *op_ptr = oc.get("params");
  const Value &op = op_ptr ? *op_ptr : kEmpty;
  s.r_bar = get_int(op, "initial_price", 100000);
  {
    const double k = get_float(op, "kappa", 0.0);
    s.kappa = k > 0 ? k / 1e9 : 1.67e-16;
    const double j = get_float(op, "jump_intensity", 0.0);
    s.megashock_lambda = j > 0 ? j / 1e9 : 2.77778e-18;
    s.fund_vol = get_float(op, "sigma", 5e-5);
    const double js = get_float(op, "jump_sigma", 0.0);
    s.megashock_mean = js != 0.0 ? js : 1000.0; // float(...) or 1000.0
    s.megashock_var = 50000.0;
  }
  if (const Value *sj = op.get("scheduled_jump"); sj && sj->truthy()) {
    s.has_scheduled_jump = true;
    s.jump_time_rel_ns = py_int(req(*sj, "time_ns"), "scheduled_jump.time_ns");
    s.jump_magnitude =
        py_int(req(*sj, "magnitude"), "scheduled_jump.magnitude");
  }

  // --- latency (ScenarioLatencyModel)
  // Without one (absent, or falsy like {}), config.py falls back to ABIDES's
  // default model.
  const Value *lc = root.get("latency_config");
  if (!lc || !lc->truthy()) {
    s.latency_model = LatencyModel::Pairwise;
  } else {
    const Value *lp_ptr = lc->get("params");
    const Value &lp = lp_ptr ? *lp_ptr : kEmpty;
    const Value *model = lc->get("model");
    const std::string m = model ? py_str(*model) : "deterministic";
    if (m == "log_normal")
      s.latency_model = LatencyModel::LogNormal;
    else if (m == "uniform")
      s.latency_model = LatencyModel::Uniform;
    else if (m == "pareto")
      s.latency_model = LatencyModel::Pareto;
    else
      s.latency_model = LatencyModel::Constant;
    s.lat_mean_ns = get_float(lp, "mean_ns", 0.0);
    s.lat_sigma = get_float(lp, "sigma", 0.0);
    s.lat_min_ns = get_float(lp, "min_ns", 0.0);
    s.lat_max_ns = get_float(lp, "max_ns", 1e12);
    s.lat_alpha = get_float(lp, "alpha", 1.5);
    s.lat_mu = s.lat_mean_ns > 0 ? std::log(s.lat_mean_ns) : 0.0;
  }

  // --- agents, expanded in construction order
  const Value &acs = req(root, "agent_configs");
  if (acs.kind != Value::Kind::Array)
    bad("agent_configs is not a list");
  for (const Value &ac : acs.arr) {
    const std::string type = py_str(req(ac, "agent_type"));
    const Value *pp = ac.get("params");
    const Value &params = pp ? *pp : kEmpty;
    AgentSpec a;
    a.interval_ns = interval_ns(params);
    if (type == "NoiseTrader") {
      a.type = AgentType::Noise;
      a.order_size_mean = get_float(params, "order_size_mean", 10.0);
      a.order_size_std = get_float(params, "order_size_std", 2.0);
      a.price_offset_ticks = get_int(params, "price_offset_ticks", 5);
      a.reference_price = s.r_bar;
    } else if (type == "MarketMaker") {
      a.type = AgentType::MarketMaker;
      a.spread_ticks =
          py_int_max(2, params.get("spread_ticks"), 2, "spread_ticks");
      a.depth_levels =
          py_int_max(1, params.get("depth_levels"), 3, "depth_levels");
      a.size_per_level =
          py_int_max(1, params.get("size_per_level"), 10, "size_per_level");
      a.reference_price = s.r_bar;
    } else if (type == "ValueTrader") {
      a.type = AgentType::Value;
      a.order_size_mean = get_float(params, "order_size_mean", 25.0);
      a.threshold_ticks = get_int(params, "threshold_ticks", 2);
      a.sigma_n = 1000.0;
    } else if (type == "MomentumTrader") {
      a.type = AgentType::Momentum;
      a.order_size_mean = get_float(params, "order_size_mean", 15.0);
      a.threshold_ticks = get_int(params, "threshold_ticks", 2);
      a.lookback = py_int_max(1, params.get("lookback"), 5, "lookback");
    } else {
      bad("unsupported agent_type: " + type);
    }
    const int64_t count = py_int(req(ac, "count"), "count");
    // Checked before expanding, so a huge count fails here instead of
    // exhausting memory.
    if (count > 0 && s.agents.size() + static_cast<uint64_t>(count) >= 65535)
      bad("too many agents for the 16-bit key");
    for (int64_t k = 0; k < count; ++k)
      s.agents.push_back(a);
  }
  if (s.agents.size() >= 65535)
    bad("too many agents for the 16-bit key");
  return s;
}

Scenario load_scenario(const std::string &path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    throw std::runtime_error("cannot open scenario: " + path);
  std::string text;
  struct stat st{};
  if (::fstat(fd, &st) == 0 && st.st_size > 0)
    text.reserve(static_cast<size_t>(st.st_size));
  char buf[65536];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof buf);
    if (n < 0) {
      ::close(fd);
      throw std::runtime_error("cannot read scenario: " + path);
    }
    if (n == 0)
      break;
    text.append(buf, static_cast<size_t>(n));
  }
  ::close(fd);
  return parse_scenario(text);
}

} // namespace t3
