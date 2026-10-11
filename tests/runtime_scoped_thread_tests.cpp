#include "gti/runtime_thread.h"
#include "gti/runtime_thread.hpp"
#include "thread_internal.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <string_view>
#include <thread>
#include <type_traits>

namespace {
int failures = 0;
void expect(bool value, const char *message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

struct Borrowed {
  int &value;
  int &destroyed;
  std::atomic<int> *arrived = nullptr;
  ~Borrowed() { ++destroyed; }
};

void writeBorrowed(void *opaque) {
  auto &context = *static_cast<Borrowed *>(opaque);
  if (context.arrived) {
    context.arrived->fetch_add(1, std::memory_order_acq_rel);
    while (context.arrived->load(std::memory_order_acquire) != 2)
      std::this_thread::yield();
  }
  context.value = 42;
}

void testBorrowedPublication() {
  int left = 0, right = 0, destroyed = 0;
  std::atomic<int> arrived{0};
  {
    Borrowed a{left, destroyed, &arrived}, b{right, destroyed, &arrived};
    const gti_thread_scoped_task_v1 tasks[] = {{writeBorrowed, &a},
                                               {writeBorrowed, &b}};
    gti_thread_state_v1 *scratch[2]{};
    const auto result = gti_rt_thread_scope_v1(tasks, scratch, 2);
    expect(result.abi_version == GTI_THREAD_ABI_VERSION_V1 &&
               result.spawn_status == GTI_THREAD_SPAWN_OK_V1 &&
               result.started == 2 && result.failed_worker == UINT64_MAX &&
               result.worker.status == GTI_THREAD_JOIN_OK_V1,
           "scope starts and completes both workers");
    expect(
        left == 42 && right == 42 && destroyed == 0 && scratch[0] == nullptr &&
            scratch[1] == nullptr,
        "join publishes ordinary writes without destroying borrowed contexts");
    // Reuse is immediately legal after the synchronous scope returns.
    left += 1;
    right += 2;
  }
  expect(left == 43 && right == 44 && destroyed == 2,
         "caller retains context lifetime and regains exclusive access");
}

struct Failing {
  gti_failure_record_v2 record{};
  gti_failure_artifact_descriptor_v1 artifact{};
  std::atomic<bool> *waitFor = nullptr;
  std::atomic<bool> *publish = nullptr;
};

void failBorrowed(void *opaque) {
  auto &context = *static_cast<Failing *>(opaque);
  if (context.waitFor)
    while (!context.waitFor->load(std::memory_order_acquire))
      std::this_thread::yield();
  gti_runtime_detail::run_managed_task_adapter([&] {
    gti_rt_failure_terminate_v2(&context.record, &context.artifact, nullptr,
                                nullptr);
  });
  if (context.publish)
    context.publish->store(true, std::memory_order_release);
}

Failing failure(unsigned site) {
  Failing result;
  result.record.abi_version = GTI_FAILURE_ABI_VERSION_V2;
  result.record.code = GTI_FAILURE_CODE_PANIC_V2;
  result.record.detail = GTI_FAILURE_DETAIL_EXPLICIT_PANIC_V2;
  result.record.site_index = site;
  result.artifact.abi_version = GTI_FAILURE_ABI_VERSION_V1;
  (void)gti_rt_failure_capture_panic_v2(
      &result.record, {.data = "scoped worker", .length = 13});
  return result;
}

void testJoinAllBeforeFailure() {
  int value = 0, destroyed = 0;
  Borrowed successful{value, destroyed};
  std::atomic<bool> secondFinished{false};
  auto first = failure(1), second = failure(2);
  first.waitFor = &secondFinished;
  second.publish = &secondFinished;
  const gti_thread_scoped_task_v1 tasks[] = {{failBorrowed, &first},
                                             {writeBorrowed, &successful},
                                             {failBorrowed, &second}};
  gti_thread_state_v1 *scratch[3]{};
  const auto result = gti_rt_thread_scope_v1(tasks, scratch, 3);
  expect(
      result.started == 3 && result.failed_worker == 0 &&
          result.worker.status == GTI_THREAD_JOIN_WORKER_FAILURE_V1 &&
          result.worker.failure_kind == GTI_THREAD_WORKER_FAILURE_ORDINARY_V1 &&
          std::memcmp(&result.worker.failure, &first.record,
                      sizeof(first.record)) == 0 &&
          result.worker.primary_artifact == &first.artifact,
      "first input failure is preserved even when another worker fails first");
  expect(value == 42 && destroyed == 0 && scratch[0] == nullptr &&
             scratch[1] == nullptr && scratch[2] == nullptr,
         "all child accesses finish before any worker failure is returned");
}

void testPartialCreation() {
  int value = 0, notStarted = 0, destroyed = 0;
  Borrowed first{value, destroyed}, last{notStarted, destroyed};
  const gti_thread_scoped_task_v1 tasks[] = {
      {writeBorrowed, &first}, {nullptr, &last}, {writeBorrowed, &last}};
  gti_thread_state_v1 *scratch[3]{};
  const auto result = gti_rt_thread_scope_v1(tasks, scratch, 3);
  expect(
      result.spawn_status == GTI_THREAD_SPAWN_RESOURCE_UNAVAILABLE_V1 &&
          result.started == 1 && result.failed_worker == UINT64_MAX &&
          value == 42 && notStarted == 0 && destroyed == 0 &&
          scratch[0] == nullptr && scratch[1] == nullptr &&
          scratch[2] == nullptr,
      "partial creation joins its prefix and leaves unstarted contexts owned");

  auto failed = failure(3);
  const gti_thread_scoped_task_v1 mixed[] = {{failBorrowed, &failed},
                                             {nullptr, &last}};
  const auto both = gti_rt_thread_scope_v1(mixed, scratch, 2);
  expect(both.started == 1 && both.failed_worker == 0 &&
             both.spawn_status == GTI_THREAD_SPAWN_RESOURCE_UNAVAILABLE_V1 &&
             both.worker.failure.site_index == 3,
         "creation failure does not discard an earlier worker's failure");

  gti::runtime::detail::setThreadTestFault(
      gti::runtime::detail::ThreadTestFault::SpawnOperationNotPermitted);
  const auto denied = gti_rt_thread_scope_v1(tasks, scratch, 3);
  expect(denied.started == 0 &&
             denied.spawn_status ==
                 GTI_THREAD_SPAWN_OPERATION_NOT_PERMITTED_V1 &&
             destroyed == 0,
         "native spawn failure retains caller-owned contexts");
}

void nestedScope(void *opaque) {
  const gti_thread_scoped_task_v1 child{writeBorrowed, opaque};
  gti_thread_state_v1 *scratch{};
  const auto result = gti_rt_thread_scope_v1(&child, &scratch, 1);
  if (result.spawn_status != GTI_THREAD_SPAWN_OK_V1 || scratch != nullptr)
    std::abort();
}

void testNestedAndEmpty() {
  const auto empty = gti_rt_thread_scope_v1(nullptr, nullptr, 0);
  expect(empty.started == 0 && empty.spawn_status == GTI_THREAD_SPAWN_OK_V1,
         "zero tasks needs no arrays or workers");
  gti_thread_state_v1 *invalidScratch[2]{nullptr, nullptr};
  const auto missingTasks = gti_rt_thread_scope_v1(nullptr, invalidScratch, 2);
  expect(missingTasks.started == 0 &&
             missingTasks.spawn_status ==
                 GTI_THREAD_SPAWN_OPERATION_NOT_PERMITTED_V1 &&
             invalidScratch[0] == nullptr && invalidScratch[1] == nullptr,
         "missing task array starts no workers and clears available scratch");
  const auto missingScratch = gti_rt_thread_scope_v1(nullptr, nullptr, 1);
  expect(missingScratch.started == 0 &&
             missingScratch.spawn_status ==
                 GTI_THREAD_SPAWN_OPERATION_NOT_PERMITTED_V1,
         "missing nonempty scratch is rejected before starting workers");
  int value = 0, destroyed = 0;
  Borrowed context{value, destroyed};
  const gti_thread_scoped_task_v1 task{nestedScope, &context};
  gti_thread_state_v1 *scratch{};
  const auto nested = gti_rt_thread_scope_v1(&task, &scratch, 1);
  expect(nested.spawn_status == GTI_THREAD_SPAWN_OK_V1 && value == 42 &&
             destroyed == 0 && scratch == nullptr,
         "nested scopes complete before their containing worker returns");
}
} // namespace

static_assert(std::is_standard_layout_v<gti_thread_scoped_task_v1> &&
              std::is_trivially_copyable_v<gti_thread_scoped_task_v1>);
static_assert(std::is_standard_layout_v<gti_thread_scope_result_v1> &&
              std::is_trivially_copyable_v<gti_thread_scope_result_v1>);

int main(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "join-failure") {
    std::set_terminate([] { std::_Exit(73); });
    int value = 0, destroyed = 0;
    Borrowed context{value, destroyed};
    const gti_thread_scoped_task_v1 task{writeBorrowed, &context};
    gti_thread_state_v1 *scratch{};
    gti::runtime::detail::setThreadTestFault(
        gti::runtime::detail::ThreadTestFault::JoinFailed);
    (void)gti_rt_thread_scope_v1(&task, &scratch, 1);
    return 1; // A failed join must never return or unwind the borrowed stack.
  }
  testBorrowedPublication();
  testJoinAllBeforeFailure();
  testPartialCreation();
  testNestedAndEmpty();
  return failures == 0 ? 0 : 1;
}
