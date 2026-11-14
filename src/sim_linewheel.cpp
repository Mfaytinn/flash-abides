// The kernel with the one-line-per-slot timing wheel in front of the event heap
// (t3/eventwheel.hpp): sim.cpp compiled a third time, in its own translation
// unit. It defines simulate_line_wheel(), which simulate() calls for the runs
// queue_kind() gives it.
#define T3_LINE_WHEEL_UNIT 1
#include "sim.cpp"
