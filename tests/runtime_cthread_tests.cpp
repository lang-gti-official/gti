#include "cthread_internal.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_set>

namespace {

using namespace gti::runtime::detail;
std::atomic<int> failures{0};

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class Gate {
public:
  void open() {
    const std::lock_guard lock(mutex);
    ready = true;
    condition.notify_all();
  }
  void wait() {
    std::unique_lock lock(mutex);
    condition.wait(lock, [&] { return ready; });
  }

private:
  std::mutex mutex;
  std::condition_variable condition;
  bool ready = false;
};

std::int32_t constant(void *) {
  return std::numeric_limits<std::int32_t>::min();
}

std::int32_t publish(void *context) {
  auto &value = *static_cast<std::int32_t *>(context);
  value += 7;
  return value;
}

std::int32_t publishIdentity(void *context) {
  const auto identity = currentCThread();
  *static_cast<CThreadHandle *>(context) = identity;
  expect(identity != 0 && currentCThread() == identity,
         "manual worker identity is stable before adapter entry");
  expect(enterCThreadCallback(publishIdentity),
         "identity does not claim entry");
  expect(currentCThread() == identity, "entry preserves identity");
  expect(leaveCThreadCallback(publishIdentity),
         "identity does not claim leave");
  expect(currentCThread() == identity, "leave preserves identity");
  return 0;
}

void testIdentity() {
  const auto initial = currentCThread();
  expect(initial != 0 && initial == currentCThread(),
         "initial identity is stable");
  std::unordered_set<CThreadHandle> identities{initial};
  for (int round = 0; round < 4; ++round) {
    Gate start;
    std::array<std::thread, 32> threads;
    std::array<CThreadHandle, 32> observed{};
    for (std::size_t index = 0; index < threads.size(); ++index) {
      threads[index] = std::thread([&, index] {
        start.wait();
        const auto identity = currentCThread();
        observed[index] = identity;
        expect(identity != 0 && identity == currentCThread(),
               "foreign identity is stable");
        expect(!enterCThreadCallback(publishIdentity),
               "observing foreign identity must not grant callback entry");
        expect(joinCThread(identity, nullptr) == CThreadStatus::Error,
               "observing identity creates no join obligation");
      });
    }
    start.open();
    for (auto &thread : threads) {
      thread.join();
    }
    for (auto identity : observed) {
      expect(identities.insert(identity).second,
             "simultaneous and exited native thread IDs are never reused");
    }
    CThreadHandle handle = 0;
    CThreadHandle observedWorker = 0;
    expect(createCThread(&handle, publishIdentity, &observedWorker) ==
                   CThreadStatus::Success &&
               joinCThread(handle, nullptr) == CThreadStatus::Success,
           "manual identity worker creates and joins");
    expect(handle == observedWorker && identities.insert(handle).second,
           "manual worker observes its exact globally unique create handle");
  }
  expect(currentCThread() == initial &&
             joinCThread(initial, nullptr) == CThreadStatus::Error,
         "initial identity remains stable and is not joinable");
}

void testInitialization() {
  CThreadHandle handle = 55;
  expect(createCThread(&handle, constant, nullptr) == CThreadStatus::Error &&
             handle == 55,
         "creation before initialization must preserve the output");
  expect(shutdownCThreadRuntime() == CThreadStatus::Error,
         "shutdown before initialization must fail");
  expect(!enterCThreadCallback(constant) && !leaveCThreadCallback(constant),
         "initial thread must not impersonate an attached callback");
  expect(beginCThreadRuntime() == CThreadStatus::Success,
         "initial runtime start must succeed");
  expect(beginCThreadRuntime() == CThreadStatus::Error,
         "duplicate start must not reset runtime state");
  expect(createCThread(nullptr, constant, nullptr) == CThreadStatus::Error &&
             createCThread(&handle, nullptr, nullptr) == CThreadStatus::Error &&
             handle == 55,
         "invalid creation inputs must fail without publication");
  expect(joinCThread(0, nullptr) == CThreadStatus::Error &&
             joinCThread(55, nullptr) == CThreadStatus::Error,
         "zero and unissued identities must be rejected");
}

void testPublicationAndConsumption() {
  CThreadHandle previous = 0;
  for (int index = 0; index < 128; ++index) {
    CThreadHandle handle = 0;
    std::int32_t value = index;
    std::int32_t result = -1;
    expect(createCThread(&handle, publish, &value) == CThreadStatus::Success &&
               handle > previous,
           "every worker must receive a fresh nonzero identity");
    expect(joinCThread(handle, &result) == CThreadStatus::Success &&
               result == index + 7 && value == index + 7,
           "join must publish both the integer result and ordinary writes");
    result = -1;
    expect(joinCThread(handle, &result) == CThreadStatus::Error && result == -1,
           "copied consumed handles must not join or publish twice");
    previous = handle;
  }
  CThreadHandle handle = 0;
  std::int32_t result = 0;
  expect(createCThread(&handle, constant, nullptr) == CThreadStatus::Success &&
             joinCThread(handle, &result) == CThreadStatus::Success &&
             result == std::numeric_limits<std::int32_t>::min(),
         "null context and the complete signed result domain must survive");
  expect(createCThread(&handle, constant, nullptr) == CThreadStatus::Success &&
             joinCThread(handle, nullptr) == CThreadStatus::Success,
         "an explicit null result output may discard a joined result");
}

void testRecoverableFailures() {
  CThreadHandle handle = 99;
  std::int32_t value = 0;
  setCThreadTestFault(CThreadTestFault::CreateNoMemory);
  expect(createCThread(&handle, publish, &value) == CThreadStatus::NoMemory &&
             handle == 99 && value == 0,
         "allocation failure must not consume context or publish a handle");
  setCThreadTestFault(CThreadTestFault::CreateResourceUnavailable);
  expect(createCThread(&handle, publish, &value) == CThreadStatus::Error &&
             handle == 99 && value == 0,
         "native creation failure must preserve caller state");
  expect(createCThread(&handle, publish, &value) == CThreadStatus::Success,
         "fault injection must be one-shot");
  std::int32_t result = -1;
  setCThreadTestFault(CThreadTestFault::JoinFailed);
  expect(joinCThread(handle, &result) == CThreadStatus::Error && result == -1,
         "recoverable join failure must not publish or consume a handle");
  expect(joinCThread(handle, &result) == CThreadStatus::Success &&
             result == 7 && value == 7,
         "join retry must retain exactly one original worker/result");
}

std::int32_t attached(void *) {
  expect(!leaveCThreadCallback(attached), "leave must not precede entry");
  expect(!enterCThreadCallback(nullptr) && !enterCThreadCallback(constant),
         "attachment must bind the exact registered callback");
  expect(enterCThreadCallback(attached), "registered worker entry must attach");
  expect(!enterCThreadCallback(attached), "recursive attachment must fail");
  expect(shutdownCThreadRuntime() == CThreadStatus::Error,
         "a worker must not close its hosted runtime");

  CThreadHandle child = 0;
  std::int32_t value = 8;
  std::int32_t result = 0;
  expect(createCThread(&child, publish, &value) == CThreadStatus::Success &&
             joinCThread(child, &result) == CThreadStatus::Success &&
             result == 15,
         "attached worker may create and join a child without losing TLS");
  expect(!leaveCThreadCallback(constant), "wrong callback cannot detach entry");
  expect(leaveCThreadCallback(attached), "exact callback must finish once");
  expect(!leaveCThreadCallback(attached) && !enterCThreadCallback(attached),
         "completed callback entry cannot be replayed");
  return 42;
}

void testAttachment() {
  CThreadHandle handle = 0;
  std::int32_t result = 0;
  expect(!enterCThreadCallback(attached), "initial thread is not a worker");
  std::thread foreign([] {
    expect(!enterCThreadCallback(attached) && !leaveCThreadCallback(attached),
           "unregistered native thread must not enter GTI");
    expect(shutdownCThreadRuntime() == CThreadStatus::Error,
           "a foreign thread must not close another invocation's runtime");
  });
  foreign.join();
  expect(createCThread(&handle, attached, nullptr) == CThreadStatus::Success &&
             joinCThread(handle, &result) == CThreadStatus::Success &&
             result == 42,
         "exact attached entry must return its result through join");
  expect(!enterCThreadCallback(attached),
         "worker attachment must not leak TLS");
}

struct SelfContext {
  Gate published;
  Gate completed;
  CThreadHandle handle = 0;
};

std::int32_t selfJoin(void *opaque) {
  auto &context = *static_cast<SelfContext *>(opaque);
  context.published.wait();
  std::int32_t result = -1;
  expect(joinCThread(context.handle, &result) == CThreadStatus::Error &&
             result == -1,
         "self-join must fail without consuming the obligation");
  context.completed.open();
  return 23;
}

void testSelfJoin() {
  SelfContext context;
  expect(createCThread(&context.handle, selfJoin, &context) ==
             CThreadStatus::Success,
         "self-join setup must succeed");
  context.published.open();
  context.completed.wait();
  std::int32_t result = 0;
  expect(joinCThread(context.handle, &result) == CThreadStatus::Success &&
             result == 23,
         "another thread must still consume the self-join handle");
}

struct BlockingContext {
  Gate entered;
  Gate finish;
};

std::int32_t blocked(void *opaque) {
  auto &context = *static_cast<BlockingContext *>(opaque);
  context.entered.open();
  context.finish.wait();
  return 77;
}

void testSimultaneousJoin() {
  BlockingContext context;
  CThreadHandle handle = 0;
  expect(createCThread(&handle, blocked, &context) == CThreadStatus::Success,
         "competing join setup must succeed");
  context.entered.wait();
  Gate start;
  Gate rejected;
  std::atomic<int> successes{0};
  std::atomic<int> busy{0};
  const auto join = [&] {
    start.wait();
    std::int32_t result = -1;
    const CThreadStatus status = joinCThread(handle, &result);
    if (status == CThreadStatus::Success) {
      expect(result == 77, "winning join must receive the one result");
      ++successes;
    } else {
      expect(status == CThreadStatus::Busy && result == -1,
             "competing join must reject while the first join is blocked");
      ++busy;
      rejected.open();
    }
  };
  std::thread first(join);
  std::thread second(join);
  start.open();
  rejected.wait();
  context.finish.open();
  first.join();
  second.join();
  expect(successes == 1 && busy == 1,
         "exactly one concurrent join may consume");
}

std::int32_t delayedAttachment(void *opaque) {
  auto &context = *static_cast<BlockingContext *>(opaque);
  context.entered.open();
  context.finish.wait();
  expect(enterCThreadCallback(delayedAttachment),
         "already registered callback may attach while shutdown drains");
  expect(leaveCThreadCallback(delayedAttachment),
         "registered callback may finish while shutdown drains");
  return 81;
}

void testShutdown() {
  BlockingContext context;
  CThreadHandle handle = 0;
  expect(createCThread(&handle, delayedAttachment, &context) ==
             CThreadStatus::Success,
         "shutdown setup must succeed");
  context.entered.wait();
  expect(shutdownCThreadRuntime() == CThreadStatus::Busy,
         "shutdown must retain the in-flight join obligation");
  CThreadHandle rejected = 123;
  expect(createCThread(&rejected, constant, nullptr) == CThreadStatus::Error &&
             rejected == 123 && beginCThreadRuntime() == CThreadStatus::Error,
         "shutdown must close creation and cannot be reopened");
  context.finish.open();
  std::int32_t result = 0;
  expect(joinCThread(handle, &result) == CThreadStatus::Success && result == 81,
         "join must stay available during shutdown");
  expect(shutdownCThreadRuntime() == CThreadStatus::Success,
         "shutdown may finish only after all workers have joined");
  expect(shutdownCThreadRuntime() == CThreadStatus::Error &&
             beginCThreadRuntime() == CThreadStatus::Error &&
             createCThread(&rejected, constant, nullptr) ==
                 CThreadStatus::Error &&
             rejected == 123 && !enterCThreadCallback(delayedAttachment),
         "stopped runtime must reject restart, creation, and callback entry");
}

std::int32_t throwsNative(void *) { throw 1; }
std::int32_t missesLeave(void *) {
  expect(enterCThreadCallback(missesLeave),
         "malformed adapter must enter first");
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2) {
    // Subprocess tests distinguish an intentional native firewall from either
    // a successful result or an exception escaping into the host runtime.
    std::set_terminate([] { std::_Exit(86); });
    const std::string_view mode(argv[1]);
    const CThreadEntry entry = mode == "throws"          ? throwsNative
                               : mode == "missing-leave" ? missesLeave
                                                         : nullptr;
    CThreadHandle handle = 0;
    if (beginCThreadRuntime() != CThreadStatus::Success || entry == nullptr ||
        createCThread(&handle, entry, nullptr) != CThreadStatus::Success) {
      return 2;
    }
    (void)joinCThread(handle, nullptr);
    return 3;
  }
  const auto initial = currentCThread();
  testInitialization();
  testIdentity();
  testPublicationAndConsumption();
  testRecoverableFailures();
  testAttachment();
  testSelfJoin();
  testSimultaneousJoin();
  testShutdown();
  expect(currentCThread() == initial,
         "identity is independent of service initialization and shutdown");
  return failures == 0 ? 0 : 1;
}
