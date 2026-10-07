#include "cthread_internal.h"
#include "gti/cthread.h"
#include <atomic>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string_view>
#include <thread>

namespace {
using namespace gti::runtime::detail;
std::atomic<int> failures{0};
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    ++failures;
  }
}
void other() {}
void nested() {
  expect(enterCOnceInitializer(nested), "nested exact enter");
  expect(leaveCOnceInitializer(nested), "nested exact leave");
}
void checked() {
  const bool outerTerminal = terminalCThreadCallbackActive();
  expect(!enterCOnceInitializer(other), "different initializer cannot claim");
  expect(enterCOnceInitializer(checked), "exact initializer enters");
  expect(terminalCThreadCallbackActive(),
         "entered callback owns terminal failure policy");
  expect(!enterCOnceInitializer(checked),
         "recursive unregistered entry rejected");
  invokeCOnceInitializer(nested);
  expect(!leaveCOnceInitializer(other), "different initializer cannot leave");
  expect(leaveCOnceInitializer(checked),
         "outer registration restored after nested call");
  expect(terminalCThreadCallbackActive() == outerTerminal,
         "normal leave restores the surrounding failure policy");
  expect(!enterCOnceInitializer(checked) && !leaveCOnceInitializer(checked),
         "replay rejected");
}
void foreignCheck() {
  expect(!enterCOnceInitializer(foreignCheck),
         "foreign thread cannot enter GTI");
}
std::int32_t manual(void *) {
  expect(enterCThreadCallback(manual), "manual registered entry");
  invokeCOnceInitializer(checked);
  expect(leaveCThreadCallback(manual), "manual registered leave");
  return 0;
}
void throws() { throw 7; }
void incomplete() { gti_rt_once_enter_v1(incomplete); }
} // namespace

int main(int argc, char **argv) {
  std::set_terminate([] { std::_Exit(86); });
  expect(!enterCOnceInitializer(checked), "no invocation cannot enter");
  expect(beginCThreadRuntime() == CThreadStatus::Success, "runtime begins");
  if (argc == 2) {
    const std::string_view mode(argv[1]);
    if (mode == "throws") {
      invokeCOnceInitializer(throws);
    }
    if (mode == "incomplete") {
      invokeCOnceInitializer(incomplete);
    }
    if (mode == "null") {
      invokeCOnceInitializer(nullptr);
    }
    return 4;
  }
  invokeCOnceInitializer(checked);
  expect(!leaveCOnceInitializer(checked), "invocation cannot escape its scope");
  std::thread foreign([] { invokeCOnceInitializer(foreignCheck); });
  foreign.join();
  CThreadHandle handle = 0;
  expect(createCThread(&handle, manual, nullptr) == CThreadStatus::Success &&
             joinCThread(handle, nullptr) == CThreadStatus::Success,
         "manual invocation completes");
  std::thread managed([] {
    enterCThreadManagedScope();
    invokeCOnceInitializer(checked);
    leaveCThreadManagedScope();
    invokeCOnceInitializer(foreignCheck);
  });
  managed.join();
  expect(shutdownCThreadRuntime() == CThreadStatus::Success,
         "runtime shuts down");
  invokeCOnceInitializer(foreignCheck);
  return failures == 0 ? 0 : 1;
}
