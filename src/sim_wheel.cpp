// The kernel with the timing wheel in front of the event heap
// (t3/eventqueue.hpp): sim.cpp compiled a second time, in its own translation
// unit. It defines simulate_wheel(), which simulate() calls for runs with many
// events in flight.
#define T3_WHEEL_UNIT 1
#include "sim.cpp"
