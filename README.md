# Flash-ABIDES: Accelerated Market Simulation for AI Agents

Flash-ABIDES is a fast C++ engine for ABIDES-style limit order book markets. Given the same
scenario and seed, it produces **exactly the same market** as the Agenthon 2026 reference
ABIDES (the organizers' pinned Python fork), down to every order, fill, timestamp and
message, and it runs much faster. That makes it practical to run the large numbers of
market simulations that AI agents need for training, evaluation and testing.

Flash-ABIDES won **1st place in Track 3 (Simulation) of Agenthon 2026**, a benchmark that
ranked simulators by speed and accepted only output identical to the reference ABIDES
market.

## What it simulates

A single-asset market with an exchange that matches limit orders, a mean-reverting
fundamental value with optional jumps, and a configurable population of noise traders,
market makers, value traders and momentum traders. Messages between agents and the
exchange are delayed by a latency model: lognormal, uniform, Pareto, constant, or
ABIDES's default distance-based model. A scenario is a JSON file that sets the agents,
market parameters, latency model and seed.

## What it outputs

Each run writes three files to the output directory.

**`trace.parquet`: the market's event log.** One row for every order event, in time
order.

| Column | Meaning |
|---|---|
| `t_ns` | Time in nanoseconds |
| `agent_id` | Agent the event belongs to |
| `msg_type` | `ORDER_SUBMITTED`, `ORDER_ACCEPTED`, `ORDER_FILLED`, `PARTIAL_FILL`, `ORDER_CANCELLED`, `ORDER_REPLACED` or `QUOTE_UPDATE` |
| `side` | `BID` or `ASK` |
| `price`, `size` | Price in integer ticks and quantity (0 where not applicable) |
| `order_id` | Order the event refers to |

**`message_trace.parquet`: the message ledger.** One row for every message delivered
between agents and the exchange: `seq`, `t_send_ns`, `t_recv_ns`, `latency_ns`, `src_id`,
`dst_id`, `message_id`, `msg_type`, `order_id`, and `causal_parent`, the message that
caused this one. You can follow any trade back through the chain of messages that led
to it.

**`events.json`: the run summary.** Scenario id, seed, number of events and messages, wall
time, events per second, peak memory, and a SHA-256 of `trace.parquet`, so you can check
that two runs produced the same trace by comparing one line.

Both Parquet files can be read directly with pandas, Polars, Arrow or DuckDB.

## Quick start

Requires Linux on an x86-64 CPU with AVX2 (x86-64-v3), CMake 3.20+ and a C++20 compiler
(tested with GCC 13 and CMake 3.28 on Ubuntu 24.04).

```bash
make
build/simulate simulate --config examples/scenario.json --out out/trace.parquet
```

The example is a 20-second market with 20 agents and produces 24,695 events.
`build/simulate simulate-batch --batch-dir scenarios/ --out-dir out/` runs every scenario
in a directory and writes one output directory per scenario. The `Dockerfile` packages the static binary alone:
`docker build -f Dockerfile -t flash-abides build`.

## How we built it

We rewrote the reference ABIDES in C++, reproducing its logic and its numpy random number
draws, and compared the output with the Python reference until every row matched. We then
optimized the engine and re-checked the output after each change. In the code, the main
gains came from a timing-wheel event queue, a cache-friendly order book, latency draws
computed ahead on a helper thread with AVX2 vector math, our own Parquet writer that
streams output during the run, and huge-page memory. On the build side, the submitted
binary is compiled with GCC 16 at `-O3` for x86-64-v3, with link-time optimization (LTO)
and profile-guided optimization (PGO) from profiles recorded on representative scenarios,
and linked statically.

## The Agenthon 2026 Final submission

- Image: `docker.io/mfaytinn/track3-sim@sha256:77edb663ca0590f927ebb42471731769a75a646b91192e5cb814c6a9d5b1c889`
- Binary `/simulate`, SHA-256: `a0ffe98228ae20d8915843516ca970a3fb39287ccaa83fd83f3613be96351369`

The source here is that code, reformatted, with unused diagnostics removed. Built with the
original compiler and profiles (below), it gives byte-identical output files to the
submitted binary (checked on all 71 public units and 26 larger scenarios) at the same
speed. Its machine code and data are identical to the submitted binary; only the debug
information differs, because reformatting moved line numbers.

**Rebuild with the original compiler and profiles.** This needs an x86-64 Linux host with
Docker (or Podman with `CONTAINER=podman`). First get the two toolchain archives,
`submission-final-toolchain.tar.gz` and `submission-final-host-inputs.tar.gz`, into
`dist/final-reproduction/`: download them from the GitHub release, or recreate them
byte for byte from the official Ubuntu packages:

```bash
reproduction/make-archives.sh
```

The archives contain unmodified Ubuntu packages, listed with their sources in
[`reproduction/SOURCES.md`](reproduction/SOURCES.md). Then build:

```bash
make final-binary
```

Compilation runs offline. Output: `build/cross-final/build/simulate`.

**Rebuild the exact submitted bytes.** `reproduction/exact/` keeps the submitted source in
its original line layout. Built with the same archives and profiles, it reproduces the
binary hash above, and the build checks it:

```bash
make exact-binary
```

Output: `build/exact/simulate`.

## Citation

If you use Flash-ABIDES, please cite:

```bibtex
@misc{flashabides2026,
  title  = {Flash-ABIDES: Accelerated Market Simulation for AI Agents},
  author = {Dervi{\c{s}}o{\u{g}}lu, G{\"o}kt{\"u}rk Bat{\i}n and Aytin, Muhammed Faruk and Birben, Muhammed R{\"u}{\c{s}}en},
  year   = {2026},
  note   = {1st place, Agenthon 2026 Track 3 (Simulation)},
  url    = {https://github.com/Mfaytinn/flash-abides}
}
```

## License

MIT (`LICENSE`). Flash-ABIDES reimplements the behavior of
[ABIDES](https://github.com/jpmorganchase/abides-jpmc-public) (BSD-3-Clause); see
`THIRD-PARTY-NOTICES.md`.
