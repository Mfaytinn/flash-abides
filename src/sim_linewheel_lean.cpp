// The kernel with the one-line-per-slot timing wheel and no ledger rows
// (sim_kernel.inc with T3_LEDGER = 0): sim.cpp compiled a fourth time, in its
// own translation unit. It defines simulate_line_wheel_lean(), which simulate()
// calls for a run on that wheel whose ledger is not written.
#define T3_LINE_WHEEL_LEAN_UNIT 1
#include "sim.cpp"
