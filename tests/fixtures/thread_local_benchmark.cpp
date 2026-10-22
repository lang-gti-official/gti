// C++ successful-path comparison with the same checked additions and opaque
// native calls as GTI. Dynamic storage uses GTI's explicit first-use/recursive
// initialization/shutdown and exit registry, not C++'s different TLS destructor
// or recursive-initialization contract. Diagnostic source tables differ.
#include "gti/runtime_thread.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

extern "C" {
std::uint64_t gti_tls_bench_clock();
std::uint64_t gti_tls_bench_iterations();
std::uint64_t gti_tls_bench_step(std::uint64_t);
void *gti_tls_bench_acquire();
void gti_tls_bench_release(void *);
void gti_tls_bench_report(std::uint64_t, std::uint64_t, std::uint64_t);
}
namespace {
[[noreturn]] void fail(std::uint16_t code, std::uint16_t detail) {
  gti_failure_record_v2 record{};
  record.abi_version = GTI_FAILURE_ABI_VERSION_V2;
  record.code = code;
  record.detail = detail;
  gti_rt_failure_terminate_v2(&record, nullptr, nullptr, nullptr);
}
std::uint64_t checkedAdd(std::uint64_t left, std::uint64_t right) {
  std::uint64_t result;
  if (__builtin_add_overflow(left, right, &result))
    fail(GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1, GTI_FAILURE_DETAIL_ADDITION_V1);
  return result;
}
std::uint64_t checkedSubtract(std::uint64_t left, std::uint64_t right) {
  std::uint64_t result;
  if (__builtin_sub_overflow(left, right, &result))
    fail(GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1,
         GTI_FAILURE_DETAIL_SUBTRACTION_V1);
  return result;
}
#ifdef GTI_TLS_BENCH_DYNAMIC
struct Resource {
  void *handle = gti_tls_bench_acquire();
  std::uint64_t value = 0;
  ~Resource() { gti_tls_bench_release(handle); }
};
struct Slot {
  alignas(Resource) std::byte bytes[sizeof(Resource)];
  unsigned state = 0;
  gti_runtime_detail::thread_local_cleanup_entry entry{};
  Resource *address() {
    return std::launder(reinterpret_cast<Resource *>(bytes));
  }
};
constinit thread_local Slot slot{};
bool cleanup(void *raw, gti_failure_record_v2 *, const gti_failure_record_v2 *,
             const gti_failure_artifact_descriptor_v1 *) {
  auto &value = *static_cast<Slot *>(raw);
  value.state = 3;
  std::destroy_at(value.address());
  return true;
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((__cold__, __noinline__))
#elif defined(_MSC_VER)
__declspec(noinline)
#endif
void ensureCold() {
  if (slot.state == 1 || slot.state == 3 ||
      !gti_runtime_detail::thread_local_storage_lifetime_active() ||
      gti_runtime_detail::thread_local_cleanup_active())
    fail(GTI_FAILURE_CODE_HOSTED_RUNTIME_CONTRACT_FAILURE_V1,
         slot.state == 1
             ? GTI_FAILURE_DETAIL_RECURSIVE_THREAD_LOCAL_INITIALIZATION_V1
         : gti_runtime_detail::thread_local_cleanup_active()
             ? GTI_FAILURE_DETAIL_THREAD_LOCAL_INITIALIZATION_DURING_SHUTDOWN_V2
             : GTI_FAILURE_DETAIL_THREAD_LOCAL_OUTSIDE_ATTACHED_LIFETIME_V2);
  slot.state = 1;
  std::construct_at(reinterpret_cast<Resource *>(slot.bytes));
  slot.state = 2;
  slot.entry.storage = &slot;
  slot.entry.cleanup = &cleanup;
  gti_runtime_detail::register_thread_local_cleanup(&slot.entry);
}
inline void ensure() {
  if (slot.state != 2) [[unlikely]]
    ensureCold();
}
void add(std::uint64_t value) {
  ensure();
  slot.address()->value = checkedAdd(slot.address()->value, value);
}
std::uint64_t read() {
  ensure();
  return slot.address()->value;
}
#else
constinit thread_local std::uint64_t counter = 0;
void add(std::uint64_t value) { counter = checkedAdd(counter, value); }
std::uint64_t read() { return counter; }
#endif
} // namespace
int main() {
  gti_runtime_detail::begin_thread_local_storage_lifetime();
  const auto coldStart = gti_tls_bench_clock();
  add(0);
  const auto cold = checkedSubtract(gti_tls_bench_clock(), coldStart);
  const auto iterations = gti_tls_bench_iterations();
  const auto start = gti_tls_bench_clock();
  for (std::uint64_t index = 0; index < iterations;
       index = checkedAdd(index, 1)) {
#ifdef GTI_TLS_BENCH_DYNAMIC
    // GTI prepares the receiver before evaluating explicit arguments.
    ensure();
    Resource *receiver = slot.address();
    const auto amount = gti_tls_bench_step(index);
    receiver->value = checkedAdd(receiver->value, amount);
#else
    // GTI's primitive compound update reads its old value before the RHS.
    const auto previous = counter;
    const auto amount = gti_tls_bench_step(index);
    counter = checkedAdd(previous, amount);
#endif
  }
  const auto elapsed = checkedSubtract(gti_tls_bench_clock(), start);
  const auto value = read();
  gti_runtime_detail::drain_thread_local_cleanup();
  gti_tls_bench_report(elapsed, cold, value);
  return 0;
}
