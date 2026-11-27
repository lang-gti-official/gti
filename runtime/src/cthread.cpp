#include "gti/cthread.h"

#if defined(_WIN32)
#include <thread>
#else
#include <sched.h>
#endif

extern "C" void gti_rt_cthread_yield_v1(void) {
#if defined(_WIN32)
  std::this_thread::yield();
#else
  sched_yield();
#endif
}
