// The kernel with the timing wheel of one slot per nanosecond in front of the
// event heap (t3/nswheel.hpp): sim.cpp compiled a fifth time, in its own
// translation unit. It defines simulate_ns_wheel(), which simulate() calls for
// the crowded runs queue_kind() gives it.
#define T3_NS_WHEEL_UNIT 1
#include "sim.cpp"
