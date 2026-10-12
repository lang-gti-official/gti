// Reuse the established process-allocation oracle, with a controllable native
// size limit to test overflow without scanning an impossibly large object.
#define gti_rt_native_size_max_v1 original_native_size_max
#include "stdlib_adoption_allocation_counts.cpp"
#undef gti_rt_native_size_max_v1

namespace {
std::uint64_t selected_limit = 0;
}
extern "C" std::uint64_t gti_rt_native_size_max_v1() {
  return selected_limit ? selected_limit : original_native_size_max();
}
extern "C" void foreign_size_limit(std::uint64_t limit) {
  selected_limit = limit;
}
