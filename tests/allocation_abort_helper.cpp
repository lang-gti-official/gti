#include "gti/runtime_failure.h"
#include "gti/runtime_thread.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace {

std::atomic<bool> forbidAllocation{false};

void marker(char value) {
#if defined(_WIN32)
  DWORD written = 0;
  (void)WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), &value, 1, &written,
                  nullptr);
#else
  (void)::write(STDOUT_FILENO, &value, 1);
#endif
}

struct Guard {
  ~Guard() { marker('D'); }
};

constexpr char source[] = "allocation.gti";
constexpr std::uint8_t canonical[] = {1};
constexpr gti_failure_outcome_descriptor_v1 outcome{
    GTI_FAILURE_CODE_ALLOCATION_FAILURE_V1,
    GTI_FAILURE_DETAIL_PRIVATE_STORAGE_V1, 0};
constexpr gti_failure_site_descriptor_v1 site{
    {source, sizeof(source) - 1}, 17, 4, 8, &outcome, 1, 0};
constexpr gti_failure_artifact_descriptor_v1 artifact{
    GTI_FAILURE_ABI_VERSION_V1, 0, {1}, &site, 1, 0, canonical,
    sizeof(canonical)};
constexpr gti_failure_record_v2 record{
    .abi_version = GTI_FAILURE_ABI_VERSION_V2,
    .code = GTI_FAILURE_CODE_ALLOCATION_FAILURE_V1,
    .detail = GTI_FAILURE_DETAIL_PRIVATE_STORAGE_V1,
    .site_index = 1,
    .artifact_identity = {1}};

[[noreturn]] void exhaust() {
  Guard guard;
  marker('B');
  forbidAllocation = true;
  gti_rt_failure_abort_allocation_v2(&record, &artifact);
}

void observer(const gti_failure_record_v2 *, void *) { exhaust(); }

void worker(void *) {
  gti_runtime_detail::run_managed_task_adapter([] { exhaust(); });
  marker('R');
}

void workerCleanup(void *) { marker('W'); }

} // namespace

// Catch accidental C++ heap allocation in the terminal report. The production
// reporter's bounded native writer also has no malloc/stream-formatting path.
void *operator new(std::size_t size) {
  if (forbidAllocation)
    std::_Exit(98);
  if (void *result = std::malloc(size == 0 ? 1 : size))
    return result;
  throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *address) noexcept { std::free(address); }
void operator delete[](void *address) noexcept { std::free(address); }

int main(int argc, char **argv) {
  if (argc != 2)
    return 99;
#if !defined(_WIN32)
  const rlimit noCore{0, 0};
  (void)setrlimit(RLIMIT_CORE, &noCore);
#endif
  std::atexit([] { marker('E'); });
  Guard guard;
  const char *mode = argv[1];
  if (std::strcmp(mode, "worker") == 0) {
    int context = 0;
    auto spawned = gti_rt_thread_spawn_v1(worker, &context, workerCleanup);
    if (spawned.status != GTI_THREAD_SPAWN_OK_V1)
      return 97;
    (void)gti_rt_thread_join_v2(&spawned.state);
    marker('J');
    return 96;
  }
  if (std::strcmp(mode, "observer") == 0) {
    auto primary = record;
    primary.code = GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1;
    primary.detail = GTI_FAILURE_DETAIL_ADDITION_V1;
    primary.site_index = 0;
    std::memset(primary.artifact_identity, 0,
                sizeof(primary.artifact_identity));
    gti_rt_failure_terminate_v2(&primary, nullptr, observer, nullptr);
  }
  if (std::strcmp(mode, "invalid") == 0) {
    marker('B');
    forbidAllocation = true;
    gti_rt_failure_abort_allocation_v2(nullptr, nullptr);
  }
  if (std::strcmp(mode, "closed") == 0) {
#if defined(_WIN32)
    (void)SetStdHandle(STD_ERROR_HANDLE, INVALID_HANDLE_VALUE);
#else
    (void)::close(STDERR_FILENO);
#endif
  } else if (std::strcmp(mode, "broken_pipe") == 0) {
#if defined(_WIN32)
    HANDLE reader = nullptr;
    HANDLE writer = nullptr;
    if (!CreatePipe(&reader, &writer, nullptr, 0))
      return 95;
    (void)CloseHandle(reader);
    (void)SetStdHandle(STD_ERROR_HANDLE, writer);
#else
    int descriptors[2];
    if (::pipe(descriptors) != 0)
      return 95;
    (void)::close(descriptors[0]);
    if (::dup2(descriptors[1], STDERR_FILENO) < 0)
      return 94;
    (void)::close(descriptors[1]);
#endif
  } else if (std::strcmp(mode, "direct") != 0) {
    return 93;
  }
  exhaust();
}
