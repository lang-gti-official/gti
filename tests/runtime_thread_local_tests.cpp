#include "gti/runtime_thread.hpp"

#include <array>
#include <cstdlib>
#include <iostream>

namespace {
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

struct Context {
  std::array<int, 8> order{};
  int count = 0;
  int failures = 0;
  int abandonedResults = 0;
  bool bodyFailure = false;
  bool reportThroughDestructor = false;
  bool sawUnwinding = false;
};

struct Node {
  gti_runtime_detail::thread_local_cleanup_entry registration;
  Context *context = nullptr;
  int id = 0;
};

gti_failure_record_v2 failureRecord() {
  return {.abi_version = GTI_FAILURE_ABI_VERSION_V2,
          .code = GTI_FAILURE_CODE_PANIC_V2,
          .detail = GTI_FAILURE_DETAIL_EXPLICIT_PANIC_V2};
}

bool cleanup(void *storage, gti_failure_record_v2 *failure,
             const gti_failure_record_v2 *primary,
             const gti_failure_artifact_descriptor_v1 *primaryArtifact) {
  auto &node = *static_cast<Node *>(storage);
  auto &context = *node.context;
  expect(!node.registration.registered && !node.registration.previous,
         "cleanup registration is disarmed before the callback");
  expect(gti_runtime_detail::thread_local_cleanup_active(),
         "new initialization is disabled during cleanup");
  context.order[context.count++] = node.id;
  context.sawUnwinding |= primary != nullptr;
  expect(primaryArtifact == nullptr, "fixture's exact primary has no artifact");
  if (primary != nullptr)
    expect(primary->abi_version == GTI_FAILURE_ABI_VERSION_V2 &&
               primary->code == GTI_FAILURE_CODE_PANIC_V2,
           "callback receives the original primary record, not only a flag");
  if (context.failures == 0)
    return true;
  --context.failures;
  *failure = failureRecord();
  if (context.reportThroughDestructor) {
    expect(gti_runtime_detail::record_managed_task_cleanup_failure(*failure,
                                                                   nullptr),
           "native destructor failure belongs to the same cleanup phase");
    return true;
  }
  return false;
}

void entry(void *opaque) {
  auto &context = *static_cast<Context *>(opaque);
  thread_local std::array<Node, 3> owners{};
  for (int index = 0; index != 3; ++index) {
    auto &node = owners[index];
    node.context = &context;
    node.id = index + 1;
    node.registration.storage = &node;
    node.registration.cleanup = cleanup;
    gti_runtime_detail::register_thread_local_cleanup(&node.registration);
  }
  if (context.bodyFailure)
    gti_runtime_detail::handoff_managed_worker_failure(failureRecord(),
                                                       nullptr);
}

void destroy(void *opaque) {
  auto &context = *static_cast<Context *>(opaque);
  context.order[context.count++] = 0;
}

void ordinaryAndFailureExit() {
  for (int mode = 0; mode != 5; ++mode) {
    Context context;
    context.failures = mode == 0 ? 0 : mode == 2 ? 2 : 1;
    context.bodyFailure = mode == 3;
    context.reportThroughDestructor = mode == 4;
    auto started = gti_rt_thread_spawn_v1(entry, &context, destroy);
    expect(started.status == GTI_THREAD_SPAWN_OK_V1, "native task starts");
    const auto result = gti_rt_thread_join_v2(&started.state);
    const bool emergency = mode == 2 || mode == 3;
    expect(result.status == (mode == 0 ? GTI_THREAD_JOIN_OK_V1
                                       : GTI_THREAD_JOIN_WORKER_FAILURE_V1),
           "join observes TLS cleanup before publishing its outcome");
    expect(result.failure_kind == (mode == 0 ? GTI_THREAD_WORKER_FAILURE_NONE_V1
                                   : emergency
                                       ? GTI_THREAD_WORKER_FAILURE_CLEANUP_V1
                                       : GTI_THREAD_WORKER_FAILURE_ORDINARY_V1),
           "ordinary and secondary cleanup failures retain their categories");
    expect(context.order[0] == 0 && context.order[1] == 3 &&
               context.count == (mode == 2   ? 3
                                 : mode == 3 ? 2
                                             : 4),
           "captures retire before reverse TLS cleanup; emergency abandons the "
           "rest");
    if (!emergency)
      expect(context.order[2] == 2 && context.order[3] == 1,
             "normal and ordinary-failure cleanup completes in reverse order");
    if (mode != 0)
      expect(context.sawUnwinding,
             "remaining cleanup sees the exact pending primary");
  }
}

struct Result {
  Context *context;
};
void *complete(void *opaque) {
  destroy(opaque);
  return new Result{static_cast<Context *>(opaque)};
}
void take(void *opaque, void *destination) {
  *static_cast<int *>(destination) = 7;
  delete static_cast<Result *>(opaque);
}
void abandon(void *opaque) {
  auto *value = static_cast<Result *>(opaque);
  ++value->context->abandonedResults;
  value->context->order[value->context->count++] = 99;
  delete value;
}

void pendingResultFailure() {
  Context context;
  context.failures = 1;
  auto started = gti_rt_thread_spawn_value_v1(entry, &context, complete,
                                              destroy, take, abandon);
  expect(started.status == GTI_THREAD_SPAWN_OK_V1, "value task starts");
  const auto result = gti_rt_thread_join_value_v2(&started.state);
  expect(
      result.join.status == GTI_THREAD_JOIN_WORKER_FAILURE_V1 &&
          result.result_context == nullptr && context.abandonedResults == 1 &&
          context.order[context.count - 1] == 99,
      "TLS failure retires an unpublished result once on its producing thread");
}
} // namespace

int main() {
  ordinaryAndFailureExit();
  pendingResultFailure();
}
