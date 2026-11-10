#include "gti/cpp_backend.h"
#include "gti/frontend.h"

#include "cpp_backend_test_support.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

// Frozen C++ backend representation of managed threads: the task, join, and
// typed-result runtime spellings both C++ standards emit. The
// backend-neutral managed-thread contract lives in managed_thread_tests.

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

lang::TargetInfo concurrentTarget(bool threads = true) {
  lang::TargetInfo target = lang::TargetInfo::host();
  target.executionProfile = lang::ExecutionProfile::Concurrent;
  target.runtimeCapabilities.threads = threads;
  return target;
}

lang::FrontendResult analyze(std::string_view name, std::string source,
                             lang::TargetInfo target = concurrentTarget()) {
  lang::FrontendOptions options;
  options.target = std::move(target);
  const std::filesystem::path standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend(std::move(options))
      .analyze(std::string(name), std::move(source),
               {standardLibrary / "prelude.gti"}, {}, {standardLibrary});
}

void printDiagnostics(const lang::FrontendResult &result) {
  for (const lang::Diagnostic &diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
}

std::string validProgram() {
  return R"(
import std.thread;
import std.memory;

class resource {
  int32_t value;
public:
  resource(int32_t value) : value(value) {}
  int32_t read() { return this.value; }
};

class class_task {
public:
  void operator()() { std::println("class read task"); }
  void operator()() mut { std::println("class mutable task"); }
  void operator()() && { std::println("class once task"); }
public:
  class_task() = default;
};

class fresh_task {
public:
  void operator()() { std::println("fresh class task"); }
public:
  fresh_task() = default;
};

int main() {
  auto first_started = std::thread::start(
      []() -> void { std::println("first task"); });
  if (!first_started.has_value()) { return 1; }
  std::thread first = std::move(first_started).value();
  mut std::thread moved = std::move(first);
  auto first_join = moved.join();
  if (!first_join.has_value() || moved.joinable()) { return 2; }

  auto owner = std::make_unique<resource>(42);
  auto second_started = std::thread::start(
      [owned = std::move(owner)]() -> void {
        std::println(owned->read());
      });
  if (!second_started.has_value()) { return 3; }
  mut std::thread second = std::move(second_started).value();
  auto second_join = second.join();
  if (!second_join.has_value()) { return 4; }

  class_task task = class_task();
  auto third_started = std::thread::start(std::move(task));
  if (!third_started.has_value()) { return 5; }
  mut std::thread third = std::move(third_started).value();
  auto third_join = third.join();
  if (!third_join.has_value()) { return 6; }

  {
    auto fourth_started = std::thread::start(fresh_task());
    if (!fourth_started.has_value()) { return 7; }
    std::thread fourth = std::move(fourth_started).value();
  }
  std::println("after automatic join");
  return 0;
}
)";
}

lang::LoweredProgram lowerConcurrent(const lang::FrontendResult &frontend) {
  return gti_test::lowerProgram(frontend, frontend.mir, frontend.mir,
                                concurrentTarget());
}

void testPublicPipelineCppText() {
  const lang::FrontendResult frontend =
      analyze("managed-thread.gti", validProgram());
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    expect(false, "the public thread workflow should complete analysis");
    return;
  }
  const lang::LoweredProgram lowered = lowerConcurrent(frontend);
  const std::string cpp20 =
      lang::CppBackend(lang::CppStandard::Cpp20).generate(lowered).contents;
  const std::string cpp23 =
      lang::CppBackend(lang::CppStandard::Cpp23).generate(lowered).contents;
  expect(
      cpp20.find("managed_thread_spawn<") != std::string::npos &&
          cpp20.find("managed_thread_join(") != std::string::npos &&
          cpp23.find("managed_thread_task_adapter<") != std::string::npos &&
          cpp23.find("gti_rt_failure_terminate_v2") != std::string::npos,
      "both C++ standards should consume lowered task and failure authority");
}

void testTypedResultCppText() {
  const lang::FrontendResult frontend = analyze("managed-thread-result.gti", R"(
import std.thread;

class result_task {
  int32_t value;
public:
  result_task(int32_t value) : value(value) {}
  int32_t operator()() && { return this.value; }
};

int main() {
  mut std::thread_result<int32_t> first =
      std::thread_result<int32_t>();
  auto first_started = first.start(result_task(42));
  if (!first_started.has_value()) { return 1; }

  auto first_joined = first.join();
  if (!first_joined.has_value()) { return 2; }

  auto restarted = first.start([]() -> int32_t { return 9; });
  if (!restarted.has_value()) { return 3; }
  auto rejoined = first.join();
  if (!rejoined.has_value()) { return 4; }

  mut std::thread_result<int32_t> abandoned =
      std::thread_result<int32_t>();
  auto abandoned_started = abandoned.start(result_task(7));
  if (!abandoned_started.has_value()) { return 5; }
  return 0;
}
)");
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    expect(false, "typed result tasks should complete analysis");
    return;
  }
  const lang::LoweredProgram lowered = lowerConcurrent(frontend);
  const std::string cpp20 =
      lang::CppBackend(lang::CppStandard::Cpp20).generate(lowered).contents;
  const std::string cpp23 =
      lang::CppBackend(lang::CppStandard::Cpp23).generate(lowered).contents;
  expect(cpp20.find("managed_thread_spawn_value<") != std::string::npos &&
             cpp20.find("managed_thread_spawn_value<"
                        "__gti_thread_task_adapter, std::int32_t>") !=
                 std::string::npos &&
             cpp20.find(".stash_result(") != std::string::npos &&
             cpp20.find(".template take_result<std::int32_t>()") !=
                 std::string::npos &&
             cpp23.find("gti_rt_thread_join_value_v2") != std::string::npos &&
             cpp23.find("managed_thread_abandon_join_result(") !=
                 std::string::npos,
         "both C++ standards should emit typed spawn, join, and abandon "
         "ownership transitions");
}

void testClassTypedResultCppText() {
  const lang::FrontendResult frontend =
      analyze("managed-thread-class-result.gti", R"(
import std.thread;

class result_value {
  int32_t value;
public:
  result_value(int32_t value) : value(value) {}
  result_value(result_value& other) = delete;
  result_value(result_value&& other) = default;
  int32_t read() { return this.value; }
};

int main() {
  mut std::thread_result<result_value> worker =
      std::thread_result<result_value>();
  auto started = worker.start([]() -> result_value {
    return result_value(42);
  });
  if (!started.has_value()) { return 1; }
  auto joined = worker.join();
  if (!joined.has_value()) { return 2; }
  result_value result = std::move(joined).value();
  if (result.read() != 42) { return 3; }
  return 0;
}
)");
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    expect(false, "class-valued result tasks should complete analysis");
    return;
  }
  const lang::LoweredProgram lowered = lowerConcurrent(frontend);
  const std::string cpp20 =
      lang::CppBackend(lang::CppStandard::Cpp20).generate(lowered).contents;
  const std::string cpp23 =
      lang::CppBackend(lang::CppStandard::Cpp23).generate(lowered).contents;
  expect(cpp20.find("managed_thread_spawn_value<") != std::string::npos &&
             cpp20.find(".stash_result(") != std::string::npos &&
             cpp23.find(".template take_result<") != std::string::npos,
         "both C++ standards should place, join, and take a class result "
         "without default construction");
}

} // namespace

int main() {
  testPublicPipelineCppText();
  testTypedResultCppText();
  testClassTypedResultCppText();
  return failures == 0 ? 0 : 1;
}
