// The kernel with the timing wheel of one slot per nanosecond and no ledger
// rows (sim_kernel.inc with T3_LEDGER = 0): sim.cpp compiled a sixth time, in
// its own translation unit. It defines simulate_ns_wheel_lean(), which
// simulate() calls for a run on that wheel whose ledger is not written.
#define T3_NS_WHEEL_LEAN_UNIT 1
#include "sim.cpp"
