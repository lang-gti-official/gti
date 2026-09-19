#include "gti/cthread.h"

#include <thread>

extern "C" void gti_rt_cthread_yield_v1(void) { std::this_thread::yield(); }
