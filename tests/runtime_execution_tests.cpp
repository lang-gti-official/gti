#include "gti/runtime_execution.h"
#include "gti/runtime_thread.hpp"

#include <type_traits>
#include <vector>

static_assert(
    std::is_same_v<decltype(&gti_rt_thread_locals_begin_v1), void (*)(void)>);
static_assert(
    std::is_same_v<decltype(&gti_rt_thread_locals_drain_v2),
                   void (*)(const gti_failure_record_v2 *,
                            const gti_failure_artifact_descriptor_v1 *)>);

namespace {
std::vector<int> order;
bool cleanup(void *storage, gti_failure_record_v2 *,
             const gti_failure_record_v2 *,
             const gti_failure_artifact_descriptor_v1 *) {
  order.push_back(*static_cast<int *>(storage));
  return true;
}
} // namespace
int main() {
  if (gti_runtime_detail::thread_local_storage_lifetime_active())
    return 1;
  gti_rt_thread_locals_begin_v1();
  if (!gti_runtime_detail::thread_local_storage_lifetime_active())
    return 2;
  int first = 1, second = 2;
  gti_runtime_detail::thread_local_cleanup_entry a{.storage = &first,
                                                   .cleanup = cleanup};
  gti_runtime_detail::thread_local_cleanup_entry b{.storage = &second,
                                                   .cleanup = cleanup};
  gti_runtime_detail::register_thread_local_cleanup(&a);
  gti_runtime_detail::register_thread_local_cleanup(&b);
  gti_rt_thread_locals_drain_v2(nullptr, nullptr);
  gti_rt_thread_locals_drain_v2(nullptr, nullptr);
  return order == std::vector<int>({2, 1}) && !a.registered && !b.registered &&
                 !gti_runtime_detail::thread_local_storage_lifetime_active()
             ? 0
             : 3;
}
