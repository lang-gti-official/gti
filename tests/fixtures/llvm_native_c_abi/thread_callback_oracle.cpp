#include "gti/cthread.h"
#include <csignal>
#include <cstdlib>
#include <exception>
#include <string_view>
#include <thread>

using Transform = int32_t (*)(int32_t);
extern "C" {
Transform get_callback_confined();
gti_cthread_entry_v1 get_callback_worker();
gti_once_initializer_v1 get_callback_once();
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  std::set_terminate([] { std::_Exit(86); });
  std::signal(SIGABRT, [](int) { std::_Exit(87); });
  const std::string_view mode(argv[1]);
  if (mode == "convert-before-begin") {
    get_callback_confined();
    return 90;
  }
  gti_rt_cthread_begin_v1();
  auto confined = get_callback_confined();
  if (get_callback_confined() != confined || confined(7) != 7 ||
      confined(confined(11)) != 11)
    return 3;
  if (mode == "invoke-foreign" || mode == "convert-foreign") {
    std::thread foreign([=] {
      if (mode == "invoke-foreign")
        confined(1);
      else
        get_callback_confined();
    });
    foreign.join();
    return 91;
  }
  auto worker = get_callback_worker();
  auto once = get_callback_once();
  if (mode == "worker-on-initial") {
    worker(nullptr);
    return 92;
  }
  if (mode == "once-unregistered") {
    once();
    return 93;
  }
  gti_rt_once_invoke_v1(once);
  uint64_t handle = 0;
  int32_t result = 0;
  if (gti_rt_cthread_create_v1(&handle, worker, nullptr) != 0 ||
      gti_rt_cthread_join_v1(handle, &result) != 0 || result != 31)
    return 4;
  gti_rt_cthread_end_v1();
  if (mode == "invoke-after-end") {
    confined(1);
    return 94;
  }
  return mode == "success" ? 0 : 5;
}
