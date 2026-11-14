// Scenario configuration, parsed with the exact Python semantics of
// baselines/abides_fork/config.py (_agent_kwargs, build_config) and agents.py
// (__init__ coercions).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace t3 {

// Absolute epoch nanoseconds used by the baseline: 2021-02-05 00:00, 09:30 and
// 16:00.
inline constexpr uint64_t kDateNs = 1612483200000000000ull;
inline constexpr uint64_t kMarketOpenNs = kDateNs + 34200000000000ull;
inline constexpr uint64_t kOracleCloseNs = kDateNs + 57600000000000ull;
inline constexpr uint64_t kPostCloseNs = 1000000000ull;

enum class AgentType : uint8_t {
  Noise = 0,
  MarketMaker = 1,
  Value = 2,
  Momentum = 3
};
// Pairwise: ABIDES's default line-distance model, used when a scenario has no
// latency_config.
enum class LatencyModel : uint8_t {
  LogNormal,
  Uniform,
  Pareto,
  Constant,
  Pairwise
};

struct AgentSpec {
  AgentType type = AgentType::Noise;
  uint64_t interval_ns = 1;
  // NoiseTrader
  double order_size_mean = 0.0;
  double order_size_std = 0.0;
  int64_t price_offset_ticks = 0;
  int64_t reference_price = 0;
  // MarketMaker
  int64_t spread_ticks = 2;
  int64_t depth_levels = 1;
  int64_t size_per_level = 1;
  // Value / Momentum
  int64_t threshold_ticks = 2;
  int64_t lookback = 5;
  double sigma_n = 1000.0;
};

struct Scenario {
  std::string scenario_id;
  std::string family;
  int64_t seed = 0;
  uint64_t horizon_ns = 0;

  // Exchange
  bool protocol = false;
  bool stp_enabled = false;
  bool stp_cancel_oldest = false;
  int64_t ack_delay_ns = 0;
  int64_t compute_delay_ns = 0;

  // Oracle
  int64_t r_bar = 100000;
  double kappa = 0.0; // per ns
  double fund_vol = 0.0;
  double megashock_lambda = 0.0; // per ns
  double megashock_mean = 1000.0;
  double megashock_var = 50000.0;
  bool has_scheduled_jump = false;
  int64_t jump_time_rel_ns =
      0; // int(scheduled_jump.time_ns), relative to market open
  int64_t jump_magnitude = 0;

  // Latency
  LatencyModel latency_model = LatencyModel::Constant;
  double lat_mean_ns = 0.0, lat_sigma = 0.0, lat_min_ns = 0.0,
         lat_max_ns = 1e12, lat_alpha = 1.5, lat_mu = 0.0;

  std::vector<AgentSpec> agents; // expanded, in construction order (ids 1..N)

  uint64_t mkt_open() const { return kMarketOpenNs; }
  uint64_t mkt_close() const { return kMarketOpenNs + horizon_ns; }
  uint64_t stop_time() const { return mkt_close() + kPostCloseNs; }
};

// Throws std::runtime_error on anything the Python baseline would reject.
Scenario load_scenario(const std::string &path);
Scenario parse_scenario(const std::string &text);

} // namespace t3
