// Output stage: canonical trace ordering and fill classification, Parquet
// encoding of both deliverables, SHA-256 and the events.json sidecar. Runs
// after the simulation window.
#pragma once

#include <string>
#include <vector>

#include "t3/parquet.hpp"
#include "t3/sim.hpp"

namespace t3 {

struct Outputs {
  pq::File trace;  // trace.parquet
  pq::File ledger; // message_trace.parquet
};

class SpinPool;

// Canonicalizes the trace in place (the last execution of each order becomes
// ORDER_FILLED; rows take the stable (t_ns, order_id) order of
// baselines/abides_fork/trace.py), then encodes both deliverables on `pool`
// (null: on the calling thread). The bytes do not depend on the threads.
// `with_ledger` false leaves Outputs::ledger empty (a unit whose card does not
// require it).
Outputs build_outputs(SimResult &res, SpinPool *pool, bool with_ledger = true);

// SHA-256 (hex) of a laid-out file's bytes.
std::string sha256_hex(const pq::File &f);

// Writes a file with writev (no fsync). Throws on failure.
void write_file(const std::string &path, const pq::File &f);
void write_file(const std::string &path, const std::string &bytes);

// Peak resident set size of this process in bytes.
long long peak_rss_bytes();

} // namespace t3
