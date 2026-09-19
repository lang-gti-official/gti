#include "gti/cthread.h"

#include <cstdint>

namespace {
std::int32_t calls = 0;
}

// A separate boundary oracle. The normal and concurrent fixtures independently
// link the real runtime. This replacement proves native calls survive GTI
// optimization, without using timing or scheduler fairness as a test oracle.
extern "C" void gti_rt_cthread_yield_v1(void) { ++calls; }
extern "C" std::int32_t gti_test_yield_count() { return calls; }
