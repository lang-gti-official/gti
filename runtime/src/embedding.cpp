#include "embedding_internal.h"

#include <atomic>
#include <cstdlib>
#include <thread>

struct gti_context_v1 {
  enum class State { Inactive, Creating, Active, Invoking };
  State state = State::Inactive;
  std::thread::id owner;
  gti_failure_observer_v1 observer = nullptr;
  void *observerContext = nullptr;
};

namespace {
gti_context_v1 context;
std::atomic_flag lock = ATOMIC_FLAG_INIT;

class Guard {
public:
  Guard() noexcept : held(!lock.test_and_set(std::memory_order_acquire)) {}
  ~Guard() {
    if (held)
      lock.clear(std::memory_order_release);
  }
  bool held;
};

gti_embed_status_v1 active(gti_context_v1 *value) noexcept {
  if (value != &context)
    return GTI_EMBED_STATUS_INVALID_ARGUMENT_V1;
  if (context.state == gti_context_v1::State::Inactive)
    return GTI_EMBED_STATUS_CONTEXT_INACTIVE_V1;
  if (context.owner != std::this_thread::get_id())
    return GTI_EMBED_STATUS_WRONG_THREAD_V1;
  if (context.state != gti_context_v1::State::Active)
    return GTI_EMBED_STATUS_CONTEXT_BUSY_V1;
  return GTI_EMBED_STATUS_OK_V1;
}

// Finishing a generated operation must not spuriously fail because another
// host thread is checking entry. Only these constant-time critical sections
// spin; no user code runs with the flag held.
void acquire() noexcept {
  while (lock.test_and_set(std::memory_order_acquire)) {
  }
}
} // namespace

extern "C" gti_embed_status_v1
gti_rt_embed_create_begin_v1(uint32_t abi, gti_failure_observer_v1 observer,
                             void *observerContext, gti_context_v1 **outContext,
                             gti_failure_record_v1 *outFailure) noexcept {
  if (!outContext || !outFailure)
    return GTI_EMBED_STATUS_INVALID_ARGUMENT_V1;
  if (abi != GTI_EMBED_ABI_VERSION_V1)
    return GTI_EMBED_STATUS_ABI_MISMATCH_V1;
  Guard guard;
  if (!guard.held || context.state != gti_context_v1::State::Inactive)
    return GTI_EMBED_STATUS_CONTEXT_BUSY_V1;
  context.state = gti_context_v1::State::Creating;
  context.owner = std::this_thread::get_id();
  context.observer = observer;
  context.observerContext = observerContext;
  return GTI_EMBED_STATUS_OK_V1;
}

extern "C" void
gti_rt_embed_create_end_v1(gti_context_v1 **outContext) noexcept {
  acquire();
  if (!outContext || context.state != gti_context_v1::State::Creating ||
      context.owner != std::this_thread::get_id())
    std::abort();
  context.state = gti_context_v1::State::Active;
  *outContext = &context;
  lock.clear(std::memory_order_release);
}

extern "C" gti_embed_status_v1
gti_rt_embed_enter_v1(gti_context_v1 *value,
                      gti_failure_record_v1 *outFailure) noexcept {
  if (!outFailure || !value)
    return GTI_EMBED_STATUS_INVALID_ARGUMENT_V1;
  Guard guard;
  if (!guard.held)
    return GTI_EMBED_STATUS_CONTEXT_BUSY_V1;
  const auto status = active(value);
  if (status == GTI_EMBED_STATUS_OK_V1)
    context.state = gti_context_v1::State::Invoking;
  return status;
}

extern "C" void gti_rt_embed_leave_v1(gti_context_v1 *value) noexcept {
  acquire();
  if (value != &context || context.state != gti_context_v1::State::Invoking ||
      context.owner != std::this_thread::get_id())
    std::abort();
  context.state = gti_context_v1::State::Active;
  lock.clear(std::memory_order_release);
}

extern "C" gti_embed_status_v1
gti_rt_embed_destroy_v1(gti_context_v1 *value,
                        gti_failure_record_v1 *outFailure) noexcept {
  if (!outFailure || !value)
    return GTI_EMBED_STATUS_INVALID_ARGUMENT_V1;
  Guard guard;
  if (!guard.held)
    return GTI_EMBED_STATUS_CONTEXT_BUSY_V1;
  const auto status = active(value);
  if (status != GTI_EMBED_STATUS_OK_V1)
    return status;
  // Admitted modules have no retained cleanup-owning state. Fallible/global
  // teardown must be implemented by verified generated cleanup, not here.
  context.state = gti_context_v1::State::Inactive;
  context.owner = {};
  context.observer = nullptr;
  context.observerContext = nullptr;
  return GTI_EMBED_STATUS_OK_V1;
}
