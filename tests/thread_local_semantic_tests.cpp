#include "gti/frontend.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
int failures = 0;
void expect(bool condition, std::string_view message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

lang::FrontendResult analyze(std::string source, bool concurrent = true) {
  lang::FrontendOptions options;
  options.target = lang::TargetInfo::host();
  if (concurrent) {
    options.target.executionProfile = lang::ExecutionProfile::Concurrent;
    options.target.runtimeCapabilities.threads = true;
  }
  const auto library = std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend(options).analyze("thread-local-semantics.gti", std::move(source),
                                         {library / "prelude.gti"}, {}, {library});
}

bool code(const lang::FrontendResult &result, std::string_view expected) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const auto &diagnostic) { return diagnostic.code == expected; });
}

void positive(std::string source, std::string_view message, bool concurrent = true) {
  const auto result = analyze(std::move(source), concurrent);
  if (!result.semanticValid)
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(result.semanticValid, message);
}

void storageAndMetadata() {
  const auto result = analyze(R"(
thread_local mut int counter = 0;
class Registry {
public:
  static thread_local mut int value = 1;
};
int next() {
  thread_local mut int local = 3;
  local += 1;
  return local;
}
int main() { counter += 1; Registry::value += 1; return next(); }
)");
  expect(result.semanticValid, "TLS mutable namespace, static and block storage is valid concurrently");
  expect(result.semantics.programInitializationPlan().steps.empty(),
         "TLS declarations never enter the eager process initialization plan");
  const auto &plan = result.semantics.threadLocalInitializationPlan();
  expect(plan.steps.size() == 3, "three distinct TLS storage identities are retained");
  for (const auto &step : plan.steps) {
    const auto *symbol = result.semantics.database().findSymbol(step.symbol);
    expect(step.threadLocal && symbol && symbol->threadLocal,
           "every TLS plan step retains an exact thread-local declaration identity");
    expect(step.ensureFailure.empty(), "literal primitive TLS needs no dynamic initialization guard");
  }
  positive(R"(
class Resource {
public:
  Resource() {}
  Resource(Resource&) = delete;
  Resource(Resource&&) = delete;
  ~Resource() {}
};
thread_local Resource resource{};
int main() { return 0; }
)", "an unrelated noncopyable cleanup-owning TLS class is accepted");
  positive("int main() { thread_local mut auto count = 0; count += 1; return count; }",
           "inferred block-local TLS retains ordinary local type inference");
  positive("thread_local mut int count = 1; int main() { return count; }",
           "TLS also works in the single-threaded profile", false);
}

void lifetimeAndEffects() {
  for (const std::string source : {
           "thread_local mut int value = 0; void modify() { value = 2; } int main() { int& borrowed = value; modify(); return borrowed; }",
           "mut int value = 0; void modify() { value = 2; } int main() { mut int& borrowed = value; modify(); return borrowed; }",
           "thread_local mut int value = 0; void modify() { value = 2; } int use(int& input) { modify(); return input; } int main() { return use(value); }",
           "thread_local mut int value = 0; void modify(); int main() { int& borrowed = value; modify(); return borrowed; } void modify() { value = 2; }"}) {
    const auto result = analyze(source, source.starts_with("thread_local"));
    expect(!result.semanticValid && code(result, "GTI-S2045") && !code(result, "GTI-B0001"),
           "transitive storage mutation conflicts with retained or call-scoped loans independently of declaration order");
  }
  positive("thread_local mut int first = 1; thread_local mut int second = 2; void modify() { second += 1; } int main() { int& borrowed = first; modify(); return borrowed; }",
           "a live TLS borrow does not prevent unrelated TLS storage mutation");
  positive("thread_local int value = 1; int read() { return value; } int main() { int& borrowed = value; int copied = read(); return borrowed + copied; }",
           "shared TLS borrows permit independent shared observations");
  positive("int& current() { thread_local mut int value = 1; return value; } int main() { int& borrowed = current(); return borrowed; }",
           "a local TLS declaration outlives its declaring function");
  positive(R"(
#include <std/thread>
thread_local int source = 7;
void joined(int& input) {
  auto result = std::parallel_invoke([&input]() -> void { int copied = input; });
}
int main() { joined(source); return 0; }
)", "a joined scoped worker may borrow TLS while the owner thread and loan remain alive");
}

void invalidDeclarations() {
  for (const std::string source : {
           "class Bad { thread_local mut int field = 0; }; int main() { return 0; }",
           "thread_local constexpr int value = 0; int main() { return value; }",
           "int generic<T>(T input) { thread_local mut int value = 0; return value; } int main() { return generic(1); }",
           "int use(int input) { thread_local int value = input; return value; } int main() { return use(1); }",
           "class Bad { int value = 1; public: int use() { thread_local int copy = this.value; return copy; } }; int main() { return 0; }"}) {
    const auto result = analyze(source);
    expect(!result.semanticValid && code(result, "GTI-S2124") && !code(result, "GTI-B0001"),
           "invalid TLS placement and automatic initializer dependencies are source diagnostics");
  }
  const std::string source = "class Bad { thread_local mut int field = 0; }; int main() { return 0; }";
  const auto invalid = analyze(source);
  const auto diagnostic = std::find_if(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                                       [](const auto &item) { return item.code == "GTI-S2124"; });
  expect(diagnostic != invalid.diagnostics.end() &&
             diagnostic->primary.start == source.find("thread_local") &&
             diagnostic->primary.end == source.find("thread_local") + 12 &&
             !diagnostic->related.empty() && diagnostic->fixes.empty(),
         "TLS contract errors retain the exact keyword, related declaration and no speculative rewrite");
}
}

int main() {
  storageAndMetadata();
  lifetimeAndEffects();
  invalidDeclarations();
  return failures ? 1 : 0;
}
