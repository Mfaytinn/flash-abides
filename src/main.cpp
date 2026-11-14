// Track-3 submission entrypoint: `simulate` and `simulate-batch` verbs.
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <malloc.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "t3/card.hpp"
#include "t3/output.hpp"
#include "t3/parallel.hpp"
#include "t3/scenario.hpp"
#include "t3/sim.hpp"

extern "C" {
extern const char
    __executable_start; // linker-provided bounds of the text segment
extern const char __etext;
// Present only in -fprofile-generate builds; _exit skips their exit-time
// profile dump. GCC's runtime has the first, a Clang-based compiler's
// the second.
void __gcov_dump(void) __attribute__((weak));
int __llvm_profile_write_file(void) __attribute__((weak));
}

namespace {

using t3::Scenario;

// Optional diagnostic for measuring the simulation window alone. Whole-process
// timing includes this work, so normal runs load code pages only as they are
// used.
void prefault_text() {
  if (!std::getenv("T3_TEXT_PREFAULT") || std::getenv("T3_NO_TEXT_PREFAULT"))
    return;
  const uintptr_t page = 4096;
  const uintptr_t lo =
      reinterpret_cast<uintptr_t>(&__executable_start) & ~(page - 1);
  const uintptr_t hi =
      (reinterpret_cast<uintptr_t>(&__etext) + page - 1) & ~(page - 1);
  if (hi <= lo)
    return;
#ifndef MADV_POPULATE_READ
#define MADV_POPULATE_READ 22 // Linux 5.14
#endif
  if (::madvise(reinterpret_cast<void *>(lo), hi - lo, MADV_POPULATE_READ) == 0)
    return;
  volatile char sink = 0;
  for (uintptr_t a = lo; a < hi; a += page)
    sink = sink + *reinterpret_cast<const volatile char *>(a);
}

void mkdirs(const std::string &dir) {
  if (dir.empty())
    return;
  // Common case first: the parent exists (one syscall; every syscall is costly
  // under gVisor).
  if (::mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST)
    return;
  std::string cur;
  for (size_t i = 0; i < dir.size(); ++i) {
    cur.push_back(dir[i]);
    if (dir[i] == '/' || i + 1 == dir.size()) {
      if (cur == "/")
        continue;
      if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST)
        throw std::runtime_error("cannot create directory " + cur);
    }
  }
}

std::string dirname_of(const std::string &path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos)
    return ".";
  if (slash == 0)
    return "/";
  return path.substr(0, slash);
}

std::string fmt_double(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  std::string s(buf);
  if (s.find_first_of(".eEn") == std::string::npos)
    s += ".0"; // keep it a JSON float
  return s;
}

std::string json_escape(const std::string &s) {
  std::string out;
  for (char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char b[8];
        std::snprintf(b, sizeof b, "\\u%04x", c);
        out += b;
      } else {
        out.push_back(c);
      }
    }
  }
  return out;
}

// Whether a single-scenario unit's ledger (message_trace.parquet) is written.
//
// The unit's card decides when the engine can read it (t3/card.hpp):
// requires_message_ledger = false leaves the ledger out, true writes it
// whatever its size. Without a readable card the engine writes the ledger,
// except on a throughput-scale single (qfbench2_track_simulation/ limits.py:
// the family's single-market units ship no ledger), where it is written unless
// it would break a limit that would reject the whole unit. The limit is 256 MiB
// per file and for the whole output folder, in Development and in the Final
// (organizers' README "Output folder rules"; it was 64 MiB until 2026-10-07),
// or a lower RLIMIT_FSIZE:
//  - file_cap: the ledger file itself, less a 1 MiB margin;
//  - tree_cap: trace + ledger together, less the same margin.
// Every repeat sees the same card and limits and writes the same bytes, so it
// makes the same decision. T3_LEDGER_CAP_BYTES overrides file_cap;
// T3_LEDGER_POLICY=keep|skip (tests and comparison scripts) always writes or
// never writes a ledger the card does not require.
struct LedgerLimits {
  uint64_t file_cap = 0, tree_cap = 0;
  bool skip = false;
};

LedgerLimits ledger_limits() {
  constexpr uint64_t kMiB = uint64_t{1} << 20, kOutputLimit = 256 * kMiB;
  auto less_margin = [&](uint64_t v) {
    return v > 2 * kMiB ? v - kMiB : v / 2;
  };
  LedgerLimits l;
  struct rlimit rl{};
  const uint64_t fsize =
      ::getrlimit(RLIMIT_FSIZE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY
          ? static_cast<uint64_t>(rl.rlim_cur)
          : 0;
  l.file_cap = l.tree_cap =
      less_margin(fsize ? std::min(fsize, kOutputLimit) : kOutputLimit);
  if (const char *c = std::getenv("T3_LEDGER_CAP_BYTES"))
    l.file_cap = std::strtoull(c, nullptr, 10);
  if (const char *p = std::getenv("T3_LEDGER_POLICY")) {
    if (std::strcmp(p, "keep") == 0)
      l.file_cap = l.tree_cap = 0;
    else if (std::strcmp(p, "skip") == 0)
      l.skip = true;
    else if (std::strcmp(p, "auto") != 0)
      throw std::runtime_error("T3_LEDGER_POLICY: keep, auto or skip");
  }
  return l;
}

// A ledger the card says is not required: left out, unless
// T3_LEDGER_POLICY=keep asks for it.
LedgerLimits ledger_not_required() {
  LedgerLimits l;
  const char *p = std::getenv("T3_LEDGER_POLICY");
  l.skip = !(p && std::strcmp(p, "keep") == 0);
  return l;
}

uint64_t file_bytes(const std::string &path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0;
}

// A run's memory, kept alive until the batch timer has stopped: freeing it
// inside the timed loop costs munmap calls whose TLB shootdowns interrupt the
// other workers.
struct Kept {
  t3::SimResult res;
  t3::Outputs out;
};

struct UnitResult {
  std::string scenario_id;
  int64_t seed = 0;
  size_t n_events = 0;
  size_t n_messages = 0;
  double sim_seconds = 0;
  std::string trace_sha256;
};

// Runs one scenario and writes trace.parquet + message_trace.parquet +
// events.json into out_dir. `batch` runs are timed end to end: they skip the
// pre-fault (it only moves page faults out of a single run's simulation window,
// and over-estimated capacity costs real time in a batch) and the latency
// producer thread (the workers already occupy the CPUs). `card` is the ledger
// flag of the unit's card; batch sub-scenarios always need their ledger.
UnitResult run_one(const Scenario &s, const std::string &trace_path,
                   unsigned out_threads, bool batch, Kept *keep = nullptr,
                   t3::LedgerFlag card = t3::LedgerFlag::Unknown) {
  const std::string out_dir = dirname_of(trace_path);
  mkdirs(out_dir);
  t3::SimOptions so;
  so.latency_thread =
      !batch && out_threads >= 2 && !std::getenv("T3_NO_LATENCY_THREAD");
  so.draw_thread = !batch && out_threads >= 3;
  // Large runs stream both files from a fourth thread during the window. With
  // fewer CPUs only very large ones do, which keeps their memory bounded
  // (simulate() decides).
  so.stream = !batch && !std::getenv("T3_NO_STREAM");
  so.threads = out_threads;
  so.trace_path = trace_path;
  so.ledger_path = out_dir + "/message_trace.parquet";
  so.prefault = !batch || std::getenv("T3_BATCH_PREFAULT");
  so.deferral = !std::getenv("T3_NO_DEFERRAL");
  LedgerLimits lim; // a required ledger: written, no limits
  if (!batch && card == t3::LedgerFlag::NotRequired)
    lim = ledger_not_required();
  else if (!batch && card == t3::LedgerFlag::Unknown &&
           s.family == "throughput-scale")
    lim = ledger_limits();
  so.ledger_cap = lim.file_cap;
  so.ledger_skip = lim.skip;
  if (lim.skip)
    ::unlink(
        so.ledger_path.c_str()); // no ledger from an earlier run in this folder
  const auto p0 = std::chrono::steady_clock::now();
  t3::SimResult res = t3::simulate(s, so);
  const auto p1 = std::chrono::steady_clock::now();
  // One pool for every output phase, created after the window (small runs stay
  // on this thread).
  std::unique_ptr<t3::SpinPool> pool;
  // A run that was not streamed holds every ledger row; they are encoded and
  // written here unless the ledger is skipped.
  const bool whole_ledger = !res.streamed && !lim.skip;
  if (!res.streamed && lim.skip)
    res.ledger_fate = "skipped";
  if (out_threads >= 2 &&
      res.trace.size() + (whole_ledger ? res.ledger.size() : 0) >= 200000)
    pool = std::make_unique<t3::SpinPool>(out_threads);
  t3::Outputs o = t3::build_outputs(res, pool.get(), whole_ledger);
  const auto p2 = std::chrono::steady_clock::now();
  UnitResult u;
  u.scenario_id = s.scenario_id;
  u.seed = s.seed;
  u.n_events = res.n_trace_rows;
  u.n_messages = res.n_deliveries;
  u.sim_seconds = res.sim_seconds;
  // Hash the trace while both files are written (tmpfs/page-cache writes are
  // memcpy-bound).
  auto io = [&](size_t k) {
    if (res.streamed)
      return;
    if (k == 0)
      u.trace_sha256 = t3::sha256_hex(o.trace);
    else if (k == 1) {
      if (whole_ledger)
        t3::write_file(so.ledger_path, o.ledger);
    } else
      t3::write_file(trace_path, o.trace);
  };
  if (res.streamed)
    u.trace_sha256 = res.trace_sha256;
  else if (pool)
    pool->run(3, io);
  else
    for (size_t k = 0; k < 3; ++k)
      io(k);
  // Whole-tree limit (see LedgerLimits); events.json is well under 4 KiB.
  if (lim.tree_cap && std::strcmp(res.ledger_fate, "written") == 0 &&
      file_bytes(trace_path) + file_bytes(so.ledger_path) + 4096 >
          lim.tree_cap) {
    ::unlink(so.ledger_path.c_str());
    res.ledger_fate = "dropped";
  }

  const double wall = u.sim_seconds > 0 ? u.sim_seconds : 1e-9;
  std::string ev = "{\n";
  ev += "  \"scenario_id\": \"" + json_escape(u.scenario_id) + "\",\n";
  ev += "  \"seed\": " + std::to_string(u.seed) + ",\n";
  ev += "  \"n_events\": " + std::to_string(u.n_events) + ",\n";
  ev += "  \"wall_clock_sec\": " + fmt_double(wall) + ",\n";
  ev += "  \"events_per_sec\": " +
        fmt_double(static_cast<double>(u.n_events) / wall) + ",\n";
  ev += "  \"trace_sha256\": \"" + u.trace_sha256 + "\",\n";
  ev += "  \"n_messages\": " + std::to_string(u.n_messages) + ",\n";
  ev += "  \"peak_memory_bytes\": " + std::to_string(t3::peak_rss_bytes()) +
        ",\n";
  ev += "  \"gpu_seconds\": 0.0\n}\n";
  t3::write_file(out_dir + "/events.json", ev);
  if (std::getenv("T3_PHASES")) {
    const auto p3 = std::chrono::steady_clock::now();
    auto ms = [](auto a, auto b) {
      return std::chrono::duration<double, std::milli>(b - a).count();
    };
    std::fprintf(
        stderr,
        "phases: setup %.2f ms, window %.2f ms, simulate() total %.2f ms, "
        "outputs %.2f ms, "
        "writes %.2f ms | rows %zu (est %zu), deliveries %zu (est %zu), "
        "trace %zu B, ledger %zu B%s, rss %zu MB, ledger file %s\n",
        res.setup_seconds * 1e3, res.sim_seconds * 1e3, ms(p0, p1), ms(p1, p2),
        ms(p2, p3), u.n_events, res.est_rows, u.n_messages, res.est_msgs,
        o.trace.size, o.ledger.size, res.streamed ? " (streamed)" : "",
        static_cast<size_t>(t3::peak_rss_bytes()) >> 20, res.ledger_fate);
  }
  if (keep) {
    keep->res = std::move(res);
    keep->out = std::move(o);
  }
  return u;
}

int cmd_simulate(int argc, char **argv) {
  std::string config, out;
  bool has_seed = false;
  long long seed = 0;
  for (int i = 0; i < argc; ++i) {
    std::string a = argv[i];
    auto val = [&](const char *name) -> std::string {
      const std::string pre = std::string(name) + "=";
      if (a.rfind(pre, 0) == 0)
        return a.substr(pre.size());
      if (i + 1 >= argc)
        throw std::runtime_error(std::string("missing value for ") + name);
      return argv[++i];
    };
    if (a == "--config" || a.rfind("--config=", 0) == 0)
      config = val("--config");
    else if (a == "--out" || a.rfind("--out=", 0) == 0)
      out = val("--out");
    else if (a == "--seed" || a.rfind("--seed=", 0) == 0) {
      has_seed = true;
      seed = std::stoll(val("--seed"));
    } else
      throw std::runtime_error("unknown argument: " + a);
  }
  if (config.empty() || out.empty()) {
    std::fprintf(stderr, "usage: simulate --config /input/scenario.json --out "
                         "/output/trace.parquet\n");
    return 2;
  }
  Scenario s = t3::load_scenario(config);
  if (has_seed)
    s.seed = seed;
  const unsigned budget = t3::cpu_budget();
  // Fixed malloc thresholds: blocks up to 32 MiB come from the heap and freed
  // memory stays mapped, so the ledger encoder's buffers cause no mmap/munmap
  // (which under gVisor stall the event loop) during the window.
  if (!std::getenv("T3_NO_MALLOPT")) {
    ::mallopt(M_MMAP_THRESHOLD, 32 << 20);
    ::mallopt(M_TRIM_THRESHOLD, 1 << 30);
  }
  const t3::LedgerFlag card = t3::read_card_ledger_flag(config);
  // Released by process exit (main ends with _exit), not by destructors.
  const UnitResult u = run_one(s, out, budget, false, new Kept, card);
  std::printf("{\"scenario_id\": \"%s\", \"n_events\": %zu, "
              "\"wall_clock_sec\": %.9f, \"events_per_sec\": %.1f}\n",
              json_escape(u.scenario_id).c_str(), u.n_events, u.sim_seconds,
              static_cast<double>(u.n_events) /
                  (u.sim_seconds > 0 ? u.sim_seconds : 1e-9));
  return 0;
}

// Developer mode: `simulate bench --config X [--repeat N]` runs only the
// simulation N times and prints each kernel-window rate (no output files). Used
// for profiling and low-noise A/B runs.
int cmd_bench(int argc, char **argv) {
  std::string config;
  int repeat = 5;
  for (int i = 0; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--config" && i + 1 < argc)
      config = argv[++i];
    else if (a == "--repeat" && i + 1 < argc)
      repeat = std::atoi(argv[++i]);
  }
  const Scenario s = t3::load_scenario(config);
  double best = 0;
  size_t rows = 0;
  for (int r = 0; r < repeat; ++r) {
    t3::SimOptions so;
    so.latency_thread = !std::getenv("T3_NO_LATENCY_THREAD");
    so.draw_thread = t3::cpu_budget() >= 3;
    so.deferral = !std::getenv("T3_NO_DEFERRAL");
    t3::SimResult res = t3::simulate(s, so);
    rows = res.trace.size();
    const double eps = static_cast<double>(rows) / res.sim_seconds;
    best = std::max(best, eps);
    std::printf("run %d: %.6f s  %.3f M/s\n", r, res.sim_seconds, eps / 1e6);
  }
  std::printf("best %.3f M/s  rows %zu\n", best / 1e6, rows);
  return 0;
}

int cmd_simulate_batch(int argc, char **argv) {
  std::string batch_dir, out_dir;
  for (int i = 0; i < argc; ++i) {
    std::string a = argv[i];
    auto val = [&](const char *name) -> std::string {
      const std::string pre = std::string(name) + "=";
      if (a.rfind(pre, 0) == 0)
        return a.substr(pre.size());
      if (i + 1 >= argc)
        throw std::runtime_error(std::string("missing value for ") + name);
      return argv[++i];
    };
    if (a == "--batch-dir" || a.rfind("--batch-dir=", 0) == 0)
      batch_dir = val("--batch-dir");
    else if (a == "--out-dir" || a.rfind("--out-dir=", 0) == 0)
      out_dir = val("--out-dir");
    else
      throw std::runtime_error("unknown argument: " + a);
  }
  if (batch_dir.empty() || out_dir.empty()) {
    std::fprintf(stderr, "usage: simulate-batch --batch-dir /input/scenarios "
                         "--out-dir /output\n");
    return 2;
  }
  std::vector<std::string> files;
  if (DIR *d = ::opendir(batch_dir.c_str())) {
    while (dirent *e = ::readdir(d)) {
      std::string name = e->d_name;
      if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0) {
        struct stat st{};
        if (::stat((batch_dir + "/" + name).c_str(), &st) == 0 &&
            S_ISREG(st.st_mode))
          files.push_back(name);
      }
    }
    ::closedir(d);
  } else {
    throw std::runtime_error("batch directory not found: " + batch_dir);
  }
  std::sort(files.begin(), files.end());
  if (files.empty())
    throw std::runtime_error("no sub-scenarios (*.json) found in " + batch_dir);

  const size_t n = files.size();
  const unsigned budget = t3::cpu_budget();
  // Batch workers reuse their populated malloc heaps. T3_BATCH_MMAP=1
  // switches back to mmap.
  const bool heap = !std::getenv("T3_BATCH_MMAP");
  if (heap) {
    t3::bigarray_heap_mode() = true;
    ::mallopt(M_MMAP_THRESHOLD,
              32 << 20); // blocks up to 32 MiB come from the heap
    ::mallopt(M_TRIM_THRESHOLD,
              1 << 30); // and freed memory stays mapped for the next sub
  }
  // Workers start before the timer (as the baseline's imports and file listing
  // do) and spin, so the timed loop pays neither thread creation nor idle-CPU
  // wake-up.
  t3::SpinPool pool(static_cast<unsigned>(std::min<size_t>(budget, n)));
  pool.set_hot(
      true); // no worker is asleep when the timer starts, or between its phases
  if (heap) {
    // Whole-process timing includes warm-up too. Do not touch an unconditional
    // 4 MiB per worker before doing useful work; malloc supplies the actual
    // working set as needed. T3_PREWARM_MB opts into heap warming for
    // narrow-window diagnostics (4 restores the old policy).
    static const size_t warm =
        (std::getenv("T3_PREWARM_MB")
             ? std::strtoull(std::getenv("T3_PREWARM_MB"), nullptr, 10)
             : 0)
        << 20;
    pool.run_on_all([](size_t) { t3::prewarm_heap(warm); });
  }

  struct rusage ru0{};
  ::getrusage(RUSAGE_SELF, &ru0);
  // Timer covers the whole batch loop (parsing, simulation and writes), like
  // the baseline's.
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<Scenario> scen(n);
  std::vector<double> cost(n, 0.0);
  pool.run(n, [&](size_t k) {
    scen[k] = t3::load_scenario(batch_dir + "/" + files[k]);
    for (const auto &a : scen[k].agents)
      cost[k] += static_cast<double>(scen[k].horizon_ns) /
                 static_cast<double>(a.interval_ns) *
                 (a.type == t3::AgentType::MarketMaker
                      ? 8.0 * static_cast<double>(a.depth_levels)
                      : 7.0);
  });
  const auto t_parsed = std::chrono::steady_clock::now();
  std::vector<size_t> order(n);
  for (size_t k = 0; k < n; ++k)
    order[k] = k;
  std::stable_sort(order.begin(), order.end(),
                   [&](size_t a, size_t b) { return cost[a] > cost[b]; });
  std::vector<std::pair<std::string, UnitResult>> results(n);
  std::vector<Kept> &kept =
      *new std::vector<Kept>(n); // released by process exit
  const bool dbg = std::getenv("T3_PHASES") != nullptr;
  std::vector<std::pair<double, double>> span(n);
  auto since = [&](std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(t - t0).count();
  };
  // This is the last phase: finished workers leave CPU time for the remaining
  // markets.
  pool.run_final(n, [&](size_t k) {
    const size_t idx = order[k];
    const auto a = std::chrono::steady_clock::now();
    const std::string sub = files[idx].substr(0, files[idx].size() - 5);
    // Heap mode frees each sub's memory as soon as its files are written, so
    // the next sub on this worker reuses pages that are already mapped.
    results[idx] = {sub,
                    run_one(scen[idx], out_dir + "/" + sub + "/trace.parquet",
                            n >= budget ? 1 : budget, true,
                            heap ? nullptr : &kept[idx])};
    span[idx] = {since(a), since(std::chrono::steady_clock::now())};
  });
  if (dbg) {
    struct rusage ru1{};
    ::getrusage(RUSAGE_SELF, &ru1);
    std::fprintf(stderr,
                 "batch: minor faults in the timed loop %ld (major %ld)\n",
                 ru1.ru_minflt - ru0.ru_minflt, ru1.ru_majflt - ru0.ru_majflt);
    std::fprintf(stderr, "batch: parse %.3f ms\n", since(t_parsed));
    for (size_t k = 0; k < n; ++k)
      std::fprintf(stderr, "batch: %-16s %.3f -> %.3f ms\n", files[k].c_str(),
                   span[k].first, span[k].second);
  }
  size_t total = 0;
  for (const auto &r : results)
    total += r.second.n_events;
  const double wall = std::max(
      1e-9, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
                .count());

  std::string bj = "{\n";
  bj += "  \"n_scenarios\": " + std::to_string(results.size()) + ",\n";
  bj += "  \"total_events\": " + std::to_string(total) + ",\n";
  bj += "  \"wall_clock_sec\": " + fmt_double(wall) + ",\n";
  bj +=
      "  \"events_per_sec\": " + fmt_double(static_cast<double>(total) / wall) +
      ",\n";
  bj += "  \"peak_memory_bytes\": " + std::to_string(t3::peak_rss_bytes()) +
        ",\n";
  bj += "  \"gpu_seconds\": 0.0,\n";
  bj += "  \"per_scenario\": [";
  for (size_t k = 0; k < results.size(); ++k) {
    bj += k ? ",\n    " : "\n    ";
    bj += "{\"sub\": \"" + json_escape(results[k].first) +
          "\", \"n_events\": " + std::to_string(results[k].second.n_events) +
          ", \"trace_sha256\": \"" + results[k].second.trace_sha256 + "\"}";
  }
  bj += "\n  ]\n}\n";
  mkdirs(out_dir);
  t3::write_file(out_dir + "/batch_events.json", bj);
  std::printf("{\"n_scenarios\": %zu, \"total_events\": %zu, "
              "\"wall_clock_sec\": %.9f, \"events_per_sec\": %.1f}\n",
              results.size(), total, wall, static_cast<double>(total) / wall);
  return 0;
}

} // namespace

// The wall clock a container's StartedAt and FinishedAt are read from, in ns
// since the epoch.
static long long wall_clock_ns() {
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

int main(int argc, char **argv) {
  const auto process_start = std::chrono::steady_clock::now();
  const long long wall_start = wall_clock_ns();
  // A write past a file-size limit (RLIMIT_FSIZE) then fails with EFBIG, which
  // the writers report, instead of the signal killing the process.
  std::signal(SIGXFSZ, SIG_IGN);
  prefault_text();
  try {
    std::string verb;
    int first = 1;
    const char *base = std::strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    if (argc > 1 && (std::strcmp(argv[1], "simulate") == 0 ||
                     std::strcmp(argv[1], "simulate-batch") == 0 ||
                     std::strcmp(argv[1], "bench") == 0)) {
      verb = argv[1];
      first = 2;
      if (verb == "simulate" && argc > 2 &&
          std::strcmp(argv[2], "bench") == 0) {
        verb = "bench";
        first = 3;
      }
    } else {
      verb = std::strcmp(base, "simulate-batch") == 0 ? "simulate-batch"
                                                      : "simulate";
    }
    int rc;
    if (verb == "simulate-batch")
      rc = cmd_simulate_batch(argc - first, argv + first);
    else if (verb == "bench")
      rc = cmd_bench(argc - first, argv + first);
    else
      rc = cmd_simulate(argc - first, argv + first);
    // Every output file is closed by now; skip destructors and let the kernel
    // reclaim memory.
    if (std::getenv("T3_PHASES")) {
      rusage ru{};
      getrusage(RUSAGE_SELF, &ru);
      std::fprintf(stderr,
                   "process: main() to _exit %.2f ms, peak rss %lld MB, %ld "
                   "minor page faults\n",
                   std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - process_start)
                       .count(),
                   t3::peak_rss_bytes() >> 20, ru.ru_minflt);
      // Where the run sits inside its container's window: compare with docker
      // inspect's State.StartedAt and State.FinishedAt.
      std::fprintf(
          stderr, "clock: main() at %lld, _exit at %lld (ns since the epoch)\n",
          wall_start, wall_clock_ns());
    }
    std::fflush(stdout);
    std::fflush(stderr);
    if (__gcov_dump)
      __gcov_dump();
    if (__llvm_profile_write_file)
      (void)__llvm_profile_write_file();
    ::_exit(rc);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
