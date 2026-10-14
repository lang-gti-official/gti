#include "cthread_internal.h"
#include "gti/cthread.h"
#include <cstdlib>
#include <exception>
#include <string_view>
#include <thread>
using namespace gti::runtime::detail;
static int32_t manual(void *) {
  if (!enterCThreadCallback(manual))
    return 1;
  const auto owner = gti_rt_confined_callback_thread_v1();
  gti_rt_confined_callback_enter_v1(owner);
  gti_rt_confined_callback_leave_v1();
  return leaveCThreadCallback(manual) ? 0 : 2;
}
int main(int argc, char **argv) {
  std::set_terminate([] { std::_Exit(86); });
  const std::string_view mode = argc == 2 ? argv[1] : "normal";
  if (mode == "before-begin")
    gti_rt_confined_callback_thread_v1();
  gti_rt_cthread_begin_v1();
  const auto owner = gti_rt_confined_callback_thread_v1();
  if (mode == "zero")
    gti_rt_confined_callback_enter_v1(0);
  if (mode == "unbalanced")
    gti_rt_confined_callback_leave_v1();
  if (mode == "foreign") {
    std::thread t([] {
      // Possession of an identity is not runtime attachment permission.
      gti_rt_confined_callback_enter_v1(gti_rt_cthread_current_v1());
    });
    t.join();
  }
  if (mode == "scope-ended") {
    std::thread t([] {
      enterCThreadManagedScope();
      const auto id = gti_rt_confined_callback_thread_v1();
      leaveCThreadManagedScope();
      gti_rt_confined_callback_enter_v1(id);
    });
    t.join();
  }
  gti_rt_confined_callback_enter_v1(owner);
  gti_rt_confined_callback_enter_v1(owner);
  gti_rt_confined_callback_leave_v1();
  if (!terminalCThreadCallbackActive())
    return 3;
  gti_rt_confined_callback_leave_v1();
  if (terminalCThreadCallbackActive())
    return 4;
  uint64_t worker = 0;
  int32_t result = 9;
  if (gti_rt_cthread_create_v1(&worker, manual, nullptr) != 0 ||
      gti_rt_cthread_join_v1(worker, &result) != 0 || result != 0)
    return 5;
  gti_rt_cthread_end_v1();
  if (mode == "after-end")
    gti_rt_confined_callback_enter_v1(owner);
  return 0;
}
